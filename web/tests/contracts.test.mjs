import { test } from "node:test";
import assert from "node:assert/strict";
import { MatterClient, sameOriginSocket, pairingCode } from "../src/protocol.js";
import { controlRequest, devicesFromNodes, writeSucceeded } from "../src/devices.js";
import { jpegSnapshot, privateCamera, snapshotTarget, Snapshots } from "../src/camera.js";
import { bulb, extra, caps } from "./fixtures.mjs";
import { readFile } from "node:fs/promises";

const tick = () => new Promise(resolve => setImmediate(resolve));
class Socket extends EventTarget {
  constructor(url) { super(); this.url = url; this.readyState = 1; this.sent = []; }
  send(envelope) { this.sent.push(JSON.parse(envelope)); }
  close() { this.readyState = 3; this.dispatchEvent(new Event("close")); }
  emit(message) { this.dispatchEvent(new MessageEvent("message", { data: JSON.stringify(message) })); }
  reply(request, result) { this.emit({ message_id: request.message_id, result }); }
}
async function ready(t) {
  const c = new MatterClient({ url: "ws://localhost/ws", WebSocketImpl: Socket, retry: false });
  t.after(() => c.dispose()); c.connect();
  const s = c.socket;
  s.emit({ schema_version: 14, min_supported_schema_version: 11, compressed_fabric_id: "A" });
  assert.deepEqual(s.sent[0], { message_id: "web-1", command: "start_listening", args: {} });
  s.reply(s.sent[0], [structuredClone(bulb)]); await tick();
  assert.equal(c.status, "ready"); assert.equal(c.profile, "compressed_fabric_id:A");
  return [c, s];
}

