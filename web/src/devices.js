const array = value => Array.isArray(value) ? value : [];
const finite = (value, lo, hi) => typeof value === "number" && Number.isFinite(value) && value >= lo && value <= hi;
const integer = (value, lo, hi) => Number.isInteger(value) && finite(value, lo, hi);
const text = value => typeof value === "string" ? value.trim() : "";
const range = (min, max, step = 1) => ({ min, max, step });
const thermostat = { off: 0, auto: 1, cool: 3, heat: 4 };
const fan = { off: 0, low: 1, medium: 2, high: 3, auto: 5 };
const modeOf = (map, value) => Object.keys(map).find(key => map[key] === value);
const round = n => n < 0 ? -Math.round(-n) : Math.round(n);
const contains = (list, item) => array(list).includes(item);
export const kinds = { light: "灯光", outlet: "插座", switch: "开关", sensor: "传感器", camera: "摄像头",
  lock: "门锁", cover: "窗帘", thermostat: "温控", fan: "风扇", device: "配件" };
export const labels = { power: "电源", brightness: "亮度", colorTemperature: "色温", color: "颜色", lock: "门锁",
  position: "开合", thermostatMode: "温控模式", heatSetpoint: "制热温度", coolSetpoint: "制冷温度", fanMode: "风扇模式", fanPercent: "风速",
  temperature: "室温", humidity: "湿度", occupancy: "人体", contact: "门窗", booleanState: "状态", battery: "电量",
  illuminance: "照度", airQuality: "空气质量", co2: "CO₂", pm25: "PM2.5", powerWatts: "功率", energyKwh: "累计电量" };
