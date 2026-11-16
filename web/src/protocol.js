const identityKeys = ["fabric_id", "compressed_fabric_id"];
const identity = value => typeof value === "number" && Number.isFinite(value) ||
  typeof value === "string" && !!value.trim();
const id = value => Number.isSafeInteger(value) && value >= 0;
const object = value => !!value && typeof value === "object" && !Array.isArray(value);
const allowed = new Set(["start_listening", "get_nodes", "get_node", "device_command", "write_attribute",
  "commission_with_code", "camera_get_capabilities", "camera_snapshot", "camera_start_stream",
  "camera_stop_stream", "camera_release_stream", "camera_provide_answer", "camera_provide_ice_candidates"]);
export function sameOriginSocket(location) {
  const url = new URL("/ws", location.href);
  if (url.protocol !== "https:" && (url.protocol !== "http:" || !["localhost", "127.0.0.1", "[::1]"].includes(url.hostname)))
    throw new Error("请使用 HTTPS 打开 MatterDesk。");
  url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
  url.username = ""; url.password = "";
  return url.href;
}
export function pairingCode(input) {
  const text = String(input).trim();
  if (/^MT:[0-9A-Z.\-]{3,1021}$/i.test(text)) return "MT:" + text.slice(3).toUpperCase();
  const numeric = text.replace(/[\s-]/g, "");
  if (/^(\d{11}|\d{21})$/.test(numeric)) return numeric;
  throw Error("请输入 11 位或 21 位 Matter 配对码，或 MT: 开头的二维码文本。");
}
export class MatterError extends Error {
  constructor(message, wire = {}) {
    super(message);
    this.name = "MatterError"; this.wire = wire;
  }
}

