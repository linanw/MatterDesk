import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const scope = { console, setTimeout, clearTimeout };
vm.runInNewContext(fs.readFileSync(new URL('../src/camera/player.js', import.meta.url), 'utf8'), scope);
const { CameraPlayer } = scope.MatterDeskCameraLogic;
const flush = () => new Promise(resolve => setImmediate(resolve));
const deferred = () => { let resolve, reject; const promise = new Promise((yes, no) => { resolve = yes; reject = no; }); return { promise, resolve, reject }; };
const track = kind => ({ kind, stopped: false, stop() { this.stopped = true; } });
class Stream {
  constructor(tracks = []) { this.tracks = tracks; }
  addTrack(t) { this.tracks.push(t); }
  getTracks() { return this.tracks; }
  getAudioTracks() { return this.tracks.filter(t => t.kind === 'audio'); }
}
class Peer {
  constructor() { this.connectionState = 'new'; this.signalingState = 'stable'; this.transceivers = []; this.candidates = []; }
  addTransceiver(kind, options) {
    const tx = { kind, direction: options.direction, currentDirection: options.direction, sender: { replaceTrack: async t => { tx.sentTrack = t; } } };
    this.transceivers.push(tx); return tx;
  }
  async createOffer() { return { type: 'offer', sdp: 'fake-offer' }; }
  async createAnswer() { return { type: 'answer', sdp: 'fake-answer' }; }
  async setLocalDescription(value) { this.localDescription = value; this.signalingState = value.type === 'offer' ? 'have-local-offer' : 'stable'; }
  async setRemoteDescription(value) { this.remoteDescription = value; this.signalingState = value.type === 'answer' ? 'stable' : 'have-remote-offer'; this.connectionState = 'connected'; this.onconnectionstatechange?.(); }
  async addIceCandidate(c) { this.candidates.push(c); }
  close() { this.closed = true; this.connectionState = 'closed'; }
}
const capabilities = { features: ['Video', 'Audio', 'Speaker'], video: { codecs: ['H264'] }, audio: { codecs: ['OPUS'], two_way_talk_support: 'FullDuplex' } };
function fixture(options = {}) {
  const calls = [], states = [], microphone = [], peers = [], retired = [];
  let player, key = 0, acquired = 0;
  const replies = new Map();
  const native = {
    request(token, command, args) {
      calls.push({ token, command, args });
      if (options.defer?.includes(command)) { replies.set(command, (success, result, error) => player.response(token, command, success, result, error)); return; }
      const custom = options.respond?.(command, args);
      if (custom) { queueMicrotask(() => player.response(token, command, custom.success !== false, custom.result ?? null, custom.error ?? {})); return; }
      const result = command === 'camera_get_capabilities' ? capabilities : command === 'camera_start_stream'
        ? { webrtc_session_id: 3, mode: 'provide_offer', talkback: true, video: { stream_id: 7, provenance: 'allocated' }, audio: { stream_id: 8, provenance: 'allocated' } }
        : command === 'camera_stop_stream' ? { ended: true } : null;
      queueMicrotask(() => player.response(token, command, true, result, {}));
    },
    playerState(...args) { states.push(args); },
    microphoneState(state) { microphone.push(state); },
    retired(key) { retired.push(key); }
  };
  const video = { muted: true, srcObject: null, play: async () => {}, pause() {} };
  player = new CameraPlayer({ native, video, makePeer: () => { const pc = options.makePeer?.() || new Peer(); peers.push(pc); return pc; }, makeStream: () => new Stream(),
    makeKey: () => (options.keyPrefix || 'attempt-') + (++key), getMicrophone: () => { ++acquired; return options.getMicrophone?.() || Promise.resolve(new Stream([track('audio')])); } });
  player.configure({ node_id: 1, endpoint_id: 1 });
  return { player, native, calls, states, microphone, peers, replies, video, retired, get acquired() { return acquired; } };
}
async function live(f) {
  await f.player.controllerConnected(true); await f.player.start();
  const a = f.player.attempt;
  f.player.event('webrtc_callback', { node_id: 1, endpoint_id: 1, client_session_key: a.key, webrtc_session_id: 3, event_type: 'answer', data: { sdp: 'fake-answer' } });
  await a.signalChain;
  assert.equal(f.player.state, 'streaming');
  return a;
}