export const modes = { off: "关闭", auto: "自动", cool: "制冷", heat: "制热", low: "低速", medium: "中速", high: "高速" };
const sensorKeys = ["temperature", "humidity", "contact", "occupancy", "booleanState", "battery", "illuminance", "airQuality", "co2", "pm25", "powerWatts", "energyKwh"];
const lights = [0x100, 0x101, 0x10c, 0x10d, 0x10e];
export function devicesFromNodes(nodes) {
  const devices = [];
  for (const node of nodes) {
    if (!Number.isSafeInteger(node?.node_id) || !node.attributes) continue;
    const attributes = node.attributes, nodeId = node.node_id;
    const endpoints = new Set();
    for (const key of Object.keys(attributes)) if (/^\d+\/\d+\/\d+$/.test(key)) {
      const ep = Number(key.split("/")[0]); if (integer(ep, 1, 65535)) endpoints.add(ep);
    }
    const nodeName = text(attributes["0/40/5"]) || text(attributes["0/40/3"]) || `Matter 节点 ${nodeId}`;
    for (const endpointId of [...endpoints].sort((a, b) => a - b)) {
      const at = (cluster, attribute) => attributes[`${endpointId}/${cluster}/${attribute}`];
      const servers = new Set(array(at(29, 1)).filter(v => integer(v, 0, 0xffffffff)));
      const types = array(at(29, 0)).map(value => value?.deviceType ?? value?.device_type ?? value?.["0"]).filter(Number.isInteger);
      if ((!servers.size && !types.length) || types.includes(14)) continue;
      const has = id => servers.has(id);
      const supports = (cluster, command) => has(cluster) && (at(cluster, 65529) === undefined || contains(at(cluster, 65529), command));
      const display = text(at(57, 5)) || text(at(40, 5)) || text(at(57, 3)) || `${nodeName} · 端点 ${endpointId}`;
      let available = node.available === true;
      let p = endpointId; const seen = new Set();
      while (p && !seen.has(p) && available) {
        seen.add(p);
        if (attributes[`${p}/57/17`] === false) available = false;
        let parent = node.endpoint_parents?.[p];
        if (!integer(parent, 0, 65535)) {
          parent = 0;
          for (const candidate of endpoints) if (candidate !== p && Object.hasOwn(attributes, `${candidate}/57/17`) &&
            contains(attributes[`${candidate}/29/3`], p)) { parent = candidate; break; }
        }
        p = parent;
      }
      const state = {}, controls = {};
      const features = (cluster, fallback) => integer(at(cluster, 65532), 0, 0xffff) ? at(cluster, 65532) : fallback;
      const colorFeatures = features(768, at(768, 16394));
      const mired = at(768, 7);
      const ct = has(768) && (colorFeatures === undefined ? mired !== undefined : colorFeatures & 16);
      let kind = lights.some(type => types.includes(type)) || has(8) || ct ? "light" :
        types.includes(0x10a) || types.includes(0x10b) ? "outlet" : has(6) ? "switch" : "device";
      if (typeof at(6, 0) === "boolean") state.power = at(6, 0);
      if (supports(6, 0) && supports(6, 1)) controls.power = true;
      if (finite(at(8, 0), 0, 254)) state.brightness = Math.round(at(8, 0) * 100 / 254);
      if (supports(8, 4)) controls.brightness = range(0, 100);
      if (finite(mired, 1, 65534)) state.colorTemperature = mired;
      if (ct && supports(768, 10)) {
        const lo = integer(at(768, 16395), 1, 65534) ? at(768, 16395) : 1;
        const hi = integer(at(768, 16396), 1, 65534) ? at(768, 16396) : 65534;
        if (lo <= hi) controls.colorTemperature = range(lo, hi);
      }
      const enhanced = !!(colorFeatures & 2) && supports(768, 67);
      if ((colorFeatures & 1) && (enhanced || supports(768, 6))) {
        controls.color = { enhanced };
        const sat = at(768, 1), h = at(768, 0), e = at(768, 16384);
        if (integer(sat, 0, 254) && ((colorFeatures & 2) && integer(e, 0, 65535) || integer(h, 0, 254)))
          state.color = { hue: (colorFeatures & 2) && integer(e, 0, 65535) ? e * 360 / 65536 : h * 360 / 254, saturation: sat * 100 / 254 };
      }
      if (has(257)) {
        kind = "lock"; const lock = at(257, 0);
        state.lockState = ["未完全锁定", "已上锁", "已解锁", "门闩已释放"][lock] ?? "未知";
        if (lock === 1 || lock === 2) state.lock = lock === 1;
        if (at(257, 2) !== false && supports(257, 0) && supports(257, 1)) controls.lock = true;
      }
      if (has(258)) {
        kind = "cover";
        if ((features(258) & 5) === 5 && supports(258, 5)) controls.position = range(0, 100);
        if (integer(at(258, 14), 0, 10000)) state.position = 100 - at(258, 14) / 100;
        else if (integer(at(258, 8), 0, 100)) state.position = 100 - at(258, 8);
      }
      if (has(513)) {
        kind = "thermostat";
        const f = features(513, undefined), seq = at(513, 27), known = integer(seq, 0, 5);
        const heat = f != null ? f & 1 : known && seq >= 2;
        const coolSeq = known && (seq <= 1 || seq >= 4);
        const cool = f != null ? f & 2 : coolSeq;
        if (known) controls.thermostatMode = ["off", ...(heat && seq >= 2 ? ["heat"] : []), ...(cool && coolSeq ? ["cool"] : []),
          ...(heat && cool && (f & 32) && seq >= 4 ? ["auto"] : [])];
        const mode = modeOf(thermostat, at(513, 28)); if (mode) state.thermostatMode = mode;
        const setpoint = (name, supported, attribute, absMin, absMax, userMin, userMax, fallbackMin, fallbackMax) => {
          const val = key => integer(at(513, key), -27315, 32767) ? at(513, key) : undefined;
          const current = val(attribute); if (current !== undefined) state[name] = current / 100;
          if (!supported) return;
          const lo = Math.max(val(absMin) ?? fallbackMin, val(userMin) ?? -Infinity);
          const hi = Math.min(val(absMax) ?? fallbackMax, val(userMax) ?? Infinity);
          if (lo <= hi) controls[name] = range(lo / 100, hi / 100, .1);
        };
        setpoint("heatSetpoint", heat, 18, 3, 4, 21, 22, 700, 3000);
        setpoint("coolSetpoint", cool, 17, 5, 6, 23, 24, 1600, 3200);
        const band = at(513, 25);
        if (integer(band, 0, 127)) state.deadband = band / 10;
        else if (f & 32) state.deadband = 2;
      }
      if (has(514)) {
        if (kind !== "thermostat") kind = "fan";
        controls.fanPercent = range(0, 100);
        const seq = at(514, 1), mode = ["off", "low", "medium", "high", "on", "auto", "smart"][at(514, 0)];
        if (mode) state.fanMode = mode;
        if (integer(at(514, 2), 0, 100)) state.fanPercent = at(514, 2);
        if (integer(seq, 0, 5)) controls.fanMode = ["off", ...(seq < 4 ? ["low"] : []), ...([0, 2].includes(seq) ? ["medium"] : []), "high",
          ...(seq >= 2 && seq <= 4 && (features(514, 2) & 2) ? ["auto"] : [])];
      }
      const sensor = (name, cluster, attr, lo, hi, divisor = 1) => {
        if (has(cluster) && finite(at(cluster, attr), lo, hi)) state[name] = at(cluster, attr) / divisor;
      };
      sensor("temperature", 1026, 0, -27315, 32767, 100); sensor("temperature", 513, 0, -27315, 32767, 100);
      sensor("humidity", 1029, 0, 0, 10000, 100); sensor("battery", 47, 12, 0, 200, 2);
      sensor("airQuality", 91, 0, 1, 6);
      if (at(0x40d, 8) === 0) sensor("co2", 0x40d, 0, 0, Infinity);
      if (at(0x42a, 8) === 4) sensor("pm25", 0x42a, 0, 0, Infinity);
      sensor("powerWatts", 144, 8, -Infinity, Infinity, 1000);
      const energy = at(145, 1)?.energy ?? at(145, 1)?.["0"];
      if (has(145) && finite(energy, 0, Infinity)) state.energyKwh = energy / 1000000;
      if (has(69) && typeof at(69, 0) === "boolean") state[types.includes(0x15) ? "contact" : "booleanState"] = at(69, 0);
      if (has(1030) && integer(at(1030, 0), 0, 255)) state.occupancy = !!(at(1030, 0) & 1);
      if (has(1024) && integer(at(1024, 0), 0, 65534)) state.illuminance = at(1024, 0) ? 10 ** ((at(1024, 0) - 1) / 10000) : 0;
      if (!Object.keys(controls).length && [1026, 1029, 69, 1030, 1024, 91, 0x40d, 0x42a].some(has)) kind = "sensor";
      const camera = types.includes(0x142) || has(0x551) && has(0x553);
      if (camera) kind = "camera";
      devices.push({ key: `${nodeId}/${endpointId}`, nodeId, endpointId, name: display, nodeName, available, kind, camera, state, controls });
    }
  }
  return devices;
}

