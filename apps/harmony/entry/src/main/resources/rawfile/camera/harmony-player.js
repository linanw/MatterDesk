"use strict";

const video = document.getElementById("video");
const placeholder = document.getElementById("placeholder");
let waitingForFrame = false;
let firstFrameTimer;
let audioAvailable = false;
const FIRST_FRAME_ERROR = "等待摄像头画面超时，请重试。";

const native = {
  request(token, command, args) {
    if (!window.CameraHost.send(token, command, JSON.stringify(args))) {
      player.response(token, command, false, null, {});
    }
  },
  playerState(state, detail, _talk, _mic, audio) {
    audioAvailable = audio;
    if (state === "streaming" && waitingForFrame && !video.videoWidth) {
      placeholder.textContent = "已连接，正在等待第一帧画面…";
      placeholder.classList.remove("hidden");
      window.CameraHost.report("connecting", placeholder.textContent, false);
      return;
    }
    if (state !== "connecting") clearFirstFrame();
    window.CameraHost.report(state, state === "streaming" ? "直播中" : detail, audio);
  },
  // The direct Harmony client has no microphone permission or talk button.
  microphoneState() {}
};
const player = new MatterDeskCameraLogic.CameraPlayer({
  native, video, placeholder,
  getMicrophone: () => Promise.reject(new Error("Harmony direct mode is receive-only")),
  makeKey() {
    return Array.from(crypto.getRandomValues(new Uint8Array(16)), n => n.toString(16).padStart(2, "0")).join("");
  }
});

let mock = null;
let closing = false;

function clearFirstFrame() {
  waitingForFrame = false;
  clearTimeout(firstFrameTimer);
}

function notifyFirstFrame() {
  if (!waitingForFrame || closing || !video.videoWidth || player.state !== "streaming") return;
  clearFirstFrame();
  placeholder.classList.add("hidden");
  window.CameraHost.report("streaming", "直播中", audioAvailable);
}

for (const event of ["loadeddata", "playing", "resize"]) video.addEventListener(event, notifyFirstFrame);

function startLive() {
  if (closing || waitingForFrame || player.state === "streaming") return;
  waitingForFrame = true;
  firstFrameTimer = setTimeout(async () => {
    clearFirstFrame();
    await player.stop(FIRST_FRAME_ERROR);
    if (!closing) window.CameraHost.report("error", FIRST_FRAME_ERROR, false);
  }, 55_000);
  return player.start();
}

function receiveOnlyCapabilities(result) {
  // Keep the shared player from negotiating an unused sender. A mic callback
  // alone would still request talkback from the camera during stream setup.
  return result && typeof result === "object" && result.audio ? {
    ...result, audio: { ...result.audio, two_way_talk_support: null }
  } : result;
}

function startMock() {
  const canvas = document.createElement("canvas");
  canvas.width = 960;
  canvas.height = 540;
  const context = canvas.getContext("2d");
  const start = performance.now();
  const draw = () => {
    const seconds = (performance.now() - start) / 1000;
    context.fillStyle = "#111e36";
    context.fillRect(0, 0, 960, 540);
    const colors = ["#f8c258", "#ed756a", "#9067e4", "#407de0", "#3dc0a0", "#d8e6f5"];
    colors.forEach((color, index) => {
      context.fillStyle = color;
      context.fillRect(48 + index * 144, 175, 132, 176);
    });
    context.fillStyle = "#e8edf8";
    context.font = "bold 34px sans-serif";
    context.fillText("MOCK CAMERA", 48, 84);
    context.font = "23px monospace";
    context.fillText(`SYNTHETIC FEED  /  ${seconds.toFixed(1)} s`, 48, 115);
    context.fillStyle = "#fbfbff";
    context.fillRect(48 + (seconds * 144) % 832, 410, 36, 36);
    context.font = "18px monospace";
    context.fillText("MATTERDESK  ·  PLAYBACK TEST", 48, 494);
  };
  draw();
  const stream = canvas.captureStream(20);
  mock = { stream, interval: setInterval(draw, 50) };
  video.onplaying = () => {
    if (!mock || closing) return;
    placeholder.classList.add("hidden");
    window.CameraHost.report("streaming", "模拟画面播放中", false);
  };
  video.srcObject = stream;
  video.play().catch(() => {
    stopMock();
    window.CameraHost.report("error", "模拟画面播放失败，请重试。", false);
  });
}

function stopMock() {
  if (!mock) return;
  clearInterval(mock.interval);
  for (const track of mock.stream.getTracks()) track.stop();
  mock = null;
  video.onplaying = null;
  video.pause();
  video.srcObject = null;
}

window.harmonyCamera = {
  async open(config) {
    if (closing) return;
    if (config.mock) {
      try { startMock(); }
      catch (_) { stopMock(); window.CameraHost.report("error", "此 Web 组件无法创建模拟视频。", false); }
      return;
    }
    if (typeof RTCPeerConnection !== "function") {
      window.CameraHost.report("error", "此 Web 组件不支持 WebRTC。", false);
      return;
    }
    player.configure(config);
    await player.controllerConnected(true);
    if (!closing) await startLive();
  },
  restart() { startLive()?.catch(() => {}); },
  response(token, command, success, result, error) {
    const caps = success && command === "camera_get_capabilities" ? receiveOnlyCapabilities(result) : result;
    player.response(token, command, success, caps, error);
  },
  event(...args) { player.event(...args); },
  sound(enabled) { player.setSound(enabled); },
  async stop() {
    closing = true;
    clearFirstFrame();
    stopMock();
    await player.stop();
    player.dispose();
    window.CameraHost.finished();
  },
  inspect() { return { width: video.videoWidth, height: video.videoHeight, time: video.currentTime }; }
};

window.addEventListener("pagehide", () => {
  closing = true;
  clearFirstFrame();
  stopMock();
  player.dispose();
});
