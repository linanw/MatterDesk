import { MatterError } from "./protocol.js";

const media = (device, args) => ({ ...args, node_id: device.nodeId, endpoint_id: device.endpointId });
const maxCompressed = 5 * 1024 * 1024;
export function snapshotTarget(capabilities) {
  const all = capabilities?.snapshot?.capabilities;
  if (!Array.isArray(all)) return null;
  const choices = all.filter(c => typeof c?.image_codec === "string" && c.image_codec.toUpperCase() === "JPEG")
    .map(c => c.resolution).filter(r => Number.isInteger(r?.width) && Number.isInteger(r?.height)
      && r.width > 0 && r.height > 0 && r.width <= 16384 && r.height <= 16384)
    .sort((a, b) => a.width * a.height - b.width * b.height);
  if (!choices.length) return null;
  const choice = choices.filter(r => r.width <= 640 && r.height <= 480).at(-1) || choices[0];
  const scale = Math.min(1, 640 / choice.width, 360 / choice.height);
  return { width: Math.max(1, Math.round(choice.width * scale)), height: Math.max(1, Math.round(choice.height * scale)) };
}
export function privateCamera(caps) {
  return caps?.privacy?.hard_mode_on === true || caps?.privacy?.soft_livestream_mode_enabled === true;
}
function jpegDimensions(bytes) {
  const y = i => bytes.charCodeAt(i);
  if (y(0) !== 255 || y(1) !== 216) return null;
  for (let i = 2; i + 4 < bytes.length;) {
    if (y(i++) !== 255) return null;
    while (y(i) === 255) i++;
    const marker = y(i++);
    if (marker === 218 || marker === 217) return null;
    if (marker === 1 || marker >= 208 && marker <= 215) continue;
    const len = y(i) * 256 + y(i + 1);
    if (!(len >= 2 && i + len <= bytes.length)) return null;
    if (marker >= 192 && marker <= 207 && ![196, 200, 204].includes(marker) && len >= 8) {
      const height = y(i+3) * 256 + y(i+4), width = y(i+5) * 256 + y(i+6);
      return { width, height };
    }
    i += len;
  }
  return null;
}
export function jpegSnapshot(result) {
  const { data, codec } = result ?? {};
  if (typeof codec !== "string" || codec.toUpperCase() !== "JPEG" || typeof data !== "string" || !data.length ||
      data.length > Math.ceil(maxCompressed / 3) * 4 || !/^[A-Za-z0-9+/]+={0,2}$/.test(data) || data.length % 4 !== 0) return null;
  try {
    const raw = atob(data);
    const size = jpegDimensions(raw);
    if (!size || !size.width || !size.height || size.width > 4096 || size.height > 4096 ||
        size.width * size.height > 12e6 || raw.length > maxCompressed || btoa(raw) !== data) return null;
    return new Blob([Uint8Array.from(raw, c => c.charCodeAt(0))], { type: "image/jpeg" });
  } catch { return null; }
}
const privateError = error => error?.wire?.error_code === 106;
const snapshotError = error => privateError(error) ? "隐私模式已开启" :
  [103, 104].includes(error?.wire?.error_code) || [137, 156].includes(error?.wire?.status) ? "摄像头正忙，稍后再试" :
  error?.wire?.error_code === 105 ? "不支持 JPEG 快照" : "快照暂不可用";