export function formatState(action, value) {
  if (value === undefined) return "未知";
  if (action === "lock") return value ? "已上锁" : "已解锁";
  if (action === "contact") return value ? "已关闭" : "已打开";
  if (action === "occupancy") return value ? "有人" : "无人";
  if (typeof value === "boolean") return value ? "开启" : "关闭";
  if (typeof value === "string") return modes[value] || value;
  if (action === "color") return `${Math.round(value.hue)}° · ${Math.round(value.saturation)}%`;
  if (action === "colorTemperature") return `${Math.round(1000000 / value)} K`;
  if (action === "airQuality") return ["未知", "优", "良", "中等", "差", "很差", "极差"][value] || "未知";
  const units = { temperature: " °C", coolSetpoint: " °C", heatSetpoint: " °C", energyKwh: " kWh", powerWatts: " W",
    illuminance: " lx", co2: " ppm", pm25: " µg/m³" };
  return new Intl.NumberFormat("zh-CN", { maximumFractionDigits: 2 }).format(value) + (units[action] ?? "%");
}
export const readings = device => sensorKeys.filter(key => key in device.state)
  .map(key => `${labels[key]} ${formatState(key, device.state[key])}`);

export function controlRequest(device, action, value) {
  const fail = () => { throw Error("设备离线、不支持此操作，或目标值超出允许范围。"); };
  if (!device?.available || !Object.hasOwn(device.controls, action)) return fail();
  const { nodeId: node_id, endpointId: endpoint_id } = device;
  const limit = device.controls[action];
  const command = (cluster_id, command_name, payload = {}) => ({ command: "device_command",
    args: { node_id, endpoint_id, cluster_id, command_name, payload, ...(cluster_id === 257 ? { timed_request_timeout_ms: 10000 } : {}) } });
  const write = (cluster, attribute, value) => ({ command: "write_attribute", args: { node_id, attribute_path: `${endpoint_id}/${cluster}/${attribute}`, value } });
  const transition = { transitionTime: 5, optionsMask: 0, optionsOverride: 0 };
  const numeric = (whole = false) => finite(value, limit.min, limit.max) && (!whole || Number.isInteger(value));
  if (action === "power" && typeof value === "boolean") return command(6, value ? "On" : "Off");
  if (action === "brightness" && numeric(true)) return command(8, "MoveToLevelWithOnOff", { level: round(value * 254 / 100), ...transition });
  if (action === "colorTemperature" && numeric(true)) return command(768, "MoveToColorTemperature", { colorTemperatureMireds: value, ...transition });
  if (action === "color" && value && finite(value.hue, 0, 360) && finite(value.saturation, 0, 100)) {
    const name = limit.enhanced ? "EnhancedMoveToHueAndSaturation" : "MoveToHueAndSaturation";
    const hue = value.hue % 360;
    return command(768, name, { [limit.enhanced ? "enhancedHue" : "hue"]: limit.enhanced ? round(hue * 65536 / 360) % 65536 : round(hue * 254 / 360),
      saturation: round(value.saturation * 254 / 100), ...transition });
  }
  if (action === "lock" && typeof value === "boolean") return command(257, value ? "LockDoor" : "UnlockDoor");
  if (action === "position" && numeric()) return command(258, "GoToLiftPercentage", { liftPercent100thsValue: round((100 - value) * 100) });
  if (action === "thermostatMode" && limit.includes(value)) return write(513, 28, thermostat[value]);
  if (action === "fanMode" && limit.includes(value)) return write(514, 0, fan[value]);
  if (action === "fanPercent" && numeric(true)) return write(514, 2, value);
  if (["heatSetpoint", "coolSetpoint"].includes(action) && numeric()) {
    const heat = action === "heatSetpoint", other = device.state[heat ? "coolSetpoint" : "heatSetpoint"], band = device.state.deadband;
    if (band !== undefined && (other === undefined || (heat ? value + band > other + .00001 : value - band < other - .00001)))
      throw Error("两个设定温度不满足恒温器的温差限制。");
    return write(513, heat ? 18 : 17, round(value * 100));
  }
  return fail();
}
export function writeSucceeded(request, response) {
  if (request.command !== "write_attribute") return true;
  const [, cluster, attribute] = request.args.attribute_path.split("/").map(Number);
  const [endpoint] = request.args.attribute_path.split("/").map(Number);
  const entry = response?.[0];
  return Array.isArray(response) && response.length === 1 && entry?.Status === 0 &&
    entry.Path?.ClusterId === cluster && entry.Path?.AttributeId === attribute && entry.Path?.EndpointId === endpoint;
}
