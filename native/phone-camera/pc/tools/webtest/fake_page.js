// Developer test: plays the iPhone page against a running server (web_probe or the companion), using the
// same protocol.js. Run web_probe, then:  node pc/tools/webtest/fake_page.js "<pairing address>"
// Checks the pages are served, pairing (and that a code works only once), /check, a refused WebSocket
// without a token, then streams fake frames for the PC's START until the PC says STOP.
"use strict";
process.env.NODE_TLS_REJECT_UNAUTHORIZED = "0"; // The PC's certificate is self-signed.
const path = require("path");
const P = require(path.join(__dirname, "..", "..", "companion", "web", "protocol.js"));

const url = new URL(process.argv[2]);
const base = url.origin;
const code = url.searchParams.get("t");
let fails = 0;
const check = (ok, what) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) fails++; };

(async () => {
  for (const p of ["/", "/app.js", "/protocol.js", "/style.css", "/icon.png"]) {
    const r = await fetch(base + p);
    check(r.status === 200 && (await r.arrayBuffer()).byteLength > 100, "GET " + p + " -> " + r.status);
  }
  check((await fetch(base + "/nope")).status === 404, "unknown path -> 404");
  const r1 = await fetch(base + "/pair", { method: "POST", body: new URLSearchParams({ t: code, name: "Test page" }) });
  const j = await r1.json();
  check(r1.status === 200 && typeof j.device === "string" && j.device.length === 43, "pair -> device token");
  const r2 = await fetch(base + "/pair", { method: "POST", body: new URLSearchParams({ t: code, name: "Again" }) });
  check(r2.status === 403, "the same code a second time -> 403");
  const c1 = await (await fetch(base + "/check", { method: "POST", body: new URLSearchParams({ d: j.device }) })).json();
  check(c1.ok === true && c1.busy === false && c1.name === "Test page", "check -> " + JSON.stringify(c1));
  check((await fetch(base + "/check", { method: "POST", body: new URLSearchParams({ d: "x" + j.device }) })).status === 403,
        "check with a wrong token -> 403");

  await new Promise((resolve) => {
    const bad = new WebSocket(base.replace("https", "wss") + "/ws?d=wrong");
    bad.onopen = () => { check(false, "WebSocket without a valid token was accepted"); bad.close(); resolve(); };
    bad.onerror = () => { check(true, "WebSocket without a valid token refused"); resolve(); };
  });

  const ws = new WebSocket(base.replace("https", "wss") + "/ws?d=" + encodeURIComponent(j.device));
  ws.binaryType = "arraybuffer";
  let timer = null, frames = 0, gotStop = false;
  const sps = Uint8Array.of(0, 0, 0, 1, 0x67, 0x42, 0, 0x28, 0, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80);
  ws.onmessage = (e) => {
    const c = P.parseCommand(e.data);
    if (!c) { check(false, "bad command from the PC"); return; }
    console.log("PC -> page: " + Object.keys(P.CMD).find((k) => P.CMD[k] === c.cmd) + " " + c.arg);
    if (c.cmd === P.CMD.HELLO) {
      ws.send(P.hello()); ws.send(P.state(0, 0)); ws.send(P.orient(0)); ws.send(P.log("fake page here"));
      ws.send(P.camera({ quality: 1, fps: 30, zoomX100: 100, zoomMinX100: 100, zoomMaxX100: 500, ev: 0, evMin: 0, evMax: 0,
        evStepX100: 0, flags: P.CAM.HAS_60, width: 0, height: 0, actualFps: 0, masks: [3, 3, 1] }));
    } else if (c.cmd === P.CMD.START || c.cmd === P.CMD.KEYFRAME) {
      ws.send(P.config(1080, 1920, 0, 0, sps));
      if (c.cmd === P.CMD.START) ws.send(P.state(1, 0));
      if (!timer) timer = setInterval(() => {
        frames++;
        const big = new Uint8Array(frames % 30 === 1 ? 200000 : 20000); // Key frames are big: tests 64-bit lengths.
        big.set(Uint8Array.of(0, 0, 0, 1, frames % 30 === 1 ? 0x65 : 0x41));
        ws.send(P.frame(frames % 30 === 1, frames * 33333, big));
      }, 33);
    } else if (c.cmd === P.CMD.STOP) {
      gotStop = true;
      clearInterval(timer);
      ws.send(P.state(0, 0));
      setTimeout(() => ws.close(1000), 300);
    }
  };
  await new Promise((resolve) => { ws.onclose = resolve; });
  check(gotStop && frames > 100, "streamed " + frames + " frames until STOP");
  console.log(fails ? fails + " FAILED" : "all good");
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log("FAIL " + e); process.exit(1); });