export class Snapshots {
  constructor(client, onUpdate) {
    this.client = client; this.onUpdate = onUpdate;
    this.states = new Map(); this.pending = new Set(); this.wanted = new Map();
    this.pause = false; this.closed = false; this.epoch = client.epoch;
    this.timer = setInterval(() => this.schedule(), 1000);
  }
  demand(devices, paused) {
    if (this.epoch !== this.client.epoch || !this.client.ready) {
      for (const key of this.states.keys()) { this.clear(key); this.onUpdate(key); }
      this.epoch = this.client.epoch;
    }
    this.wanted = new Map(devices.filter(d => d.available && d.camera).slice(0, 16).map(d => [d.key, d]));
    for (const key of this.states.keys()) if (!this.wanted.has(key)) { this.clear(key); this.onUpdate(key); }
    this.pause = paused;
    this.schedule();
  }
  clear(key) {
    const prior = this.states.get(key);
    if (prior?.url) URL.revokeObjectURL(prior.url);
    this.states.delete(key);
  }
  get(key) { return this.states.get(key); }
  schedule() {
    if (this.pause || this.closed || !this.client.ready || this.client.info.schema_version < 14 || this.pending.size >= 2) return;
    for (const [key] of this.wanted) if (!this.pending.has(key) && (!this.states.get(key)?.next || this.states.get(key).next <= Date.now())) {
      this.capture(key);
      if (this.pending.size >= 2) break;
    }
  }
  refresh(key) {
    if (!this.pending.has(key) && this.wanted.has(key)) {
      const prior = this.states.get(key);
      if (prior) prior.next = 0;
      this.schedule();
    }
  }
  async capture(key) {
    const device = this.wanted.get(key), epoch = this.client.epoch;
    if (!device || this.pending.has(key)) return;
    const valid = () => !this.closed && this.client.ready && epoch === this.client.epoch;
    const wanted = () => valid() && !this.pause && this.wanted.has(key);
    const send = (command, args, timeout) => this.client.request(command, media(device, args), timeout);
    this.pending.add(key);
    const old = this.states.get(key);
    this.states.set(key, { ...old, note: old?.url ? "正在更新快照…" : "正在读取快照…" });
    this.onUpdate(key);
    let allocated;
    try {
      const caps = await send("camera_get_capabilities", {}, 15000);
      if (!wanted()) return;
      if (privateCamera(caps)) throw new MatterError("隐私模式已开启", { error_code: 106 });
      const resolution = snapshotTarget(caps);
      if (!resolution) throw new MatterError("不支持 JPEG 快照", { error_code: 105 });
      const result = await send("camera_snapshot", { codec: "JPEG", resolution }, 30000);
      if (result?.provenance === "allocated" && Number.isInteger(result.stream_id) && result.stream_id >= 0 && result.stream_id <= 65535)
        allocated = result.stream_id;
      if (!wanted()) return;
      const image = jpegSnapshot(result);
      if (!image) throw Error("无效的 JPEG 快照");
      this.clear(key);
      this.states.set(key, { url: URL.createObjectURL(image), note: "刚刚更新", captured: Date.now(), next: Date.now() + 30000 });
    } catch (error) {
      if (!wanted()) return;
      if (privateError(error)) this.clear(key);
      this.states.set(key, { ...this.states.get(key), note: snapshotError(error), stale: true, next: Date.now() + 30000 });
    } finally {
      if (allocated !== undefined && valid()) send("camera_release_stream", { kind: "snapshot", stream_id: allocated }, 15000).catch(() => {});
      this.pending.delete(key);
      this.onUpdate(key);
    }
  }
  dispose() { this.closed = true; clearInterval(this.timer); for (const key of this.states.keys()) this.clear(key); }
}

export class BrowserCamera {
  constructor(client, device, video, placeholder, report, microphone) {
    this.client = client; this.device = device; this.video = video; this.epoch = client.epoch;
    if (typeof globalThis.RTCPeerConnection !== "function" || !globalThis.MatterDeskCameraLogic?.CameraPlayer)
      throw new Error("此浏览器不支持 Matter WebRTC 直播。可继续查看 JPEG 快照。");
    const native = { request: (token, command, args) => {
      if (args.client_session_key && command === "camera_start_stream") this.key = args.client_session_key;
      client.request(command, media(device, args), command === "camera_start_stream" ? 45000 : 15000).then(
        result => this.player.response(token, command, true, result, null),
        error => this.player.response(token, command, false, null, error.wire));
    }, playerState: report, microphoneState: microphone, retired: key => { if (this.key === key) this.key = undefined; } };
    this.player = new globalThis.MatterDeskCameraLogic.CameraPlayer({ native, video, placeholder });
    this.player.configure(media(device, {}));
    this.listener = (name, event) => this.player.event(name, event);
    client.cameraListeners.add(this.listener);
    void this.player.controllerConnected(client.ready);
  }
  async close() {
    if (this.closing) return this.closing;
    this.closing = this.player.stop().finally(() => this.dispose());
    return this.closing;
  }
  dispose() { this.client.cameraListeners.delete(this.listener); this.player.dispose(); }
  abandon() {
    // Page departure cannot await WebRTC teardown. Best-effort stop for our own
    // key on the same connection; the controller owns disconnected sessions.
    if (this.key && this.client.ready && this.client.epoch === this.epoch)
      this.client.request("camera_stop_stream", media(this.device, { client_session_key: this.key })).catch(() => {});
    this.dispose();
  }
}