export class MatterClient {
  constructor({ url, WebSocketImpl = globalThis.WebSocket, notify = () => {}, retry = true } = {}) {
    this.url = url ?? sameOriginSocket(globalThis.location);
    this.WebSocketImpl = WebSocketImpl; this.notify = notify; this.retry = retry;
    this.nodes = new Map(); this.pending = new Map(); this.cameraListeners = new Set();
    this.status = "offline"; this.ready = false; this.info = {}; this.epoch = 0; this.sequence = 0; this.retries = 0;
  }
  change(status) { this.status = status; this.notify(); }
  get profile() {
    const keys = [...identityKeys].reverse();
    for (const key of keys) if (identity(this.info[key])) return key + ":" + String(this.info[key]);
    return null; // Do not share saved rooms across unidentified fabrics.
  }
  connect() {
    if (this.socket) return;
    clearTimeout(this.timer);
    this.wanted = true; this.info = {}; this.nodes.clear(); this.ready = false; ++this.epoch;
    this.change("connecting");
    const socket = this.socket = new this.WebSocketImpl(this.url);
    this.hello = setTimeout(() => { if (socket === this.socket) socket.close(); }, 15000);
    socket.addEventListener("message", event => {
      if (socket !== this.socket || typeof event.data !== "string" || event.data.length > 16 * 1024 * 1024) return;
      try { this.receive(JSON.parse(event.data)); } catch { /* Ignore malformed envelopes. */ }
    });
    socket.addEventListener("error", () => {});
    socket.addEventListener("close", () => { if (socket === this.socket) this.reset(); });
  }
  reset({ permanent = false, reopen = false } = {}) {
    if (permanent) this.wanted = false;
    clearTimeout(this.timer); clearTimeout(this.hello);
    const socket = this.socket; this.socket = null;
    this.ready = false; ++this.epoch; this.info = {}; this.nodes.clear();
    for (const pending of this.pending.values()) { clearTimeout(pending.timer); pending.reject(new MatterError("控制器连接已断开。")); }
    this.pending.clear();
    if (socket && socket.readyState < 2) socket.close();
    this.change(permanent ? "offline" : "disconnected");
    if (!permanent && this.wanted && (reopen || this.retry))
      this.timer = setTimeout(() => this.connect(), reopen ? 0 : Math.min(30000, 1000 * 2 ** Math.min(this.retries++, 5)));
  }
  dispose() { this.reset({ permanent: true }); }
  request(command, args = {}, timeout = 15000) {
    if (!allowed.has(command) || (command !== "start_listening" && !this.ready) || this.socket?.readyState !== 1)
      return Promise.reject(new MatterError("控制器尚未连接或不支持此操作。"));
    if (command.startsWith("camera_") && this.info.schema_version < 14)
      return Promise.reject(new MatterError("此摄像头功能需要 Matter Server 协议 14。"));
    const message_id = "web-" + (++this.sequence);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(message_id); reject(new MatterError("控制器未在时限内响应。")); }, timeout);
      this.pending.set(message_id, { command, args, resolve, reject, timer });
      try { this.socket.send(JSON.stringify({ message_id, command, args })); }
      catch { clearTimeout(timer); this.pending.delete(message_id); reject(new MatterError("无法向控制器发送请求。")); }
    });
  }
  receive(message) {
    if (!object(message)) return;
    if (typeof message.message_id === "string") {
      const pending = this.pending.get(message.message_id);
      if (!pending) return;
      this.pending.delete(message.message_id); clearTimeout(pending.timer);
      if (Object.hasOwn(message, "error_code")) {
        // Keep numeric zero; drop unrelated controller data from this result.
        const { error_code, status, cluster_status, details } = message;
        pending.reject(new MatterError("控制器拒绝此请求。", { error_code, status, cluster_status, details }));
      } else if (!Object.hasOwn(message, "result")) pending.reject(new MatterError("控制器响应缺少结果。"));
      else pending.resolve(message.result);
      return;
    }
    if (message.event) { if (this.ready) this.event(message.event, message.data); return; }
    if (!Number.isInteger(message.schema_version) || this.ready || this.subscribing === this.epoch) return;
    if (message.schema_version < 6 || message.min_supported_schema_version > 14) {
      this.reset({ permanent: true }); this.change("incompatible"); return;
    }
    const epoch = this.epoch;
    this.subscribing = epoch; this.info = message;
    this.request("start_listening").then(result => {
      if (epoch !== this.epoch) return;
      if (!Array.isArray(result)) { this.reset(); return; }
      clearTimeout(this.hello);
      this.setNodes(result); this.ready = true; this.retries = 0; this.change("ready");
    }, () => { if (epoch === this.epoch) this.reset(); });
  }
  setNodes(nodes) { this.nodes.clear(); for (const node of nodes) if (this.validNode(node)) this.nodes.set(node.node_id, node); }
  validNode(node) { return object(node) && id(node.node_id) && object(node.attributes); }
  updateNode(node) { if (this.validNode(node)) { this.nodes.set(node.node_id, node); this.notify(); } }
  async refresh() {
    const epoch = this.epoch;
    const nodes = await this.request("get_nodes");
    if (epoch !== this.epoch || !Array.isArray(nodes)) return;
    this.setNodes(nodes); this.notify();
  }
  async refreshNode(node_id) {
    const epoch = this.epoch;
    const node = await this.request("get_node", { node_id });
    if (epoch === this.epoch) this.updateNode(node);
  }
  event(name, data) {
    if ((name === "webrtc_callback" || name === "camera_session_ended") && object(data)) {
      for (const listener of this.cameraListeners) listener(name, data);
    } else if (name === "node_added" || name === "node_updated") this.updateNode(data);
    else if (name === "node_removed" && id(data)) { this.nodes.delete(data); this.notify(); }
    else if (name === "attribute_updated" && Array.isArray(data) && data.length === 3 && id(data[0]) && /^\d+\/\d+\/\d+$/.test(data[1])) {
      const node = this.nodes.get(data[0]);
      if (node) { node.attributes[data[1]] = data[2]; this.notify(); }
    } else if (name === "endpoint_removed" && id(data?.node_id) && id(data.endpoint_id)) {
      const node = this.nodes.get(data.node_id);
      if (node) { for (const key of Object.keys(node.attributes)) if (key.startsWith(data.endpoint_id + "/")) delete node.attributes[key]; this.notify(); }
    } else if (name === "endpoint_added" && id(data?.node_id)) this.refreshNode(data.node_id).catch(() => {});
    else if (name === "server_info_updated" && object(data)) {
      if (identityKeys.some(key => identity(this.info[key]) && identity(data[key]) && this.info[key] !== data[key])) {
        this.reset({ reopen: true }); return;
      }
      this.info = { ...this.info, ...Object.fromEntries(Object.entries(data).filter(([key, value]) => !identityKeys.includes(key) || identity(value))) };
      this.notify();
    } else if (name === "server_shutdown") this.reset();
  }
  async commission(code) {
    const epoch = this.epoch;
    const result = await this.request("commission_with_code", { code: pairingCode(code), network_only: true }, 300000);
    if (epoch === this.epoch) { if (this.validNode(result)) this.updateNode(result); else await this.refresh(); }
  }
}