test('initial connection reports idle without a peer and never requests a microphone', async () => {
  const f = fixture(); await f.player.controllerConnected(true);
  assert.equal(f.player.state, 'idle'); assert.equal(f.peers.length, 0); assert.equal(f.acquired, 0); assert.equal(f.player.active(null), false);
  assert.equal(f.calls[0].command, 'camera_get_capabilities');
});

test('offer, early ICE, answer, and session correlation use the actual API shape', async () => {
  const f = fixture(); await f.player.controllerConnected(true); await f.player.start(); const a = f.player.attempt;
  const candidate = { candidate: 'fake-ice', sdpMid: '0', sdpMLineIndex: 0 };
  f.player.event('webrtc_callback', { node_id: 1, endpoint_id: 1, client_session_key: 'stale', event_type: 'answer', data: { sdp: 'wrong-answer' } });
  f.player.event('webrtc_callback', { node_id: 1, endpoint_id: 1, client_session_key: a.key, event_type: 'ice_candidates', data: { ice_candidates: [candidate] } });
  await a.signalChain; assert.equal(a.pc.candidates.length, 0);
  f.player.event('webrtc_callback', { node_id: 1, endpoint_id: 1, client_session_key: a.key, event_type: 'answer', data: { sdp: 'right-answer' } });
  await a.signalChain;
  assert.equal(a.pc.remoteDescription.sdp, 'right-answer'); assert.equal(a.pc.candidates.length, 1);
  const start = f.calls.find(c => c.command === 'camera_start_stream');
  assert.equal(start.args.stream_usage, 'LiveView'); assert.equal(start.args.talkback, true); assert.equal(start.args.allow_eviction, false); assert.equal(f.acquired, 0);
  await f.player.stop();
  assert.equal(a.pc.closed, true); assert.equal(f.calls.filter(c => c.command === 'camera_release_stream').length, 2);
});

test('stop cancels a start that has not yet returned its session id', async () => {
  const f = fixture({ defer: ['camera_start_stream'] }); await f.player.controllerConnected(true);
  const starting = f.player.start(); await flush(); const a = f.player.attempt;
  assert.ok(a.startSent); const stopping = f.player.stop(); await flush();
  assert.equal(a.pc.closed, true);
  assert.equal(f.calls.find(c => c.command === 'camera_stop_stream').args.client_session_key, a.key);
  f.replies.get('camera_start_stream')(false, null, { error_code: 7 }); await starting; await stopping;
  assert.equal(f.player.attempt, null); assert.equal(f.player.state, 'idle');
});

test('late microphone permission result after stop releases every acquired track', async () => {
  const permission = deferred(); const f = fixture({ getMicrophone: () => permission.promise }); await live(f);
  const talking = f.player.setTalk(true); assert.equal(f.acquired, 1);
  const stopping = f.player.stop(); const t = track('audio'); permission.resolve(new Stream([t]));
  await talking; await stopping; assert.equal(t.stopped, true); assert.equal(f.player.attempt, null);
});

test('microphone ownership is established before asynchronous replaceTrack', async () => {
  const sender = deferred(); const t = track('audio'); const f = fixture({ getMicrophone: async () => new Stream([t]) });
  const a = await live(f); a.talkTx.sender.replaceTrack = value => value ? sender.promise : Promise.resolve();
  const talking = f.player.setTalk(true); await flush(); assert.equal(t.stopped, false);
  const stopping = f.player.stop(); assert.equal(t.stopped, true, 'capture must stop synchronously, before replaceTrack settles');
  sender.resolve(); await talking; await stopping;
});

