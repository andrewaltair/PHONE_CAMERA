// MyCam protocol v3 for the iPhone web page (protocol/PROTOCOL.md, docs/web-camera-format.md).
// One MyCam packet per binary WebSocket message, big-endian. Checked against protocol/golden.txt by
// pc/tools/webtest/golden_check.js (run by ctest when Node.js is installed).
"use strict";

(function (root) {
  const MAGIC_PACKET = 0x4D43414D; // "MCAM"
  const MAGIC_COMMAND = 0x4D434D44; // "MCMD"
  const VERSION = 3;
  const TYPE = { HELLO: 0, CONFIG: 1, FRAME: 2, ORIENT: 3, STATE: 4, LOG: 5, CAMERA: 6 };
  const CMD = { HELLO: 1, START: 2, STOP: 3, KEYFRAME: 4, SET_FACING: 5, PAUSE: 6, RESUME: 7,
                SET_QUALITY: 8, SET_FPS: 9, SET_ZOOM: 10, SET_EXPOSURE: 11, SET_TORCH: 12, SET_FOCUS: 13 };
  const STATE = { IDLE: 0, STREAMING: 1, ERROR: 2, PAUSED: 3 };
  const FACING = { BACK: 0, FRONT: 1 };
  // CAMERA flags and per-quality frame-rate bits.
  const CAM = { TORCH_AVAILABLE: 0x01, TORCH_ON: 0x02, FOCUS_LOCKED: 0x04, HAS_60: 0x08, HAS_4K: 0x10,
                AUTOFOCUS: 0x20, HAS_120: 0x40 };
  const FPS_BIT = { 30: 0x01, 60: 0x02, 120: 0x04 };
  // Quality index -> landscape size (the PROTOCOL's 720p / 1080p / 4K).
  const QUALITY_SIZE = [[1280, 720], [1920, 1080], [3840, 2160]];
  const MAX_PAYLOAD = 8 * 1024 * 1024;

  function packet(type, flags, ptsUs, payload) {
    if (payload.length > MAX_PAYLOAD) throw new Error("payload too big");
    const buf = new ArrayBuffer(20 + payload.length);
    const dv = new DataView(buf);
    dv.setUint32(0, MAGIC_PACKET);
    dv.setUint8(4, type);
    dv.setUint8(5, flags);
    dv.setUint16(6, 0);
    dv.setBigInt64(8, BigInt(ptsUs));
    dv.setUint32(16, payload.length);
    new Uint8Array(buf, 20).set(payload);
    return buf;
  }

  function u16(v) {
    const p = new Uint8Array(2);
    new DataView(p.buffer).setUint16(0, v);
    return p;
  }

  const hello = (version = VERSION) => packet(TYPE.HELLO, 0, 0, u16(version));

  function config(width, height, sensorOrientation, facing, spsPps) {
    const p = new Uint8Array(7 + spsPps.length);
    const dv = new DataView(p.buffer);
    dv.setUint16(0, width);
    dv.setUint16(2, height);
    dv.setUint16(4, sensorOrientation);
    dv.setUint8(6, facing);
    p.set(spsPps, 7);
    return packet(TYPE.CONFIG, 0, 0, p);
  }

  const frame = (keyFrame, ptsUs, data) => packet(TYPE.FRAME, keyFrame ? 1 : 0, ptsUs, data);
  const orient = (degrees) => packet(TYPE.ORIENT, 0, 0, u16(degrees));
  const state = (s, facing) => packet(TYPE.STATE, 0, 0, Uint8Array.of(s, facing));
  const log = (text) => packet(TYPE.LOG, 0, 0, new TextEncoder().encode(text));

  // c: { quality, fps, zoomX100, zoomMinX100, zoomMaxX100, ev, evMin, evMax, evStepX100, flags, width, height,
  //      actualFps, masks: [720p, 1080p, 4K] } - without masks it is the 18-byte (pre-1.3.2) form.
  function camera(c) {
    const p = new Uint8Array(c.masks ? 21 : 18);
    const dv = new DataView(p.buffer);
    dv.setUint8(0, c.quality);
    dv.setUint8(1, c.fps);
    dv.setUint16(2, c.zoomX100);
    dv.setUint16(4, c.zoomMinX100);
    dv.setUint16(6, c.zoomMaxX100);
    dv.setInt8(8, c.ev);
    dv.setInt8(9, c.evMin);
    dv.setInt8(10, c.evMax);
    dv.setUint8(11, c.evStepX100);
    dv.setUint8(12, c.flags);
    dv.setUint16(13, c.width);
    dv.setUint16(15, c.height);
    dv.setUint8(17, c.actualFps);
    if (c.masks) for (let q = 0; q < 3; q++) dv.setUint8(18 + q, c.masks[q]);
    return packet(TYPE.CAMERA, 0, 0, p);
  }

  // A PC command, or null if it isn't one (wrong size, magic, number or reserved bytes).
  function parseCommand(buf) {
    if (!(buf instanceof ArrayBuffer) || buf.byteLength !== 8) return null;
    const dv = new DataView(buf);
    if (dv.getUint32(0) !== MAGIC_COMMAND || dv.getUint16(6) !== 0) return null;
    const cmd = dv.getUint8(4);
    if (cmd < CMD.HELLO || cmd > CMD.SET_FOCUS) return null;
    return { cmd, arg: dv.getUint8(5) };
  }

  // The SPS (7) and PPS (8) NAL units, with their start codes, from the start of an Annex-B key frame.
  function extractSpsPps(data) {
    const d = data.subarray(0, Math.min(data.length, 1024));
    const starts = [];
    for (let i = 0; i + 3 < d.length; i++) {
      if (d[i] !== 0 || d[i + 1] !== 0) continue;
      if (d[i + 2] === 1) { starts.push({ pos: i, body: i + 3 }); i += 2; }
      else if (d[i + 2] === 0 && d[i + 3] === 1) { starts.push({ pos: i, body: i + 4 }); i += 3; }
    }
    const parts = [];
    for (let k = 0; k < starts.length; k++) {
      const type = d[starts[k].body] & 0x1F;
      if (type !== 7 && type !== 8) continue;
      parts.push(d.slice(starts[k].pos, k + 1 < starts.length ? starts[k + 1].pos : d.length));
    }
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
  }

  // H.264 level for a size and rate (Baseline profile, as tested on the iPhone 13).
  function codecFor(width, height, fps) {
    const px = width * height;
    if (px <= 1280 * 720) return fps <= 30 ? "avc1.42001f" : "avc1.420020";
    if (px <= 1920 * 1088) return fps <= 30 ? "avc1.420028" : "avc1.42002a";
    return fps <= 30 ? "avc1.420033" : "avc1.420034";
  }

  // The bitrate to start at; the page lowers it under back-pressure.
  function bitrateFor(width, height, fps) {
    const px = width * height;
    const base = px >= 3840 * 2160 ? 20e6 : px >= 1920 * 1080 ? 8e6 : 4e6;
    return Math.round(fps >= 60 ? base * 1.5 : base);
  }

  const api = { VERSION, TYPE, CMD, STATE, FACING, CAM, FPS_BIT, QUALITY_SIZE, MAX_PAYLOAD,
                packet, hello, config, frame, orient, state, log, camera, parseCommand, extractSpsPps,
                codecFor, bitrateFor };
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  else root.MyCamProtocol = api;
})(typeof self !== "undefined" ? self : globalThis);
