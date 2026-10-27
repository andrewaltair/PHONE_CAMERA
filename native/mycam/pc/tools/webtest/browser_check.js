// Developer test: runs the real iPhone page in headless Chrome/Edge with a fake camera, against a running
// server (web_probe or the companion). It opens the pairing address, taps Start and reports what the page
// shows, so the page's own code (WebCodecs, pairing, the protocol) is exercised end to end.
//
//   node pc/tools/webtest/browser_check.js "<pairing address>" [seconds] [path to chrome or msedge]
"use strict";
const { spawn } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const url = process.argv[2];
const seconds = Number(process.argv[3] || 15);
const browser = process.argv[4] || "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
const port = 9333;
const profile = fs.mkdtempSync(path.join(os.tmpdir(), "mycam-browser-"));

const proc = spawn(browser, [
  "--headless=new", `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`, "--ignore-certificate-errors",
  "--use-fake-device-for-media-stream", "--use-fake-ui-for-media-stream", "--autoplay-policy=no-user-gesture-required",
  "--no-first-run", "--no-default-browser-check", url,
], { stdio: "ignore" });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  let target;
  for (let i = 0; i < 50 && !target; i++) {
    await sleep(200);
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
      target = list.find((t) => t.type === "page");
    } catch (e) { /* not up yet */ }
  }
  if (!target) throw new Error("the browser didn't start");
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => { ws.onopen = r; });
  let id = 0;
  const pending = new Map();
  ws.onmessage = (e) => {
    const m = JSON.parse(e.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); }
    if (m.method === "Runtime.consoleAPICalled") console.log("console:", m.params.args.map((a) => a.value).join(" "));
    if (m.method === "Runtime.exceptionThrown") console.log("EXCEPTION:", JSON.stringify(m.params.exceptionDetails).slice(0, 400));
  };
  const call = (method, params = {}) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
  const evalJs = async (expr) => (await call("Runtime.evaluate", { expression: expr, returnByValue: true })).result.value;
  await call("Runtime.enable");
  const screen = () => evalJs(`document.getElementById("headline").textContent + " | " + document.getElementById("sentence").textContent + " | " + document.getElementById("facts").textContent`);

  await sleep(2500);
  console.log("before Start:", await screen());
  console.log("address now:", await evalJs("location.href"));
  await evalJs(`document.getElementById("start").click()`);
  for (let t = 0; t < seconds; t++) {
    await sleep(1000);
    console.log(`${t + 1}s:`, await screen());
  }
  ws.close();
})().catch((e) => console.log("FAIL", e.message)).finally(() => {
  proc.kill();
  setTimeout(() => { try { fs.rmSync(profile, { recursive: true, force: true }); } catch (e) { /* in use */ } process.exit(0); }, 500);
});