test('disconnect and disposal release microphone and ignore stale events', async () => {
  const t = track('audio'); const f = fixture({ getMicrophone: async () => new Stream([t]) }); const a = await live(f);
  await f.player.setTalk(true); assert.equal(f.player.inspect().micActive, true);
  f.player.dispose(); assert.equal(t.stopped, true); assert.equal(a.pc.closed, true);
  f.player.event('webrtc_callback', { node_id: 1, endpoint_id: 1, client_session_key: a.key, event_type: 'answer', data: { sdp: 'stale' } });
  assert.equal(f.player.attempt, null); assert.equal(f.acquired, 1);
});

test('stop during a pending capability read cannot start media afterward', async () => {
  const f = fixture({ defer: ['camera_get_capabilities'] }); f.player.connected = true;
  const starting = f.player.start(); await flush(); const stopping = f.player.stop();
  f.replies.get('camera_get_capabilities')(true, capabilities, {}); await starting; await stopping;
  assert.equal(f.peers.length, 0); assert.equal(f.player.attempt, null);
});

test('a rejected offer from a stopped attempt cannot poison a later start', async () => {
  const offer = deferred(); let first = true;
  const f = fixture({ makePeer: () => { const pc = new Peer(); if (first) { first = false; pc.createOffer = () => offer.promise; } return pc; } });
  await f.player.controllerConnected(true); const old = f.player.start(); await flush(); await f.player.stop();
  offer.reject(new Error('old-offer-failed')); await old;
  const a = await live(f); assert.equal(f.player.attempt, a); assert.equal(f.player.state, 'streaming'); await f.player.stop();
});

test('an old connection capability reply cannot replace current capabilities', async () => {
  const f = fixture({ defer: ['camera_get_capabilities'] }); f.player.connected = true;
  const old = f.player.loadCapabilities(); const reply = f.replies.get('camera_get_capabilities');
  ++f.player.epoch; f.player.capabilities = { video: { codecs: ['current'] } };
  reply(true, capabilities, {}); await old;
  assert.equal(f.player.capabilities.video.codecs[0], 'current');
});

test('old disconnect cleanup cannot overwrite a rapid reconnect status', async () => {
  const f = fixture(); await f.player.controllerConnected(true);
  const retired = deferred(); f.player.retiring = retired.promise;
  const disconnecting = f.player.controllerConnected(false);
  await f.player.controllerConnected(true);
  const ready = f.player.detail;
  retired.resolve(); await disconnecting;
  assert.equal(f.player.connected, true); assert.equal(f.player.state, 'idle'); assert.equal(f.player.detail, ready);
  assert.ok(f.player.capabilities);
});

test('local ICE waits for both start result and applied remote answer', async () => {
  for (const answerFirst of [true, false]) {
    const f = fixture({ defer: ['camera_start_stream'] }); await f.player.controllerConnected(true);
    const starting = f.player.start(); await flush(); const a = f.player.attempt;
    a.pc.onicecandidate({ candidate: { candidate: 'fake-local-ice', sdpMid: '0', sdpMLineIndex: 0 } });
    assert.equal(f.calls.filter(c => c.command === 'camera_provide_ice_candidates').length, 0);
    const answer = async () => {
      f.player.event('webrtc_callback', { node_id: 1, endpoint_id: 1, client_session_key: a.key, event_type: 'answer', data: { sdp: 'fake-answer' } });
      await a.signalChain;
    };
    const response = () => f.replies.get('camera_start_stream')(true, { webrtc_session_id: 3, talkback: true, video: null, audio: null }, {});
    if (answerFirst) await answer(); else { response(); await starting; }
    assert.equal(f.calls.filter(c => c.command === 'camera_provide_ice_candidates').length, 0, 'a single readiness condition is insufficient');
    if (answerFirst) { response(); await starting; } else await answer();
    await flush();
    const ice = f.calls.filter(c => c.command === 'camera_provide_ice_candidates');
    assert.equal(ice.length, 1); assert.equal(ice[0].args.client_session_key, a.key);
    assert.equal(ice[0].args.ice_candidates[0].candidate, 'fake-local-ice');
    await f.player.stop();
  }
});

