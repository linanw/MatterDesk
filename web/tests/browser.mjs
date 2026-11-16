import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { createHash } from "node:crypto";
import { readFile, mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { bulb, caps } from "./fixtures.mjs";

const web = fileURLToPath(new URL("..", import.meta.url));
const jpeg = (await readFile(new URL("./snapshot.jpg", import.meta.url))).toString("base64");
const dist = join(web, "dist");
const data = structuredClone(bulb);
const frames = [];
const wireNotes = [];
const wsSockets = new Set();
const mime = { html: "text/html", css: "text/css", js: "text/javascript" };
const server = createServer(async (req, res) => {
  const requested = req.url === "/" ? "/index.html" : req.url;
  if (!/^\/[\w/.-]+$/.test(requested) || requested.includes("..")) { res.writeHead(404).end(); return; }
  try { res.writeHead(200, { "Content-Type": mime[requested.split(".").at(-1)] || "application/octet-stream" }); res.end(await readFile(join(dist, requested))); }
  catch { res.end(); }
});
function reply(socket, message) {
  const payload = Buffer.from(JSON.stringify(message));
  const header = payload.length <= 125 ? Buffer.from([0x81,payload.length]) : Buffer.from([0x81,126,payload.length >> 8,payload.length & 255]);
  socket.write(Buffer.concat([header,payload]));
}
server.on("upgrade", (req, socket) => {
  if (req.url !== "/ws" || req.headers.origin !== `http://${req.headers.host}`) { socket.destroy(); return; }
  const key = createHash("sha1").update(req.headers["sec-websocket-key"]+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
  socket.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "+key+"\r\n\r\n");
  wsSockets.add(socket); socket.on("close", () => wsSockets.delete(socket)); socket.on("error", () => {});
  let buffer = Buffer.alloc(0);
  socket.on("data", bytes => {
    buffer = Buffer.concat([buffer, bytes]);
    while (buffer.length >= 2) {
      const op = buffer[0] & 15; let len = buffer[1] & 127, h = 2;
      if (len === 127 || !(buffer[1] & 128)) { wireNotes.push(["invalid", op, len, buffer[1]]); socket.destroy(); return; }
      if (len === 126) { if (buffer.length < 4) return; len = buffer.readUInt16BE(2); h = 4; }
      if (buffer.length < h + 4 + len) return;
      const payload = Buffer.alloc(len);
      for (let i = 0; i < len; i++) payload[i] = buffer[h+4+i] ^ buffer[h+i%4];
      buffer = buffer.subarray(h+4+len);
      if (op === 8) { wireNotes.push(["close",payload.length >= 2 ? payload.readUInt16BE() : null]); socket.end(Buffer.from([0x88,0])); return; }
      if (op !== 1) continue;
      const request = JSON.parse(payload.toString());
      frames.push(request);
      const done = result => reply(socket, { message_id: request.message_id, result });
      const { command, args } = request;
      if (["start_listening","get_nodes"].includes(command)) done([data]);
      else if (command === "get_node") done(data);
      else if (command === "device_command") {
        done(null);
        const path = `${args.endpoint_id}/6/0`;
        setTimeout(() => { if (socket.destroyed) return; data.attributes[path] = args.command_name === "On"; reply(socket, { event: "attribute_updated", data: [1,path,data.attributes[path]] }); }, 350);
      }
      else if (command === "commission_with_code") {
        done({ node_id: 10, available: true, attributes: { "1/29/0": [{ "0": 0x100 }], "1/29/1": [6,29], "1/40/5": "新配对灯" } });
      }
      else if (command === "camera_get_capabilities") done(caps);
      else if (command === "camera_snapshot") done({ codec: "JPEG", data: jpeg, provenance: "allocated", stream_id: 55 });
      else if (command === "camera_start_stream") reply(socket, { message_id: request.message_id, error_code: 0, status: 137, details: "ResourceExhausted" });
      else done(null);
    }
  });
  reply(socket, { schema_version: 14, min_supported_schema_version: 11, compressed_fabric_id: "fixture-fabric" });
});
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const wait = async (predicate, label, limit = 6500) => {
  const expire = Date.now() + limit;
  do { if (await predicate()) return; await sleep(35); } while (Date.now() < expire);
  throw Error("Browser assertion timed out: " + label);
};
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
const profile = await mkdtemp(join(tmpdir(), "matterdesk-web-fixture-"));
const chrome = spawn(process.env.MATTERDESK_CHROME || "google-chrome", ["--headless=new","--disable-dev-shm-usage",
  "--disable-background-networking","--disable-component-update","--no-first-run","--no-default-browser-check","--remote-debugging-port=0",
  "--enable-logging=stderr","--user-data-dir="+profile, "about:blank"], { stdio: ["ignore","ignore","pipe"] });
let failure, target, stderr = "";
chrome.on("error", e => failure = e);
chrome.on("exit", (code, signal) => { failure = Error(`Chromium exited before the fixture completed (${signal || `code ${code}`})`); });
chrome.stderr.on("data", chunk => { stderr = (stderr + chunk).slice(-3500); });
let browser;
const transport = []; let debug;
try {
  // With a fresh profile and port 0 Chromium writes the selected port here.
  // Some hosted Chrome builds suppress the DevTools stderr announcement.
  const activePort = join(profile, "DevToolsActivePort");
  await wait(async () => {
    if (failure) throw failure;
    const port = Number((await readFile(activePort, "utf8").catch(() => "")).split("\n")[0]);
    if (!Number.isInteger(port) || port < 1 || port > 65535) return false;
    const list = await fetch(`http://127.0.0.1:${port}/json/list`, { signal: AbortSignal.timeout(1000) })
      .then(response => response.json()).catch(() => []);
    target = Array.isArray(list) && list.find(item => item.type === "page" && typeof item.webSocketDebuggerUrl === "string");
    return Boolean(target);
  }, "Chromium/CDP startup", 30000);
  browser = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { browser.onopen = resolve; browser.onerror = reject; });
  let id = 0; const pending = new Map(); const exceptions = [];
  browser.onmessage = event => {
    const data = JSON.parse(event.data); const entry = pending.get(data.id);
    if (entry) { pending.delete(data.id); data.error ? entry.reject(Error(data.error.message)) : entry.resolve(data.result); }
    if (data.method?.includes("webSocketFrameError")) transport.push(data.params.errorMessage);
    if (data.method === "Runtime.exceptionThrown") exceptions.push(data.params.exceptionDetails);
  };
  const rpc = (method, params) => new Promise((resolve, reject) => { const number = ++id; pending.set(number, { resolve, reject }); browser.send(JSON.stringify({ id: number, method, params })); });
  const evaluate = async expression => {
    const result = await rpc("Runtime.evaluate", { expression, returnByValue: true, awaitPromise: true });
    if (result.exceptionDetails) throw Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text);
    return result.result.value;
  };
  debug = () => evaluate("({connection:document.querySelector('#connection')?.textContent, image:document.querySelector('.preview img')?.outerHTML, notice:document.querySelector('.preview')?.textContent})");
  const fill = async (sel, value) => evaluate(`(()=>{const input=document.querySelector(${JSON.stringify(sel)});input.value=${JSON.stringify(value)};input.dispatchEvent(new Event("input",{bubbles:true}));input.dispatchEvent(new Event("change",{bubbles:true}));})()`);
  const click = async selector => evaluate(`document.querySelector(${JSON.stringify(selector)}).click()`);
  const contains = async (selector, text) => evaluate(`document.querySelector(${JSON.stringify(selector)})?.textContent.includes(${JSON.stringify(text)})`);
  const colors = async selector => evaluate(`(()=>{const style=getComputedStyle(document.querySelector(${JSON.stringify(selector)}));return [style.color,style.backgroundColor]})()`);
  const luminance = color => color.match(/\d+/g).map(Number).slice(0, 3).reduce((sum, value, i) => {
    const s = value / 255;
    return sum + [0.2126, 0.7152, 0.0722][i] * (s <= .04045 ? s / 12.92 : ((s + .055) / 1.055) ** 2.4);
  }, 0);
  const readable = (fg, bg) => {
    const levels = [luminance(fg), luminance(bg)].sort((a, b) => a - b);
    assert.ok((levels[1] + .05) / (levels[0] + .05) >= 4.5, `${fg} on ${bg} needs 4.5:1 contrast`);
  };
  const checkPalette = async () => {
    const [, sidebar] = await colors(".sidebar");
    const [, card] = await colors(".card");
    const [addInk, add] = await colors("#pair");
    const [controlInk, control] = await colors('[data-key="1/1"] .card-actions button');
    const [statusInk] = await colors('[data-key="1/1"] .status');
    const [offlineInk] = await colors(".status.offline");
    const gradient = await evaluate("getComputedStyle(document.querySelector('.shell')).backgroundImage");
    assert.equal(sidebar, "rgb(238, 237, 241)"); // Qt sidebar
    assert.equal(card, "rgb(243, 241, 244)"); // Qt device card
    assert.equal(add, "rgb(223, 141, 64)");
    assert.equal(control, "rgb(230, 240, 231)");
    for (const stop of ["rgb(56, 61, 91)", "rgb(99, 97, 129)", "rgb(178, 140, 145)"]) assert.ok(gradient.includes(stop), gradient);
    for (const [fg, bg] of [[addInk, add], [controlInk, control], [statusInk, card], [offlineInk, card],
      await colors(".nav[aria-pressed=true]"), await colors(".footnote")]) readable(fg, bg);
  };
  await rpc("Runtime.enable");
  await rpc("Page.enable");
  await rpc("Network.enable");
  await rpc("Emulation.setDeviceMetricsOverride", { width: 1100, height: 900, deviceScaleFactor: 1, mobile: false });
  await rpc("Page.navigate", { url: `http://127.0.0.1:${server.address().port}/` });
  await wait(() => contains("#connection","在线"), "same-origin app and real browser WebSocket greeting");
  assert.equal(await evaluate("document.querySelectorAll('.card').length"), 4);
  await checkPalette();
  await fill("#search","门窗"); assert.equal(await evaluate("document.querySelectorAll('.card').length"), 1);
  await fill("#search","");
  await click("#room-add"); await fill("#room-name","客厅"); await click("#room-form button[type=submit]");
  await click('[data-key="1/1"] button[aria-label^="查看详情"]');
  await fill("#override-name","书桌灯");
  await evaluate("document.querySelector('#device-room').selectedIndex=1; document.querySelector('#device-favorite').checked=true;");
  await click("#metadata button"); await click("#detail-close");
  await wait(() => contains('[data-key="1/1"] h3',"书桌灯"), "custom name");
  await click('button[data-view="favorites"]'); assert.equal(await evaluate("document.querySelectorAll('.card').length"), 1);
  await click('[data-key="1/1"] button[aria-label^="开启"]');
  assert.ok(await contains('[data-key="1/1"] .status',"关闭"), "successful write must still await authoritative device event");
  await wait(() => contains('[data-key="1/1"] .status',"开启"), "authoritative control event");
  await click('button[data-view="home"]');
  await wait(() => evaluate("document.querySelector('[data-key=\"1/3\"] img')?.naturalWidth === 128"), "decoded camera JPEG thumbnail");
  await click('[data-key="1/3"] button[aria-label^="查看详情"]');
  await wait(() => evaluate("document.querySelector('#camera-image').naturalWidth === 128"), "camera detail snapshot");
  await click("#camera-play");
  await wait(() => contains("#camera-note","停止直播"), "shared player surfaces error code zero with busy status", 12000);
  assert.equal(await evaluate("document.querySelector('#camera-play').textContent"),"重试");
  assert.equal(await evaluate("document.querySelector('#camera-talk').hidden"),true);
  assert.equal(frames.filter(f => f.command === "camera_start_stream").length,1);
  assert.equal(frames.find(f => f.command === "camera_start_stream").args.allow_eviction,false);
  await click("#camera-stop"); await click("#detail-close");
  await click("#pair"); await fill("#pair-code","12345678901"); await click("#pair-submit");
  await wait(() => contains('[data-key="10/1"] h3',"新配对灯"), "commission response adds actual device");
  assert.equal(await evaluate("document.querySelector('#pair-code').value"),"");
  assert.equal(await evaluate("JSON.stringify({...localStorage}).includes('12345678901')"),false);
  const output = process.env.MATTERDESK_WEB_SCREENSHOT_DIR;
  const screenshot = async name => {
    if (!output) return;
    await mkdir(output, { recursive: true });
    const { data: image } = await rpc("Page.captureScreenshot", { format: "png", captureBeyondViewport: false });
    await writeFile(join(output, name), Buffer.from(image, "base64"));
  };
  await screenshot("matterdesk-web-desktop.png");
  await rpc("Emulation.setDeviceMetricsOverride", { width: 375, height: 812, deviceScaleFactor: 1, mobile: true });
  await wait(() => evaluate("document.documentElement.scrollWidth <= 375 && innerWidth <= 375"), "mobile portrait no horizontal overflow");
  await checkPalette();
  await screenshot("matterdesk-web-mobile.png");
  assert.equal(exceptions.length, 0, JSON.stringify(exceptions));
  console.log("Chromium fixture passed: desktop/mobile layout, same-origin WebSocket, room/favorite, authoritative control, JPEG, WebRTC busy recovery, pairing.");
} catch (error) {
  console.error(error, await debug?.().catch(() => undefined), transport, wireNotes, frames.map(f => f.command), stderr.slice(-2500)); process.exitCode = 1;
} finally {
  browser?.close();
  for (const socket of wsSockets) socket.destroy();
  const serverClosed = new Promise(resolve => server.close(resolve));
  server.closeAllConnections();
  await serverClosed;
  chrome.kill("SIGTERM");
  if (chrome.pid && chrome.exitCode === null && chrome.signalCode === null)
    await Promise.race([new Promise(resolve => chrome.once("exit", resolve)), sleep(2200)]);
  await rm(profile, { recursive: true, force: true, maxRetries: 6, retryDelay: 150 });
}
