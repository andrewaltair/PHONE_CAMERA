// Checks the iPhone page's protocol code (pc/companion/web/protocol.js) against protocol/golden.txt.
// Run: node pc/tools/webtest/golden_check.js [path/to/golden.txt]   (ctest runs it when Node.js is found)
"use strict";

const fs = require("fs");
const path = require("path");
const P = require(path.join(__dirname, "..", "..", "companion", "web", "protocol.js"));

const goldenPath = process.argv[2] || path.join(__dirname, "..", "..", "..", "protocol", "golden.txt");
const golden = {};
for (const raw of fs.readFileSync(goldenPath, "utf8").split("\n")) {
  const line = raw.replace(/#.*$/, "").trim();
  const m = /^([A-Za-z0-9_.]+)\s*=\s*(.+)$/.exec(line);
  if (m) golden[m[1]] = m[2].replace(/\s+/g, "").toUpperCase();
}

const hex = (buf) => Buffer.from(buf instanceof ArrayBuffer ? new Uint8Array(buf) : buf).toString("hex").toUpperCase();
const toArrayBuffer = (h) => { const b = Buffer.from(h, "hex"); return b.buffer.slice(b.byteOffset, b.byteOffset + b.length); };

const packets = {
  "packet.hello": () => P.hello(1), // golden.txt uses protocol version 1 here; the page sends 3
  "packet.config": () => P.config(0x780, 0x438, 0x5A, 1, Uint8Array.of(0, 0, 0, 1, 0x67)),
  "packet.frame": () => P.frame(true, 0x0102030405060708n, Uint8Array.of(0xDE, 0xAD, 0xBE, 0xEF)),
  "packet.orient": () => P.orient(270),
  "packet.state": () => P.state(P.STATE.STREAMING, P.FACING.FRONT),
  "packet.state_paused": () => P.state(P.STATE.PAUSED, P.FACING.BACK),
  "packet.log": () => P.log("hi"),
  "packet.camera": () => P.camera({ quality: 1, fps: 30, zoomX100: 100, zoomMinX100: 60, zoomMaxX100: 1000, ev: 0,
    evMin: -12, evMax: 12, evStepX100: 33, flags: P.CAM.TORCH_AVAILABLE | P.CAM.HAS_60 | P.CAM.HAS_4K,
    width: 1920, height: 1080, actualFps: 30 }),
  "packet.camera_modes": () => P.camera({ quality: 1, fps: 120, zoomX100: 100, zoomMinX100: 100, zoomMaxX100: 1000,
    ev: 0, evMin: -12, evMax: 12, evStepX100: 33,
    flags: P.CAM.TORCH_AVAILABLE | P.CAM.HAS_60 | P.CAM.HAS_4K | P.CAM.HAS_120, width: 1920, height: 1080,
    actualFps: 120, masks: [0x07, 0x07, 0x01] }),
};
const commands = {
  "command.hello": [P.CMD.HELLO, 0], "command.start": [P.CMD.START, 0], "command.stop": [P.CMD.STOP, 0],
  "command.keyframe": [P.CMD.KEYFRAME, 0], "command.facing_front": [P.CMD.SET_FACING, 1],
  "command.pause": [P.CMD.PAUSE, 0], "command.resume": [P.CMD.RESUME, 0],
  "command.quality_4k": [P.CMD.SET_QUALITY, 2], "command.fps_120": [P.CMD.SET_FPS, 120],
  "command.fps_60": [P.CMD.SET_FPS, 60], "command.zoom_2x": [P.CMD.SET_ZOOM, 20],
  "command.exposure_m2": [P.CMD.SET_EXPOSURE, 0xFE], "command.torch_on": [P.CMD.SET_TORCH, 1],
  "command.focus_lock": [P.CMD.SET_FOCUS, 1],
};

let pass = 0, fail = 0;
const check = (ok, what) => { if (ok) pass++; else { fail++; console.log("FAIL " + what); } };
for (const [name, value] of Object.entries(golden)) {
  if (name.startsWith("packet.")) {
    if (!(name in packets)) { check(false, name + ": golden.txt has a packet the page doesn't build"); continue; }
    const mine = hex(packets[name]());
    check(mine === value, name + "\n  golden " + value + "\n  page   " + mine);
  } else if (name.startsWith("command.")) {
    if (!(name in commands)) { check(false, name + ": golden.txt has a command the page doesn't know"); continue; }
    const c = P.parseCommand(toArrayBuffer(value));
    check(c && c.cmd === commands[name][0] && c.arg === commands[name][1], name + " parsed as " + JSON.stringify(c));
  }
}
// Things the page must refuse.
check(P.parseCommand(toArrayBuffer("4D434D4402000001")) === null, "reserved bytes must be zero");
check(P.parseCommand(toArrayBuffer("4D434D4463000000")) === null, "unknown command");
check(P.parseCommand(toArrayBuffer("4D43414D02000000")) === null, "wrong magic");
check(P.parseCommand(new ArrayBuffer(9)) === null, "wrong size");
// SPS/PPS come out of a key frame, nothing else does.
const key = Uint8Array.from(Buffer.from("00000001674200 0000000168CE 0000016588".replace(/ /g, ""), "hex"));
check(hex(P.extractSpsPps(key)) === "000000016742000000000168CE", "extractSpsPps " + hex(P.extractSpsPps(key)));

console.log(pass + " pass, " + fail + " fail");
process.exit(fail ? 1 : 0);