test('camera re-offer holds local ICE until its answer command is acknowledged', async () => {
  const f = fixture({ defer: ['camera_provide_answer'] }); const a = await live(f);
  f.player.event('webrtc_callback', { node_id: 1, endpoint_id: 1, client_session_key: a.key, webrtc_session_id: 3, event_type: 'offer', data: { sdp: 'fake-re-offer' } });
  await flush();
  a.pc.onicecandidate({ candidate: { candidate: 'fake-reoffer-ice', sdpMid: '0', sdpMLineIndex: 0 } });
  assert.equal(f.calls.filter(c => c.command === 'camera_provide_ice_candidates').length, 0);
  f.replies.get('camera_provide_answer')(true, null, {}); await a.signalChain;
  assert.equal(f.calls.filter(c => c.command === 'camera_provide_ice_candidates').length, 1);
  await f.player.stop();
});

test('explicit provider busy failures explain concurrent Mini playback without guessing from code zero', () => {
  const f = fixture();
  const busy = '摄像头暂时无法再建立直播连接。请先在 Mini 或其他播放窗口停止直播，再重试。';
  const cases = [
    { error_code: 103 },
    { error_code: 104 },
    { error_code: 0, status: 137 },
    { error_code: 0, status: 156 },
    { error_code: 0, details: 'ProvideOffer refused: ResourcesSessionAlreadyExists' },
    { error_code: 0, details: { message: 'resourcessessionalreadyexists' } },
    { error_code: 0, details: JSON.stringify({ message: 'ResourcesSessionAlreadyExists' }) },
    { error_code: 0, details: { reason: 'Session already exists' } },
    { error_code: 0, details: JSON.stringify({ reason: 'SESSION ALREADY EXISTS' }) }
  ];
  for (const cameraError of cases) assert.equal(f.player.errorText({ cameraError }), busy);
  for (const cameraError of [
    { error_code: 0, details: 'Unrelated provider failure' },
    { error_code: 0, status: 203, details: { message: 'Invalid state' } },
    { error_code: 0, status: '156' },
    { error_code: 0, details: { message: 'ResourcesSessionAlreadyExistsSuffix' } }
  ]) {
    const text = f.player.errorText({ cameraError });
    assert.match(text, /错误 0/);
    assert.doesNotMatch(text, /Mini/);
  }
  assert.match(f.player.errorText({ cameraError: { error_code: 102, details: { reason: 'codec' } } }), /H\.264 \/ Opus/);
  assert.match(f.player.errorText({ cameraError: { error_code: 106, status: 156 } }), /隐私模式/);
});

test('a false response with error zero fails only its own attempt and preserves safe numeric status', async () => {
  for (const status of [137, 156]) {
    const f = fixture({ defer: ['camera_start_stream'] });
    await f.player.controllerConnected(true);
    const starting = f.player.start(); await flush(); const a = f.player.attempt;
    assert.equal(f.calls.find(call => call.command === 'camera_start_stream').args.allow_eviction, false);
    f.replies.get('camera_start_stream')(false, null, {
      error_code: 0, status, cluster_status: 7,
      // This matches the provider's resource refusal, with no session-exists
      // token: the numeric Matter status must survive error_code zero.
      details: status === 137 ? 'Resource exhausted (code 137)' : { message: 'Busy', sdp: 'synthetic-private-signalling' }
    });
    await starting;
    assert.equal(f.player.state, 'error'); assert.equal(f.player.attempt, null); assert.equal(a.pc.closed, true);
    assert.match(f.player.detail, /Mini/); assert.doesNotMatch(f.player.detail, /错误 0/);
    assert.equal(f.calls.filter(call => call.command === 'camera_start_stream').length, 1, 'busy must not trigger automatic retries');
    assert.equal(f.calls.filter(call => call.command === 'camera_release_stream').length, 0, 'a refused start owns no streams');
    const stop = f.calls.find(call => call.command === 'camera_stop_stream');
    assert.equal(stop.args.client_session_key, a.key, 'cleanup may name only this failed attempt');
    const failure = f.player.inspect().lastFailure;
    assert.equal(failure.stage, 'await-start-result'); assert.equal(failure.code, 0);
    assert.equal(failure.status, status); assert.equal(failure.clusterStatus, 7);
    assert.doesNotMatch(JSON.stringify(failure), /synthetic-private-signalling|Resource exhausted|details/);
    assert.equal(f.acquired, 0);
  }
});

