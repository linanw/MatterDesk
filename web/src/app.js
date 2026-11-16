import { MatterClient, sameOriginSocket } from "./protocol.js";
import { devicesFromNodes, controlRequest, writeSucceeded, formatState, kinds, labels, modes, readings } from "./devices.js";
import { BrowserCamera, Snapshots } from "./camera.js";

const $ = id => document.getElementById(id);
const node = (name, className, text) => {
  const el = document.createElement(name);
  if (className) el.className = className;
  if (text != null) el.textContent = text;
  return el;
};
const button = (name, fn, className) => { const el = node("button", className, name); el.type = "button"; el.addEventListener("click", fn); return el; };
const option = (key, label) => { const e = node("option", null, label); e.value = key; return e; };
const statusText = { ready: "控制器在线", connecting: "正在连接控制器…", disconnected: "控制器未连接", incompatible: "控制器协议不兼容", offline: "未连接" };
const titles = { home: "家庭总览", favorites: "收藏", cameras: "摄像头" };
const symbols = {
  light: "M7 18h10M9 22h6M9 15v-2a5 5 0 1 1 6 0v2z", outlet: "M8 2v5M16 2v5M6 7h12v2a6 6 0 0 1-12 0zM12 15v7",
  switch: "M12 3v9M7.1 6.4a8 8 0 1 0 9.8 0", sensor: "M2 12h4l3-6 4 12 3-6h6",
  camera: "M3 6h13v12H3zM16 10l5-3v10l-5-3", lock: "M5 10h14v11H5zM8 10V7a4 4 0 0 1 8 0v3M12 14v3",
  cover: "M3 3h18v18H3zM7 3v18M12 3v18M17 3v18", thermostat: "M10 3a2 2 0 0 1 4 0v10a5 5 0 1 1-4 0zM12 9v8",
  fan: "M12 12c3-7 7-7 8-3s-2 4-8 3c7 3 7 7 3 8s-4-2-3-8c-3 7-7 7-8 3s2-4 8-3c-7-3-7-7-3-8s4 2 3 8z",
  device: "M3 7l9-4 9 4v10l-9 4-9-4zM3 7l9 5 9-5M12 12v9"
};
function icon(kind) {
  const outer = node("span", "icon");
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", "0 0 24 24"); svg.setAttribute("aria-hidden", "true");
  const shape = document.createElementNS(svg.namespaceURI, "path"); shape.setAttribute("d", symbols[kind] || symbols.device);
  svg.append(shape); outer.append(svg); return outer;
}
let client, snapshots, transportError = "", devices = [], profile = null, selected = null, roomEdit = null, view = "home", camera = null, cameraLive = false;
let prefs = { rooms: [], devices: {} }, feedbackTimer, pairBusy = false, closeBusy = false;
const pending = new Set(), cardImages = new Map(), visibleCards = new Set(), controlRows = new Map();
const keyFor = id => "matterdesk.web.home.v1:" + id;
function loadPrefs() {
  prefs = { rooms: [], devices: {} };
  if (!profile) return;
  try {
    const stored = JSON.parse(localStorage.getItem(keyFor(profile)));
    if (stored && Array.isArray(stored.rooms) && stored.devices && typeof stored.devices === "object") {
      prefs = { rooms: stored.rooms.filter(r => typeof r.id === "string" && typeof r.name === "string"), devices: stored.devices };
    }
  } catch { /* Private or unavailable browser storage. */ }
}
function savePrefs() {
  if (!profile) return;
  try { localStorage.setItem(keyFor(profile), JSON.stringify(prefs)); }
  catch { notice("浏览器无法保存设置；本次页面仍可使用。", true); }
}
const prefsFor = d => prefs.devices[d.key] || {};
const display = d => prefsFor(d).name || d.name;
const current = () => devices.find(d => d.key === selected);
const roomName = i => prefs.rooms.find(r => r.id === i)?.name || "未分配房间";
function notice(message) {
  const n = $("notice"); n.hidden = !message; n.textContent = message;
  clearTimeout(feedbackTimer); feedbackTimer = setTimeout(() => { n.hidden = true; }, 4100);
}
function feedback(id, message, fail = false) {
  $(id).textContent = message; $(id).dataset.error = fail;
}
const observer = typeof IntersectionObserver === "function" ? new IntersectionObserver(entries => {
  for (const entry of entries) {
    if (!entry.target.isConnected) continue;
    const key = entry.target.dataset.key;
    if (entry.isIntersecting) visibleCards.add(key); else visibleCards.delete(key);
  }
  updateDemand();
}) : null;
function updateDemand() {
  if (!snapshots) return;
  const detail = current();
  const targets = detail && $("details").open ? [detail] : devices.filter(d => visibleCards.has(d.key));
  snapshots.demand(targets, document.hidden || cameraLive);
}
function applySnapshot(img, placeholder, state, fallback) {
  if (state?.url) {
    if (img.getAttribute("src") !== state.url) img.src = state.url;
    img.hidden = false; placeholder.hidden = true;
  } else {
    img.hidden = true; img.removeAttribute("src"); placeholder.hidden = false; placeholder.textContent = state?.note || fallback;
  }
}
function snapshotChanged(key) {
  const state = snapshots.get(key);
  const tile = cardImages.get(key);
  if (tile) {
    applySnapshot(tile.img, tile.note, state, client?.info.schema_version < 14 ? "快照需要协议 14" : "快照暂不可用");
    tile.status.textContent = state?.stale && state.url ? "上次快照 · " + state.note : state?.note || "";
  }
  if (key === selected && !cameraLive && $("details").open) {
    applySnapshot($("camera-image"), $("camera-placeholder"), state, "等待快照");
    $("camera-note").textContent = state?.stale && state.url ? "上次快照 · " + state.note : state?.note || "";
  }
}
function renderRooms() {
  const roomList = $("rooms"); roomList.replaceChildren();
  for (const room of prefs.rooms) {
    const row = node("div", "room-row");
    const go = button(room.name, () => setView(room.id), "nav");
    go.setAttribute("aria-pressed", String(view === room.id));
    const edit = button("⋯", () => openRoom(room), "small-button");
    edit.setAttribute("aria-label", "编辑房间：" + room.name);
    row.append(go, edit); roomList.append(row);
  }
  $("room-add").disabled = !profile || prefs.rooms.length >= 40;
  for (const nav of document.querySelectorAll("button[data-view]")) nav.setAttribute("aria-pressed", String(view === nav.dataset.view));
}
function setView(next) { view = next; render(); }
function visible(d) {
  const query = $("search").value.trim().toLocaleLowerCase("zh-CN");
  const kind = $("kind").value, presence = $("availability").value;
  if (view === "favorites" && !prefsFor(d).favorite || view === "cameras" && !d.camera ||
      !Object.hasOwn(titles, view) && prefsFor(d).room !== view) return false;
  if (kind && d.kind !== kind || presence && d.available !== (presence === "online")) return false;
  return !query || [display(d), d.name, d.nodeName, roomName(prefsFor(d).room)].some(name => name.toLocaleLowerCase("zh-CN").includes(query));
}
function buildCard(device) {
  const tile = node("article", "card"); tile.dataset.key = device.key;
  const heading = node("div", "card-head");
  const label = node("div");
  label.append(node("h3", undefined, display(device)), node("p", "tag", kinds[device.kind] + " · " + roomName(prefsFor(device).room)));
  const favorite = !!prefsFor(device).favorite;
  const star = button(favorite ? "★" : "☆", () => {
    prefs.devices[device.key] = { ...prefsFor(device), favorite: !favorite }; savePrefs(); render();
  }, "star");
  star.disabled = !profile; star.setAttribute("aria-label", "收藏：" + display(device)); star.setAttribute("aria-pressed", String(favorite));
  heading.append(icon(device.kind), label, star); tile.append(heading);
  if (device.camera) {
    const preview = node("div", "preview"); const img = node("img"); img.alt = "摄像头 JPEG 快照"; img.hidden = true;
    const note = node("span", undefined, device.available ? "等待快照" : "摄像头离线"); preview.append(img, note); tile.append(preview);
    const status = node("span", "muted"); tile.append(status); cardImages.set(device.key, { img, note, status });
  }
  const state = node("p", device.available ? "status" : "status offline", device.available ? "● 在线" : "○ 离线");
  if (device.state.power !== undefined) state.append(" · " + formatState("power", device.state.power));
  else if (device.state.lockState) state.append(" · " + device.state.lockState);
  tile.append(state);
  const values = readings(device);
  if (values.length) { const row = node("div", "card-readings"); for (const reading of values.slice(0, 3)) row.append(node("span", null, reading)); tile.append(row); }
  const actions = node("div", "card-actions");
  if (device.controls.power) {
    const power = button(device.state.power === true ? "关闭" : "开启", () => sendControl(device.key, "power", device.state.power !== true));
    power.disabled = !device.available || device.state.power === undefined || pending.has(device.key + ":power");
    power.setAttribute("aria-label", power.textContent + "：" + display(device)); actions.append(power);
  }
  const detail = button(device.camera ? "查看摄像头" : "配件详情", () => openDetails(device.key));
  detail.setAttribute("aria-label", "查看详情：" + display(device));
  actions.append(detail); tile.append(actions); return tile;
}
function renderGrid() {
  const matching = devices.filter(visible);
  $("match-count").textContent = client?.ready ? matching.length + " 个配件" : "";
  const list = $("cards");
  const active = document.activeElement;
  const focused = list.contains(active) ? [active.closest("article")?.dataset.key, active.getAttribute("aria-label")] : null;
  observer?.disconnect(); cardImages.clear(); visibleCards.clear(); list.replaceChildren();
  for (const d of matching) {
    const tile = buildCard(d); list.append(tile);
    if (d.camera) { if (observer) observer.observe(tile); else visibleCards.add(d.key); }
  }
  if (focused) {
    const tile = [...list.children].find(d => d.dataset.key === focused[0]);
    [...tile?.querySelectorAll("button") ?? []].find(b => b.getAttribute("aria-label") === focused[1])?.focus({ preventScroll: true });
  }
  $("empty").hidden = matching.length > 0;
  $("reconnect").hidden = client?.ready || client?.status === "connecting" || !client;
  $("empty-title").textContent = client?.ready ? devices.length ? "没有符合条件的配件" : "还没有配件" : statusText[client?.status] || "无法建立连接";
  $("empty-note").textContent = client?.ready ? devices.length ? "试着换一个房间、类别或搜索词。" : "点「添加配件」完成 Matter 配对。"
    : transportError || "请确认已登录到此站点，且 Matter 控制器可用。";
  for (const key of cardImages.keys()) snapshotChanged(key);
  // New intersection callbacks will replace the old card elements; defer
  // demand so a redraw alone does not revoke an in-view snapshot.
  if (!observer || $("details").open || !matching.some(d => d.camera)) updateDemand();
}
function render() {
  const title = titles[view] || roomName(view);
  $("page-title").textContent = title;
  $("page-caption").textContent = view === "home" ? "在一个地方查看和控制家庭配件。" : view === "favorites" ? "常用配件一直在手边。" :
    view === "cameras" ? "浏览快照，按需打开直播。" : "查看分配到此房间的配件。";
  $("connection").textContent = transportError || statusText[client?.status];
  $("connection").parentElement.dataset.ready = Boolean(client?.ready);
  for (const [id, value] of [["total", devices.length], ["online", devices.filter(d => d.available).length], ["camera-total", devices.filter(d => d.camera).length]])
    $(id).textContent = client?.ready ? value : "—";
  $("pair").disabled = !client?.ready || pairBusy;
  $("reload").disabled = !client || client.status === "connecting";
  renderRooms(); renderGrid(); renderDetail();
}
function clearCameraUi() {
  $("video").hidden = true;
  $("camera-stop").hidden = $("camera-sound").hidden = $("camera-talk").hidden = true;
  for (const id of ["camera-sound", "camera-talk"]) $(id).setAttribute("aria-pressed", "false");
  $("camera-sound").textContent = "打开声音"; $("camera-talk").textContent = "开启对讲";
  $("camera-play").hidden = $("camera-refresh").hidden = false;
  $("camera-play").textContent = "开始直播";
  $("camera-placeholder").classList.remove("hidden");
  if (selected) snapshotChanged(selected);
}
function renderDetail() {
  const d = current(); if (!$("details").open || !d) return;
  $("details-kind").textContent = kinds[d.kind] + " · " + (d.available ? "在线" : "离线");
  $("device-name").textContent = display(d);
  $("device-meta").textContent = d.nodeName + " · 节点 " + d.nodeId + " / " + d.endpointId;
  $("readings").replaceChildren(...readings(d).map(r => node("span", null, r)));
  $("camera-panel").hidden = !d.camera;
  if (!d.available && camera) { void camera.close(); camera = null; cameraLive = false; clearCameraUi(); updateDemand(); }
  $("camera-play").disabled = !d.available || !client?.ready || client.info.schema_version < 14;
  $("camera-refresh").disabled = $("camera-play").disabled;
  const signature = JSON.stringify(d.controls);
  if ($("controls").dataset.signature !== signature) {
    $("controls").dataset.signature = signature; $("controls").replaceChildren(); controlRows.clear();
    for (const [action, limit] of Object.entries(d.controls)) buildControl(d, action, limit);
    if (!controlRows.size) $("controls").append(node("p", "muted", "此配件没有可写的常用控制。"));
  }
  for (const [action, row] of controlRows) {
    row.state.textContent = "当前：" + (action === "lock" ? d.state.lockState : formatState(action, d.state[action]));
    for (const input of row.inputs) input.disabled = !client?.ready || !d.available || pending.has(d.key + ":" + action);
    row.apply.disabled = !client?.ready || !d.available || pending.has(d.key + ":" + action);
  }
  if (d.camera) snapshotChanged(d.key);
}
function buildControl(d, action, limit) {
  const row = node("div", "control");
  const label = node("label", null, labels[action]); label.htmlFor = "target-" + action;
  const state = node("span", "current"); const targets = node("div", "targets");
  const inputs = [];
  let value;
  if (limit === true || Array.isArray(limit)) {
    const select = node("select"); select.id = "target-" + action;
    select.append(option("", "选择目标"));
    const pairs = limit === true ? [["true", action === "lock" ? "上锁" : "开启"], ["false", action === "lock" ? "解锁" : "关闭"]]
      : limit.map(mode => [mode, modes[mode] || mode]);
    for (const [key, name] of pairs) select.append(option(key, name));
    if (d.state[action] !== undefined) select.value = String(d.state[action]);
    if (select.selectedIndex < 0) select.selectedIndex = 0;
    inputs.push(select); targets.append(select);
    value = () => !select.value ? undefined : limit === true ? select.value === "true" : select.value;
  } else {
    const makeNumber = (id, text, description, min, max, step, initial) => {
      const input = node("input"); input.id = id; input.type = "number"; input.min = min; input.max = max; input.step = step;
      input.setAttribute("aria-label", description); input.placeholder = text;
      if (Number.isFinite(initial)) input.value = initial;
      inputs.push(input); targets.append(input); return input;
    };
    if (action === "color") {
      const hue = makeNumber("target-color", "色相 °", "目标色相（0 至 360 度）", 0, 360, 1, Number.isFinite(d.state.color?.hue) ? Math.round(d.state.color.hue) : undefined);
      const sat = makeNumber("target-color-saturation", "饱和度 %", "目标饱和度（0 至 100）", 0, 100, 1, Number.isFinite(d.state.color?.saturation) ? Math.round(d.state.color.saturation) : undefined);
      value = () => ({ hue: hue.valueAsNumber, saturation: sat.valueAsNumber });
    } else {
      const input = makeNumber("target-" + action, action === "colorTemperature" ? "mired" : action.endsWith("Setpoint") ? "°C" : "0–100%", labels[action], limit.min, limit.max, limit.step, d.state[action]);
      value = () => input.valueAsNumber;
    }
  }
  const apply = button("应用", () => sendControl(d.key, action, value())); apply.setAttribute("aria-label", "应用" + labels[action]);
  targets.append(apply); row.append(label, state, targets); $("controls").append(row);
  controlRows.set(action, { state, apply, inputs });
}
async function sendControl(key, action, value) {
  const requestKey = key + ":" + action;
  if (pending.has(requestKey)) return;
  const epoch = client.epoch;
  const show = (message, fail) => selected === key && $("details").open ? feedback("detail-feedback", message, fail) : notice(message);
  try {
    const d = devices.find(device => device.key === key);
    const wire = controlRequest(d, action, value);
    pending.add(requestKey); render();
    const response = await client.request(wire.command, wire.args);
    if (client.epoch !== epoch) return;
    if (!writeSucceeded(wire, response)) throw Error("设备未确认属性写入。");
    show("命令已完成，当前值以设备回报为准。");
    setTimeout(() => { if (client.ready && client.epoch === epoch) client.refreshNode(d.nodeId).catch(() => {}); }, 350);
  } catch (error) {
    if (client.epoch === epoch) show(error.message, true);
  } finally { if (client.epoch === epoch) { pending.delete(requestKey); render(); } }
}
function openDetails(key) {
  const d = devices.find(device => device.key === key); if (!d) return;
  selected = key; $("controls").dataset.signature = "";
  const setting = prefsFor(d); $("override-name").value = setting.name || "";
  $("device-room").replaceChildren(option("", "未分配房间"), ...prefs.rooms.map(r => option(r.id, r.name)));
  $("device-room").value = setting.room || ""; $("device-favorite").checked = !!setting.favorite;
  $("override-name").disabled = $("device-room").disabled = $("device-favorite").disabled = !profile;
  $("metadata").querySelector("button").disabled = !profile;
  feedback("detail-feedback", ""); $("details").showModal(); renderDetail(); updateDemand();
}
async function stopVideo() {
  if (!camera) { cameraLive = false; clearCameraUi(); updateDemand(); return; }
  const old = camera; camera = null; $("camera-stop").disabled = true;
  $("camera-placeholder").classList.remove("hidden");
  $("camera-note").textContent = "正在结束本次会话…";
  try { await old.close(); } finally {
    $("camera-stop").disabled = false;
    cameraLive = false; clearCameraUi(); updateDemand(); renderDetail();
  }
}
async function closeDetail() {
  if (closeBusy) return;
  closeBusy = true;
  try { await stopVideo(); if ($("details").open) $("details").close(); selected = null; }
  finally { closeBusy = false; updateDemand(); }
}
function videoReport(state, text, talk, mic, audio) {
  if (!cameraLive) return;
  $("camera-note").textContent = text;
  $("camera-play").hidden = !["idle", "error"].includes(state);
  $("camera-play").textContent = state === "error" ? "重试" : "开始直播";
  $("camera-sound").hidden = state !== "streaming" || !audio;
  $("camera-talk").hidden = state !== "streaming" || !talk || typeof navigator.mediaDevices?.getUserMedia !== "function";
  $("camera-talk").setAttribute("aria-pressed", String(mic)); $("camera-talk").textContent = mic ? "结束对讲" : "开启对讲";
}
function playVideo() {
  const d = current(); if (!d?.available || closeBusy) return;
  if (camera) { void camera.player.start(); return; }
  cameraLive = true; updateDemand();
  $("video").hidden = false; $("camera-image").hidden = true; $("camera-placeholder").hidden = false;
  $("camera-stop").hidden = false; $("camera-refresh").hidden = true; $("camera-play").hidden = true;
  try {
    camera = new BrowserCamera(client, d, $("video"), $("camera-placeholder"), videoReport, state => {
      if (state === "denied") $("camera-note").textContent = "麦克风未授权或不可用，直播仍可继续。";
      $("camera-talk").textContent = state === "active" ? "结束对讲" : state === "requesting" ? "请求权限…" : "开启对讲";
      $("camera-talk").disabled = state === "requesting";
      $("camera-talk").setAttribute("aria-pressed", String(state === "active"));
    });
    void camera.player.start();
  } catch (e) { cameraLive = false; clearCameraUi(); updateDemand(); feedback("detail-feedback", e.message, true); }
}
function openRoom(room) {
  if (!profile) return;
  roomEdit = room?.id || null; $("room-heading").textContent = roomEdit ? "编辑房间" : "添加房间";
  $("room-name").value = room?.name || ""; $("room-delete").hidden = !roomEdit;
  feedback("room-feedback", ""); $("room-dialog").showModal();
}
function modelChanged() {
  const next = client.profile;
  if (next !== profile) {
    profile = next; loadPrefs(); view = "home"; pending.clear();
    if ($("room-dialog").open) $("room-dialog").close();
  }
  devices = devicesFromNodes(client.nodes.values());
  if (!client.ready || selected && !current()) {
    pending.clear();
    if (camera) { if (client.ready) void camera.close(); else camera.dispose(); camera = null; }
    cameraLive = false; clearCameraUi();
    if ($("details").open) { $("details").close(); selected = null; }
    if ($("pair-dialog").open) $("pair-dialog").close();
  }
  render(); if (!client.ready) updateDemand();
}