test("same-origin secure /ws only, and local fixture HTTP", () => {
  assert.equal(sameOriginSocket({ href: "https://user:pw@example.test:443/home?q=secret" }), "wss://example.test/ws");
  assert.equal(sameOriginSocket({ href: "http://127.0.0.1:10080/" }), "ws://127.0.0.1:10080/ws");
  assert.throws(() => sameOriginSocket({ href: "http://example.test/" }), /HTTPS/);
});
test("normalizes accepted commissioning inputs without accepting HomeKit length or query payloads", () => {
  assert.equal(pairingCode("12-3456-78901"), "12345678901");
  assert.equal(pairingCode("mt:y.123-5"), "MT:Y.123-5");
  assert.throws(() => pairingCode("123-45-678"));
  assert.throws(() => pairingCode("MT:ABC?pw=leak"));
});
test("subscribes, keeps device state authoritative, preserves error zero/status in a rejected reply", async t => {
  const [c, s] = await ready(t);
  const wire = controlRequest(devicesFromNodes(c.nodes.values())[0], "power", true);
  const promise = c.request(wire.command, wire.args);
  assert.equal(c.nodes.get(1).attributes["1/6/0"], false);
  s.reply(s.sent.at(-1), null); await promise;
  assert.equal(c.nodes.get(1).attributes["1/6/0"], false);
  s.emit({ event: "attribute_updated", data: [1, "1/6/0", true] });
  assert.equal(c.nodes.get(1).attributes["1/6/0"], true);
  const camera = c.request("camera_start_stream", { node_id: 1, endpoint_id: 3 });
  s.emit({ message_id: s.sent.at(-1).message_id, error_code: 0, status: 137, details: "ResourceExhausted" });
  await assert.rejects(camera, e => e.wire.error_code === 0 && e.wire.status === 137);
});
test("changing identities invalidates state and old request before new subscription", async t => {
  const [c, s] = await ready(t);
  const waiting = c.request("get_nodes");
  const rejected = assert.rejects(waiting, /断开/);
  s.emit({ event: "server_info_updated", data: { compressed_fabric_id: "B" } });
  await rejected;
  assert.equal(c.ready, false); assert.equal(c.nodes.size, 0);
  assert.equal(s.readyState, 3);
  await new Promise(resolve => setTimeout(resolve, 15));
  assert.notEqual(c.socket, s);
  s.emit({ event: "node_added", data: bulb });
  assert.equal(c.nodes.size, 0);
});
test("server clusters, unknown values, inherited reachability, numeric Descriptor TLV", () => {
  const mock = structuredClone(bulb);
  mock.attributes["5/29/0"] = [{ "0": 14 }]; mock.attributes["5/29/1"] = [29, 57]; mock.attributes["5/57/17"] = false;
  mock.endpoint_parents = { 1: 5 };
  mock.attributes["2/6/0"] = false; mock.attributes["2/29/2"] = [6]; mock.attributes["1/8/0"] = 255;
  const [lamp, sensor, camera, outlet] = devicesFromNodes([mock]);
  assert.equal(lamp.kind, "light"); assert.equal(lamp.available, false); assert.equal(lamp.state.brightness, undefined);
  assert.equal(sensor.controls.power, undefined); assert.equal(sensor.state.contact, true);
  assert.equal(camera.camera, true); assert.equal(outlet.available, false);
  assert.throws(() => controlRequest(lamp, "power", true), /离线/);
});
test("Qt-shaped control payloads, capabilities, timed lock and write paths", () => {
  const [lamp] = devicesFromNodes([bulb]);
  assert.deepEqual(controlRequest(lamp, "brightness", 50), { command: "device_command", args: { node_id: 1, endpoint_id: 1,
    cluster_id: 8, command_name: "MoveToLevelWithOnOff", payload: { level: 127, transitionTime: 5, optionsMask: 0, optionsOverride: 0 } } });
  assert.equal(controlRequest(lamp, "color", { hue: 90, saturation: 100 }).args.payload.enhancedHue, 16384);
  assert.throws(() => controlRequest(lamp, "colorTemperature", 152));
  const [thermostat, cover, lock, fan] = devicesFromNodes([extra]);
  const heat = controlRequest(thermostat, "heatSetpoint", 23);
  assert.deepEqual(heat, { command: "write_attribute", args: { node_id: 2, attribute_path: "1/513/18", value: 2300 } });
  assert.throws(() => controlRequest(thermostat, "heatSetpoint", 24), /温差/);
  assert.equal(controlRequest(cover, "position", 25).args.payload.liftPercent100thsValue, 7500);
  assert.equal(lock.state.lock, undefined);
  assert.equal(controlRequest(lock, "lock", false).args.timed_request_timeout_ms, 10000);
  assert.equal(controlRequest(fan, "fanMode", "auto").args.value, 5);
  assert.equal(writeSucceeded(heat, [{ Status: 0, Path: { ClusterId: 513, AttributeId: 18, EndpointId: 1 } }]), true);
  assert.equal(writeSucceeded(heat, [{ Status: 0, Path: { ClusterId: 513, AttributeId: 17, EndpointId: 1 } }]), false);
});
test("JPEG target is advisory, private mode and decoder size caps", async () => {
  assert.deepEqual(snapshotTarget(caps), { width: 640, height: 360 });
  assert.equal(snapshotTarget({ snapshot: { capabilities: [{ image_codec: "PNG" }] } }), null);
  assert.equal(privateCamera({ privacy: { soft_livestream_mode_enabled: true } }), true);
  const jpeg = await readFile(new URL("./snapshot.jpg", import.meta.url));
  assert.equal(jpegSnapshot({ data: jpeg.toString("base64"), codec: "JPEG" }).type, "image/jpeg");
  assert.equal(jpegSnapshot({ data: "AAAA", codec: "JPEG" }), null);
  assert.equal(jpegSnapshot({ data: jpeg.toString("base64"), codec: "HTML" }), null);
});
test("late snapshot releases its own allocation even if no longer wanted; never releases adopted stream", async () => {
  const device = devicesFromNodes([bulb]).find(d => d.camera);
  const called = []; let finish;
  const jpeg = await readFile(new URL("./snapshot.jpg", import.meta.url));
  const client = { epoch: 1, ready: true, info: { schema_version: 14 }, request(command, args) {
    called.push([command, args]);
    if (command === "camera_get_capabilities") return Promise.resolve(caps);
    if (command === "camera_snapshot") return new Promise(resolve => { finish = resolve; });
    return Promise.resolve(null);
  } };
  const snapshots = new Snapshots(client, () => {});
  try {
    snapshots.demand([device], false); await tick();
    snapshots.demand([], false); finish({ stream_id: 48, provenance: "allocated", codec: "JPEG", data: jpeg.toString("base64") }); await tick();
    assert.equal(snapshots.get(device.key), undefined);
    assert.deepEqual(called.at(-1), ["camera_release_stream", { node_id: 1, endpoint_id: 3, kind: "snapshot", stream_id: 48 }]);
    snapshots.demand([device], false); await tick();
    finish({ stream_id: 49, provenance: "adopted", codec: "JPEG", data: jpeg.toString("base64") }); await tick();
    assert.ok(snapshots.get(device.key).url);
    assert.equal(called.filter(([cmd]) => cmd === "camera_release_stream").length, 1);
  } finally { snapshots.dispose(); }
});