test('failure diagnostics accept finite numbers and reject string or nonfinite status data', () => {
  const f = fixture();
  f.player.recordFailure('await-start-result', { cameraError: { error_code: 0, status: 137, cluster_status: 7 } });
  assert.equal(f.player.lastFailure.code, 0); assert.equal(f.player.lastFailure.status, 137); assert.equal(f.player.lastFailure.clusterStatus, 7);
  for (const value of ['156', NaN, Infinity, -Infinity, {}, null]) {
    f.player.recordFailure('await-start-result', { cameraError: { error_code: value, status: value, cluster_status: value } });
    assert.equal(f.player.lastFailure.code, null); assert.equal(f.player.lastFailure.status, null); assert.equal(f.player.lastFailure.clusterStatus, null);
  }
});

test('allocated and reused viewers retire their own session before shared stream release; in-use errors preserve the survivor', async () => {
  const active = new Set(), owned = new Set(['video', 'audio']), order = [], releases = [];
  let starts = 0;
  const respond = (command, args) => {
    if (command === 'camera_start_stream') {
      active.add(args.client_session_key); const provenance = ++starts === 1 ? 'allocated' : 'reused';
      return { result: { webrtc_session_id: starts + 2, mode: 'provide_offer', talkback: true,
        video: { stream_id: 7, provenance }, audio: { stream_id: 8, provenance } } };
    }
    if (command === 'camera_stop_stream') { order.push(['stop', args.client_session_key]); active.delete(args.client_session_key); return { result: { ended: true } }; }
    if (command === 'camera_release_stream') {
      order.push(['release', args.kind]); releases.push({ ...args, inUse: active.size > 0 });
      if (active.size) return { success: false, error: { error_code: 104 } };
      owned.delete(args.kind); return { result: null };
    }
  };
  const first = fixture({ respond, keyPrefix: 'allocated-' }), second = fixture({ respond, keyPrefix: 'reused-' });
  const a = await live(first), b = await live(second);
  assert.equal(active.size, 2); assert.equal(b.result.video.provenance, 'reused');
  await first.player.stop();
  assert.deepEqual(order, [['stop', a.key], ['release', 'video'], ['release', 'audio']]);
  assert.equal(releases.length, 2); assert.ok(releases.every(value => value.inUse));
  assert.equal(owned.size, 2); assert.deepEqual([...active], [b.key]);
  assert.equal(second.player.state, 'streaming'); assert.equal(b.pc.closed, undefined);
  assert.equal(first.player.state, 'idle'); assert.equal(first.player.inspect().lastFailure, null);
  assert.equal(first.calls.filter(call => call.command === 'camera_stop_stream').length, 1);
  assert.equal(first.calls.find(call => call.command === 'camera_stop_stream').args.client_session_key, a.key);
  await second.player.stop();
  assert.deepEqual(order.slice(3), [['stop', b.key], ['release', 'video'], ['release', 'audio']]);
  assert.equal(active.size, 0); assert.equal(owned.size, 0, 'the last reused viewer releases both controller-owned allocations');
  assert.ok(releases.slice(2).every(value => !value.inUse));
  assert.deepEqual(releases.map(value => [value.kind, value.stream_id]), [['video', 7], ['audio', 8], ['video', 7], ['audio', 8]]);
  assert.equal(first.acquired, 0); assert.equal(second.acquired, 0);
});