for (const [key, name] of Object.entries(kinds)) $("kind").append(option(key, name));
for (const nav of document.querySelectorAll("button[data-view]")) nav.addEventListener("click", () => setView(nav.dataset.view));
for (const control of ["search", "kind", "availability"]) $(control).addEventListener(control === "search" ? "input" : "change", renderGrid);
for (const closer of document.querySelectorAll("[data-close]")) closer.addEventListener("click", () => $(closer.dataset.close).close());
$("reconnect").addEventListener("click", () => client?.connect());
$("reload").addEventListener("click", async () => {
  if (!client?.ready) { client?.connect(); return; }
  try { await client.refresh(); notice("配件列表已刷新。"); } catch { notice("刷新失败，请重试。"); }
});
$("pair").addEventListener("click", () => {
  if (!client?.ready || pairBusy) return;
  $("pair-code").value = ""; feedback("pair-feedback", ""); $("pair-dialog").showModal();
});
$("pair-dialog").addEventListener("close", () => { $("pair-code").value = ""; });
$("pair-form").addEventListener("submit", async event => {
  event.preventDefault(); if (pairBusy || !client?.ready) return;
  const value = $("pair-code").value;
  $("pair-code").value = "";
  pairBusy = true; $("pair-submit").disabled = $("pair").disabled = true;
  feedback("pair-feedback", "正在配对，可能需要几分钟…");
  const epoch = client.epoch;
  try {
    await client.commission(value);
    if (epoch === client.epoch) { $("pair-dialog").close(); notice("配对完成。"); }
  } catch (error) {
    if (epoch === client.epoch) feedback("pair-feedback", error.message + " 请确认配件仍处于配对模式。", true);
  } finally { pairBusy = false; $("pair-submit").disabled = false; $("pair").disabled = !client.ready; }
});
$("detail-close").addEventListener("click", closeDetail);
$("details").addEventListener("cancel", event => { event.preventDefault(); void closeDetail(); });
$("details").addEventListener("close", () => updateDemand());
$("camera-play").addEventListener("click", playVideo);
$("camera-stop").addEventListener("click", stopVideo);
$("camera-refresh").addEventListener("click", () => selected && snapshots.refresh(selected));
$("camera-sound").addEventListener("click", () => {
  if (!camera) return;
  const enable = $("camera-sound").getAttribute("aria-pressed") !== "true";
  camera.player.setSound(enable); $("camera-sound").setAttribute("aria-pressed", String(enable));
  $("camera-sound").textContent = enable ? "关闭声音" : "打开声音";
});
$("camera-talk").addEventListener("click", () => {
  if (camera) void camera.player.setTalk($("camera-talk").getAttribute("aria-pressed") !== "true");
});
$("metadata").addEventListener("submit", event => {
  event.preventDefault();
  if (!profile || !current()) return;
  prefs.devices[selected] = { name: $("override-name").value.trim(), room: $("device-room").value, favorite: $("device-favorite").checked };
  savePrefs(); feedback("detail-feedback", "家庭设置已保存。"); render();
});
$("room-add").addEventListener("click", () => openRoom(null));
$("room-form").addEventListener("submit", event => {
  event.preventDefault(); if (!profile) return;
  const name = $("room-name").value.trim();
  if (!name) { feedback("room-feedback", "请输入房间名称。", true); return; }
  if (prefs.rooms.some(r => r.name === name && r.id !== roomEdit)) { feedback("room-feedback", "该房间名称已存在。", true); return; }
  if (roomEdit) prefs.rooms = prefs.rooms.map(r => r.id === roomEdit ? { ...r, name } : r);
  else prefs.rooms.push({ id: "room-" + crypto.randomUUID(), name });
  savePrefs(); $("room-dialog").close(); render();
});
$("room-delete").addEventListener("click", () => {
  if (!profile || !roomEdit) return;
  prefs.rooms = prefs.rooms.filter(r => r.id !== roomEdit);
  for (const setting of Object.values(prefs.devices)) if (setting.room === roomEdit) setting.room = "";
  if (view === roomEdit) view = "home";
  savePrefs(); $("room-dialog").close(); render();
});
document.addEventListener("visibilitychange", updateDemand);
window.addEventListener("pagehide", () => { camera?.abandon(); snapshots?.dispose(); client?.dispose(); });
window.addEventListener("pageshow", event => { if (event.persisted) location.reload(); });
try {
  client = new MatterClient({ url: sameOriginSocket(location), notify: modelChanged });
  snapshots = new Snapshots(client, snapshotChanged);
  client.connect();
} catch (error) { transportError = error.message; render(); }
