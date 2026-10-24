"use strict";
(function (scope) {
  // No media, SDP or ICE content is written to the console or application log.
  class CameraPlayer {
    constructor(options) {
      this.native = options.native;
      this.video = options.video;
      this.placeholder = options.placeholder || { textContent: "", classList: { toggle() {} } };
      this.makePeer = options.makePeer || (() => new RTCPeerConnection({ iceServers: [] }));
      this.makeStream = options.makeStream || (() => new MediaStream());
      this.getMicrophone = options.getMicrophone || (() => navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true }, video: false }));
      this.makeKey = options.makeKey || (() => crypto.randomUUID());
      this.config = null; this.capabilities = null; this.connected = false; this.epoch = 0;
      this.sequence = 0; this.pending = new Map(); this.attempt = null;
      this.retiring = Promise.resolve(); this.sound = false; this.disposed = false; this.intent = 0; this.starting = false;
      this.state = "idle"; this.detail = "点击「开始直播」连接摄像头。";
      this.lastFailure = null;
      this.lastRequest = null;
    }
    configure(config) { this.config = config; }
    async controllerConnected(connected) {
      if (this.connected === connected) return;
      this.connected = connected;
      if (!connected) {
        const epoch = ++this.epoch; this.capabilities = null;
        this.rejectPending("控制器连接已断开");
        await this.stop("控制器已断开，重新连接后可再次开始直播。", false);
        if (!this.connected && epoch === this.epoch) this.report("disconnected", "控制器已断开，麦克风与视频连接已释放。");
      } else {
        ++this.intent;
        this.report("idle", "正在读取摄像头能力…");
        await this.loadCapabilities();
      }
    }
    async loadCapabilities() {
      const epoch = this.epoch;
      try {
        const result = await this.request("camera_get_capabilities", {});
        if (!this.connected || this.disposed || epoch !== this.epoch) return;
        this.capabilities = result;
        if (!this.attempt && !this.starting) this.report("idle", "摄像头已就绪，点击「开始直播」。收听和麦克风默认关闭。");
      } catch (_) {
        if (this.connected && !this.disposed && epoch === this.epoch && !this.attempt) this.report("error", "无法读取摄像头能力，请确认节点在线后重试。");
      }
    }
    request(command, args) {
      if (!this.connected || !this.config || this.disposed) return Promise.reject(new Error("not-connected"));
      const token = "camera-" + (++this.sequence);
      return new Promise((resolve, reject) => {
        this.pending.set(token, { resolve, reject });
        this.native.request(token, command, args);
      });
    }
    response(token, _command, success, result, error) {
      const pending = this.pending.get(token);
      if (!pending) return;
      this.pending.delete(token);
      if (success) pending.resolve(result);
      else pending.reject(Object.assign(new Error("camera-command-failed"), { cameraError: error || {} }));
    }
    rejectPending(reason) {
      for (const pending of this.pending.values()) pending.reject(new Error(reason));
      this.pending.clear();
    }
    active(attempt) { return Boolean(attempt && this.attempt === attempt && attempt.epoch === this.epoch && attempt.intent === this.intent && !this.disposed); }
    feature(name) {
      return !Array.isArray(this.capabilities?.features) || this.capabilities.features.includes(name);
    }
    async start() {
      if (this.attempt || this.starting || this.disposed || !this.connected) return;
      this.starting = true; const intent = ++this.intent; let ownAttempt = null;
      let stage = "await-cleanup";
      this.report("connecting", "正在建立加密视频连接…");
      try {
        await this.retiring;
        if (this.disposed || !this.connected || this.attempt || intent !== this.intent) return;
        stage = "read-capabilities";
        if (!this.capabilities) await this.loadCapabilities();
        if (!this.capabilities || !this.connected || this.disposed || intent !== this.intent) return;
        this.report("connecting", "正在建立加密视频连接…");
        const codecs = this.capabilities.video?.codecs || [];
        if (!this.feature("Video") || (codecs.length && !codecs.some(c => String(c).toUpperCase() === "H264"))) {
          this.report("error", "此摄像头没有提供播放器支持的 H.264 视频。"); return;
        }
        const audioCodecs = this.capabilities.audio?.codecs || [];
        const receiveAudio = this.feature("Audio") && audioCodecs.some(c => String(c).toUpperCase() === "OPUS");
        const talkMode = this.capabilities.audio?.two_way_talk_support;
        const canTalk = receiveAudio && this.feature("Speaker") && ["HalfDuplex", "FullDuplex"].includes(talkMode);
        this.lastRequest = { audio: receiveAudio, talkback: canTalk, video: true };
        stage = "create-peer"; const pc = this.makePeer();
        const a = { key: this.makeKey(), epoch: this.epoch, intent, pc, stream: this.makeStream(),
          audio: receiveAudio, talkMode: canTalk ? talkMode : null, talkAllowed: false, talkTx: null,
          mic: null, micWanted: false, micGeneration: 0, starting: null, startSent: false,
          result: null, localCandidates: [], remoteCandidates: [], localIceSending: false,
          signallingReady: false, signalChain: Promise.resolve(), closed: false };
        this.attempt = a; ownAttempt = a;
        stage = "add-transceivers"; pc.addTransceiver("video", { direction: "recvonly" });
        if (receiveAudio) a.talkTx = pc.addTransceiver("audio", { direction: canTalk ? "sendrecv" : "recvonly" });
        pc.onicecandidate = ev => {
          if (!this.active(a) || !ev.candidate) return;
          const c = ev.candidate;
          const wire = { candidate: c.candidate, sdpMid: c.sdpMid, sdpMLineIndex: c.sdpMLineIndex };
          a.localCandidates.push(wire);
          this.flushLocalIce(a);
        };
        pc.ontrack = ev => {
          if (!this.active(a)) return;
          if (!a.stream.getTracks().includes(ev.track)) a.stream.addTrack(ev.track);
          this.video.srcObject = a.stream;
          this.applyMute(a);
          const playing = this.video.play();
          if (playing?.catch) playing.catch(() => { if (this.active(a)) this.report(this.state, "视频已连接，播放被系统阻止，请重新开始直播。"); });
        };
        pc.onconnectionstatechange = () => {
          if (!this.active(a)) return;
          if (pc.connectionState === "connected") this.report("streaming", "直播已连接 · 收听和对讲可单独开启");
          else if (pc.connectionState === "failed" || pc.connectionState === "closed") this.fail(a, "视频连接已中断，请重新开始直播。");
          else if (pc.connectionState === "disconnected") this.report("connecting", "视频网络暂时中断，正在等待恢复…");
        };
        stage = "create-offer"; const offer = await pc.createOffer();
        if (!this.active(a)) return;
        stage = "set-local-description"; await pc.setLocalDescription(offer);
        if (!this.active(a)) return;
        const sdp = pc.localDescription?.sdp;
        if (!sdp) throw new Error("empty-offer");
        stage = "send-start"; a.startSent = true;
        a.starting = this.request("camera_start_stream", { stream_usage: "LiveView", sdp,
          client_session_key: a.key, video: { codecs: ["H264"] },
          audio: receiveAudio ? { codecs: ["OPUS"] } : false,
          allow_eviction: false, ...(canTalk ? { talkback: true } : {}) }).then(result => {
            a.result = result; a.talkAllowed = Boolean(result?.talkback);
            if (result?.audio === null) a.audio = false;
            return result;
          });
        stage = "await-start-result"; await a.starting;
        if (!this.active(a)) return;
        await this.flushLocalIce(a);
        this.report(this.state, this.detail);
      } catch (error) {
        this.recordFailure(stage, error);
        if (ownAttempt && this.active(ownAttempt)) await this.fail(ownAttempt, this.errorText(error));
        else if (!ownAttempt && intent === this.intent && !this.disposed && this.connected) this.report("error", this.errorText(error));
      } finally {
        this.starting = false;
      }
    }
    errorText(error) {
      const failure = error?.cameraError;
      const rawDetails = failure?.details;
      let details = rawDetails;
      if (typeof details === "string") { try { details = JSON.parse(details); } catch (_) { details = null; } }
      const code = failure?.error_code;
      // Some providers return an untyped error (code 0) when they cannot open
      // another WebRTC session. Keep the device's explicit busy/status evidence;
      // a nonempty session list alone does not prove that multi-view is refused.
      const detailTexts = [typeof rawDetails === "string" ? rawDetails : null, details?.message, details?.reason];
      const sessionExists = detailTexts.some(text => typeof text === "string"
        && /\bResourcesSessionAlreadyExists\b|\bsession\s+already\s+exists\b/i.test(text.slice(0, 2048)));
      if (code === 106) return "摄像头隐私模式已开启，请先在摄像头中关闭对应隐私开关。";
      if (code === 103 || code === 104 || failure?.status === 137 || failure?.status === 156 || sessionExists)
        return "摄像头暂时无法再建立直播连接。请先在 Mini 或其他播放窗口停止直播，再重试。";
      if (code === 102 || details?.reason === "codec") return "摄像头与播放器的媒体参数不兼容（需要 H.264 / Opus）。";
      return Number.isFinite(code) ? `摄像头请求失败（错误 ${Number(code)}），请确认节点和视频源在线后重试。` : "摄像头连接失败，请确认视频源在线后重试。";
    }
    recordFailure(stage, error) {
      // Diagnostics contain no signalling payload: only the browser's first
      // explanatory line, with SDP/candidate text and network addresses removed.
      const message = String(error?.message || "").split(/[\r\n]/)[0]
        .replace(/(?:candidate:|v=0|a=ice-|a=fingerprint:|MT:).*$/gi, "[redacted]")
        .replace(/(?:\d{1,3}\.){3}\d{1,3}/g, "[address]")
        .replace(/\b[0-9a-f]{0,4}:[0-9a-f:]+\b/gi, "[address]").slice(0, 220);
      this.lastFailure = { stage, name: String(error?.name || "Error").slice(0, 40), message,
        code: Number.isFinite(error?.cameraError?.error_code) ? error.cameraError.error_code : null,
        status: Number.isFinite(error?.cameraError?.status) ? error.cameraError.status : null,
        clusterStatus: Number.isFinite(error?.cameraError?.cluster_status) ? error.cameraError.cluster_status : null };
    }
    async flushLocalIce(a) {
      // ProvideOffer can answer before the camera has constructed its media
      // session. Its SDP callback is the readiness boundary for local trickle ICE.
      if (!this.active(a) || !a.result || !a.signallingReady || a.localIceSending || !a.localCandidates.length) return;
      a.localIceSending = true;
      const candidates = a.localCandidates.splice(0);
      try { await this.request("camera_provide_ice_candidates", { client_session_key: a.key, ice_candidates: candidates }); }
      catch (error) { this.recordFailure("local-ice", error); if (this.active(a)) await this.fail(a, "无法交换视频网络候选地址，请重新开始直播。"); }
      finally {
        a.localIceSending = false;
        if (this.active(a) && a.localCandidates.length) this.flushLocalIce(a);
      }
    }
    event(name, event) {
      const a = this.attempt;
      if (!a || !this.config || String(event?.node_id) !== String(this.config.node_id) || event.endpoint_id !== this.config.endpoint_id || event.client_session_key !== a.key) return;
      if (name === "camera_session_ended" || (name === "webrtc_callback" && event.event_type === "end")) {
        this.stop("摄像头已结束本次直播，麦克风已关闭。").catch(() => {}); return;
      }
      if (name !== "webrtc_callback") return;
      a.signalChain = a.signalChain.then(async () => {
        if (!this.active(a)) return;
        const data = event.data || {};
        if (event.event_type === "answer") {
          if (a.pc.signalingState !== "have-local-offer") return;
          await a.pc.setRemoteDescription({ type: "answer", sdp: data.sdp });
          if (!this.active(a)) return;
          a.signallingReady = true;
          await this.flushRemoteIce(a);
          await this.flushLocalIce(a);
          if (this.active(a) && a.pc.connectionState === "connected") this.report("streaming", "直播已连接 · 收听和对讲可单独开启");
        } else if (event.event_type === "offer") {
          if (a.pc.signalingState !== "stable") return;
          a.signallingReady = false;
          await a.pc.setRemoteDescription({ type: "offer", sdp: data.sdp });
          if (!this.active(a)) return;
          const answer = await a.pc.createAnswer();
          await a.pc.setLocalDescription(answer);
          if (!this.active(a)) return;
          await this.request("camera_provide_answer", { webrtc_session_id: event.webrtc_session_id, sdp: a.pc.localDescription.sdp });
          if (!this.active(a)) return;
          a.signallingReady = true;
          await this.flushRemoteIce(a);
          await this.flushLocalIce(a);
        } else if (event.event_type === "ice_candidates") {
          const candidates = data.ice_candidates || [];
          if (!a.pc.remoteDescription) a.remoteCandidates.push(...candidates);
          else for (const candidate of candidates) { if (this.active(a)) await a.pc.addIceCandidate(candidate); }
        }
      }).catch(error => { this.recordFailure("remote-" + event.event_type, error); if (this.active(a)) this.fail(a, "视频信令处理失败，请重新开始直播。"); });
    }
    async flushRemoteIce(a) {
      for (const candidate of a.remoteCandidates.splice(0)) if (this.active(a)) await a.pc.addIceCandidate(candidate);
    }
    applyMute(a) { this.video.muted = !this.sound || Boolean(a?.mic && a.talkMode === "HalfDuplex"); }
    setSound(enabled) { this.sound = Boolean(enabled); this.applyMute(this.attempt); }
    negotiatedTalk(a) {
      const direction = a?.talkTx?.currentDirection;
      return Boolean(a?.talkAllowed && a.talkMode && ["sendrecv", "sendonly"].includes(direction));
    }
    async setTalk(enabled) {
      const a = this.attempt;
      if (!enabled) { if (a) await this.releaseMicrophone(a); return; }
      if (!a || !this.active(a) || this.state !== "streaming" || !this.negotiatedTalk(a)) { this.native.microphoneState("off"); return; }
      if (a.micWanted) return;
      a.micWanted = true; const generation = ++a.micGeneration;
      this.native.microphoneState("requesting");
      let stream;
      try {
        stream = await this.getMicrophone();
        if (!this.active(a) || !a.micWanted || generation !== a.micGeneration) { for (const t of stream.getTracks()) t.stop(); return; }
        const track = stream.getAudioTracks()[0];
        if (!track) throw new Error("no-microphone-track");
        a.mic = stream; this.applyMute(a); this.native.microphoneState("active");
        await a.talkTx.sender.replaceTrack(track);
        if (!this.active(a) || !a.micWanted || generation !== a.micGeneration) { for (const t of stream.getTracks()) t.stop(); await a.talkTx.sender.replaceTrack(null); return; }
        track.onended = () => { if (this.active(a)) this.releaseMicrophone(a); };
        this.native.microphoneState("active"); this.report(this.state, "正在双向对讲 · 停止对讲会立即释放麦克风");
      } catch (error) {
        this.recordFailure("microphone", error);
        if (stream) for (const track of stream.getTracks()) track.stop();
        if (a.mic === stream) a.mic = null;
        if (!this.active(a) || generation !== a.micGeneration) return;
        a.micWanted = false; this.native.microphoneState("denied"); this.report(this.state, "麦克风未授权或不可用，视频和收听仍可继续。");
      }
    }
    async releaseMicrophone(a) {
      a.micWanted = false; ++a.micGeneration;
      const stream = a.mic; a.mic = null;
      if (stream) for (const track of stream.getTracks()) { track.onended = null; track.stop(); }
      this.native.microphoneState("off"); this.applyMute(this.attempt);
      try { if (a.talkTx) await a.talkTx.sender.replaceTrack(null); } catch (_) {}
      if (this.active(a)) this.report(this.state, "直播已连接 · 麦克风关闭");
    }
    closeLocal(a) {
      if (a.closed) return; a.closed = true;
      // Stop tracks synchronously, before waiting for any network operation.
      this.releaseMicrophone(a).catch(() => {});
      a.pc.ontrack = null; a.pc.onicecandidate = null; a.pc.onconnectionstatechange = null;
      a.pc.close();
      for (const track of a.stream.getTracks()) track.stop();
      if (this.video.srcObject === a.stream) { this.video.pause(); this.video.srcObject = null; }
    }
    async retire(a, sendStop) {
      this.closeLocal(a);
      if (!sendStop || !a.startSent || !this.connected || a.epoch !== this.epoch || this.disposed) return;
      let stopped = false;
      try { await this.request("camera_stop_stream", { client_session_key: a.key }); stopped = true; } catch (_) {}
      if (a.starting) { try { await a.starting; } catch (_) {} }
      if (!stopped || !this.connected || a.epoch !== this.epoch || this.disposed) return;
      for (const kind of ["video", "audio"]) {
        const track = a.result?.[kind];
        // Reused streams are known to belong to this controller too. A device
        // refuses deallocation while another session still references them.
        if (!["allocated", "reused"].includes(track?.provenance)) continue;
        try { await this.request("camera_release_stream", { kind, stream_id: track.stream_id }); } catch (_) {}
      }
      // Native cleanup must survive a page closed between stop and release.
      if (this.connected && a.epoch === this.epoch && !this.disposed) this.native.retired?.(a.key);
    }
    async stop(message = "直播已停止，麦克风已关闭。", sendStop = true) {
      const intent = ++this.intent;
      const a = this.attempt; this.attempt = null;
      if (a) {
        this.report("stopping", "正在结束摄像头会话，麦克风已关闭…");
        const retire = this.retire(a, sendStop);
        this.retiring = Promise.all([this.retiring, retire]).then(() => undefined, () => undefined);
      }
      await this.retiring;
      if (!this.disposed && !this.attempt && intent === this.intent) this.report("idle", message);
    }
    async fail(a, detail) {
      if (!this.active(a)) return;
      await this.stop(detail);
      if (!this.disposed && !this.attempt && this.connected) this.report("error", detail);
    }
    report(state, detail) {
      this.state = state; this.detail = detail;
      const a = this.attempt;
      const talk = this.active(a) && this.negotiatedTalk(a);
      this.placeholder.textContent = detail;
      this.placeholder.classList.toggle("hidden", state === "streaming");
      this.native.playerState(state, detail, Boolean(talk), Boolean(a?.mic), Boolean(a?.audio));
    }
    dispose() {
      // Native CameraBridge independently cancels by key, surviving this page.
      this.disposed = true; ++this.epoch;
      if (this.attempt) this.closeLocal(this.attempt);
      this.attempt = null; this.rejectPending("player-closed");
      this.native.microphoneState("off");
    }
    inspect() {
      const a = this.attempt;
      return { state: this.state, connected: this.connected, peerState: a?.pc.connectionState || "closed",
        micActive: Boolean(a?.mic), talkAvailable: this.negotiatedTalk(a), audioAvailable: Boolean(a?.audio),
        videoWidth: this.video.videoWidth || 0, videoHeight: this.video.videoHeight || 0,
        receivedVideoFrames: this.video.getVideoPlaybackQuality?.().totalVideoFrames || 0,
        remoteAudioTracks: a?.stream.getAudioTracks().length || 0, lastFailure: this.lastFailure,
        capabilitiesType: typeof this.capabilities,
        capabilityFeatures: Array.isArray(this.capabilities?.features) ? this.capabilities.features.map(String) : [],
        videoCodecs: Array.isArray(this.capabilities?.video?.codecs) ? this.capabilities.video.codecs.map(String) : [],
        audioCodecs: Array.isArray(this.capabilities?.audio?.codecs) ? this.capabilities.audio.codecs.map(String) : [],
        twoWayTalkSupport: typeof this.capabilities?.audio?.two_way_talk_support === "string" ? this.capabilities.audio.two_way_talk_support : null,
        lastRequest: this.lastRequest };
    }
    async sampleStats() {
      const summary = { videoBytes: 0, audioBytes: 0, videoFramesDecoded: 0, audioPackets: 0 };
      if (!this.attempt) return summary;
      const stats = await this.attempt.pc.getStats();
      stats.forEach(report => {
        if (report.type !== "inbound-rtp") return;
        if (report.kind === "video") { summary.videoBytes += report.bytesReceived || 0; summary.videoFramesDecoded += report.framesDecoded || 0; }
        if (report.kind === "audio") { summary.audioBytes += report.bytesReceived || 0; summary.audioPackets += report.packetsReceived || 0; }
      });
      return summary;
    }
  }
  scope.MatterDeskCameraLogic = { CameraPlayer };
  if (typeof document !== "undefined" && typeof QWebChannel !== "undefined") {
    new QWebChannel(qt.webChannelTransport, channel => {
      const native = channel.objects.cameraBridge;
      const player = new CameraPlayer({ native, video: document.getElementById("video"), placeholder: document.getElementById("placeholder") });
      native.configured.connect(config => player.configure(config));
      native.response.connect((...args) => player.response(...args));
      native.serverEvent.connect((...args) => player.event(...args));
      native.controllerState.connect(connected => { player.controllerConnected(connected).catch(() => {}); });
      native.action.connect((name, enabled) => {
        if (name === "start") player.start().catch(() => {});
        else if (name === "stop") player.stop().catch(() => {});
        else if (name === "sound") player.setSound(enabled);
        else if (name === "talk") player.setTalk(enabled).catch(() => {});
      });
      scope.MatterDeskCamera = { dispose: () => player.dispose(), inspect: () => player.inspect(), sampleStats: () => player.sampleStats(),
        setPresentation: config => {
          const fill = config?.embedded === true && config?.fit !== true;
          document.getElementById("video").style.objectFit = fill ? "cover" : "contain";
          // The camera viewer supplies status in its native overlays.
          document.getElementById("placeholder").style.display = fill ? "none" : "";
        } };
      native.ready();
    });
  }
})(globalThis);