test('stream retirement releases allocated or reused provenance and never adopted or unknown ownership', async () => {
  for (const provenance of ['allocated', 'reused', 'adopted', 'future-unknown']) {
    const f = fixture({ respond(command) {
      if (command === 'camera_start_stream') return { result: { webrtc_session_id: 3, mode: 'provide_offer', talkback: false,
        video: { stream_id: 21, provenance }, audio: { stream_id: 22, provenance } } };
    } });
    await live(f); await f.player.stop();
    const commands = f.calls.filter(call => ['camera_stop_stream', 'camera_release_stream'].includes(call.command));
    assert.equal(commands[0].command, 'camera_stop_stream');
    const owned = ['allocated', 'reused'].includes(provenance);
    assert.equal(commands.length, owned ? 3 : 1, provenance);
    if (owned) assert.deepEqual(commands.slice(1).map(call => [call.args.kind, call.args.stream_id]), [['video', 21], ['audio', 22]]);
    assert.deepEqual(f.retired, ['attempt-1'], 'native retirement follows all allowed releases, including no releases for adopted streams');
    assert.equal(f.acquired, 0);
  }
});

test('native retirement waits for stop and every stream release attempt, including an in-use response', async () => {
  const f = fixture({ defer: ['camera_stop_stream', 'camera_release_stream'] });
  const a = await live(f); const stopping = f.player.stop(); await flush();
  assert.deepEqual(f.retired, []);
  f.replies.get('camera_stop_stream')(true, { ended: true }, {}); await flush();
  assert.equal(f.calls.filter(call => call.command === 'camera_release_stream').length, 1);
  assert.deepEqual(f.retired, []);
  f.replies.get('camera_release_stream')(false, null, { error_code: 104 }); await flush();
  assert.equal(f.calls.filter(call => call.command === 'camera_release_stream').length, 2);
  assert.deepEqual(f.retired, []);
  f.replies.get('camera_release_stream')(true, null, {}); await stopping;
  assert.deepEqual(f.retired, [a.key]); assert.equal(f.player.state, 'idle');
});

test('a failed stop or page disposal during release retains native cleanup ownership', async () => {
  const failed = fixture({ respond(command) {
    if (command === 'camera_stop_stream') return { success: false, error: { error_code: 0 } };
  } });
  await live(failed); await failed.player.stop();
  assert.deepEqual(failed.retired, []);
  assert.equal(failed.calls.filter(call => call.command === 'camera_release_stream').length, 0);
  const closing = fixture({ defer: ['camera_release_stream'] });
  await live(closing); const stopping = closing.player.stop(); await flush();
  assert.equal(closing.calls.filter(call => call.command === 'camera_release_stream').length, 1);
  closing.player.dispose(); await stopping;
  assert.deepEqual(closing.retired, []);
  assert.equal(closing.calls.filter(call => call.command === 'camera_release_stream').length, 1);
});

test('retirement stays compatible with a native adapter without a completion hook', async () => {
  const f = fixture(); delete f.native.retired;
  await live(f); await f.player.stop();
  assert.equal(f.player.state, 'idle');
  assert.equal(f.calls.filter(call => call.command === 'camera_release_stream').length, 2);
});

test('a cancelled pending start releases its late reused allocations after its own stop acknowledgement', async () => {
  const f = fixture({ defer: ['camera_start_stream', 'camera_stop_stream'] });
  await f.player.controllerConnected(true); const starting = f.player.start(); await flush(); const a = f.player.attempt;
  const stopping = f.player.stop(); await flush();
  f.replies.get('camera_stop_stream')(true, { ended: true }, {}); await flush();
  assert.equal(f.calls.filter(call => call.command === 'camera_release_stream').length, 0);
  f.replies.get('camera_start_stream')(true, { webrtc_session_id: 3, talkback: false,
    video: { stream_id: 31, provenance: 'reused' }, audio: { stream_id: 32, provenance: 'adopted' } }, {});
  await starting; await stopping;
  const released = f.calls.filter(call => call.command === 'camera_release_stream');
  assert.equal(released.length, 1); assert.equal(released[0].args.kind, 'video'); assert.equal(released[0].args.stream_id, 31);
  assert.equal(f.calls.find(call => call.command === 'camera_stop_stream').args.client_session_key, a.key);
  assert.equal(f.player.attempt, null); assert.equal(f.player.state, 'idle'); assert.equal(f.acquired, 0);
});
