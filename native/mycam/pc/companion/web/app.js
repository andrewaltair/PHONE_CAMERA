// MyCam web camera: an iPhone (or any modern browser) as the camera of MyCam on a Windows PC.
// Served by the PC companion. Camera -> WebCodecs H.264 (hardware) -> MyCam protocol over wss://.
// docs/web-camera-format.md describes the messages; docs/AIYAN-IPHONE.md the behaviour.
"use strict";

(function () {
  const P = window.MyCamProtocol;
  const $ = (id) => document.getElementById(id);
  const ui = {
    hero: $("hero"), headline: $("headline"), sentence: $("sentence"), facts: $("facts"), progress: $("progress"),
    start: $("start"), pause: $("pause"), pauseTitle: $("pauseTitle"), pauseHint: $("pauseHint"),
    cameraGroup: $("cameraGroup"), tipsGroup: $("tipsGroup"), dim: $("dim"), dimScreen: $("dimScreen"),
    video: $("video"),
  };
  const facingInputs = Array.from(document.querySelectorAll('input[name="facing"]'));

  // --- Storage (each wrapped: private mode or blocked storage must not break the page) -------------
  const store = {
    get(k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
    set(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* ignore */ } },
    del(k) { try { localStorage.removeItem(k); } catch (e) { /* ignore */ } },
  };

  // --- State --------------------------------------------------------------------------------------
  let deviceToken = store.get("mycam.device");
  let ws = null;
  let connected = false;      // The WebSocket is open.
  let helloDone = false;      // The PC said HELLO.
  let started = false;        // The user tapped Start (camera allowed, wake lock asked for).
  let wantStream = false;     // The PC said START and hasn't said STOP.
  let userPaused = store.get("mycam.paused") === "1"; // Survives reconnects, like the Android app's pause.
  let hiddenPaused = false;   // iOS took the camera away (page hidden / phone locked).
  let facing = store.get("mycam.facing") === "1" ? P.FACING.FRONT : P.FACING.BACK;
  let quality = 1;            // 1080p
  let fps = 30;
  let phoneState = P.STATE.IDLE;
  let capture = null;
  let queue = Promise.resolve();
  let reconnectDelay = 1000;
  let reconnectTimer = 0;
  let stopReason = "";        // Why the page stopped trying (forgotten, replaced...).
  let wake = null;
  const caps = {};            // Per facing: { masks: [3], zoom: {min,max}|null, torch: bool }
  let zoom = 1;
  let torch = false;
  let lastTs = 0;
  const tBase = performance.now();

  const nowUs = () => {
    let ts = Math.round((performance.now() - tBase) * 1000);
    if (ts <= lastTs) ts = lastTs + 1; // Timestamps only ever go up, across camera restarts too.
    lastTs = ts;
    return ts;
  };

  // --- Sending ------------------------------------------------------------------------------------
  const send = (buf) => { if (ws && ws.readyState === WebSocket.OPEN) ws.send(buf); };
  // Lines from before the PC said HELLO (the camera check at Start, say) wait for it, so they reach the log.
  const earlyLogs = [];
  const sendLog = (text) => {
    if (helloDone && ws && ws.readyState === WebSocket.OPEN) send(P.log(text));
    else if (earlyLogs.length < 20) earlyLogs.push(text);
  };
  const sendState = () => send(P.state(phoneState, facing));
  function setState(s) {
    phoneState = s;
    sendState();
    render();
  }

  // The CAMERA packet: what this camera and browser can do, for the PC's settings to grey out the rest.
  function sendCamera() {
    const c = caps[facing];
    const masks = c ? c.masks : [P.FPS_BIT[30], P.FPS_BIT[30], 0];
    let flags = 0;
    if (c && c.torch) flags |= P.CAM.TORCH_AVAILABLE;
    if (torch) flags |= P.CAM.TORCH_ON;
    if (masks.some((m) => m & P.FPS_BIT[60])) flags |= P.CAM.HAS_60;
    if (masks.some((m) => m & P.FPS_BIT[120])) flags |= P.CAM.HAS_120;
    if (masks[2]) flags |= P.CAM.HAS_4K;
    const z = c && c.zoom ? c.zoom : { min: 1, max: 1 };
    send(P.camera({
      quality: effectiveMode().q, fps, // The quality that really runs; the asked rate (the PC applies the mask).
      zoomX100: Math.round(zoom * 100), zoomMinX100: Math.round(z.min * 100), zoomMaxX100: Math.round(z.max * 100),
      ev: 0, evMin: 0, evMax: 0, evStepX100: 0, // Safari has no exposure control.
      flags,
      width: capture ? capture.w : 0, height: capture ? capture.h : 0,
      actualFps: capture ? capture.measuredFps || capture.fps : 0,
      masks,
    }));
  }

  // --- What the camera and encoder can do (CLAUDE.md "Works on every phone") ---------------------
  async function encoderSupports(w, h, f) {
    try {
      const r = await VideoEncoder.isConfigSupported({
        codec: P.codecFor(w, h, f), width: w, height: h, framerate: f, bitrate: P.bitrateFor(w, h, f),
        latencyMode: "realtime", hardwareAcceleration: "prefer-hardware", avc: { format: "annexb" },
      });
      return !!r.supported;
    } catch (e) { return false; }
  }

  // Opens the camera once to read its limits, then works out the frame-rate mask of each quality.
  async function probe(which) {
    let stream;
    try {
      stream = await navigator.mediaDevices.getUserMedia({
        audio: false,
        video: { facingMode: { ideal: which === P.FACING.FRONT ? "user" : "environment" },
                 width: { ideal: 3840 }, height: { ideal: 2160 } },
      });
    } catch (e) {
      return { error: e };
    }
    const track = stream.getVideoTracks()[0];
    const tc = (track.getCapabilities && track.getCapabilities()) || {};
    stream.getTracks().forEach((t) => t.stop());
    const w = tc.width && tc.width.max ? tc.width.max : 1920;
    const h = tc.height && tc.height.max ? tc.height.max : 1080;
    const big = Math.max(w, h), small = Math.min(w, h);
    const fpsMax = tc.frameRate && tc.frameRate.max ? tc.frameRate.max : 30;
    const masks = [0, 0, 0];
    for (let q = 0; q < 3; q++) {
      const [qw, qh] = P.QUALITY_SIZE[q];
      if (big < qw || small < qh) continue;
      for (const f of [30, 60, 120]) {
        // 30 is the lowest rate the protocol knows, so it stands for "this size works"; 60 and 120 must be real.
        if (f > 30 && fpsMax + 0.5 < f) continue;
        if (await encoderSupports(qw, qh, f)) masks[q] |= P.FPS_BIT[f];
      }
    }
    // A camera smaller than 720p still works: it is offered as the lowest quality, at 30 fps.
    if (!masks[0] && !masks[1] && !masks[2]) masks[0] = P.FPS_BIT[30];
    const zoom = tc.zoom && tc.zoom.max > tc.zoom.min ? { min: tc.zoom.min, max: tc.zoom.max } : null;
    return { masks, zoom, torch: !!tc.torch, fpsMax, maxW: big, maxH: small };
  }

  async function ensureCaps(which) {
    if (caps[which]) return caps[which];
    const r = await probe(which);
    if (r.error) throw r.error;
    caps[which] = r;
    sendLog("camera " + (which ? "front" : "back") + ": up to " + r.maxW + "x" + r.maxH + ", modes 720p=" + r.masks[0] +
            " 1080p=" + r.masks[1] + " 4K=" + r.masks[2] + " (bits 1=30 2=60 4=120), max " + r.fpsMax + " fps, zoom " +
            (r.zoom ? r.zoom.min + "-" + r.zoom.max : "no") + ", torch " + (r.torch ? "yes" : "no"));
    return r;
  }

  // The quality and rate that will really run: the asked ones, or the best working ones below them.
  function effectiveMode() {
    const c = caps[facing];
    if (!c) return { q: Math.min(quality, 1), f: 30 };
    let q = quality;
    while (q > 0 && !c.masks[q]) q--;
    const m = c.masks[q] || P.FPS_BIT[30];
    const f = fps >= 120 && (m & P.FPS_BIT[120]) ? 120 : fps >= 60 && (m & P.FPS_BIT[60]) ? 60 : 30;
    return { q, f };
  }

  // --- Camera and encoder -------------------------------------------------------------------------
  async function startCapture() {
    if (capture || hiddenPaused || userPaused) return;
    try {
      await ensureCaps(facing);
    } catch (e) {
      cameraFailed(e);
      return;
    }
    const mode = effectiveMode();
    const [w, h] = P.QUALITY_SIZE[mode.q];
    let stream;
    try {
      stream = await navigator.mediaDevices.getUserMedia({
        audio: false,
        video: { facingMode: { ideal: facing === P.FACING.FRONT ? "user" : "environment" },
                 width: { ideal: w }, height: { ideal: h }, frameRate: { ideal: mode.f, max: mode.f } },
      });
    } catch (e) {
      cameraFailed(e);
      return;
    }
    // Things may have changed while the camera opened.
    if (capture || !wantStream || userPaused || hiddenPaused) {
      stream.getTracks().forEach((t) => t.stop());
      return;
    }
    ui.video.srcObject = stream;
    try { await ui.video.play(); } catch (e) { /* muted inline video plays without a gesture */ }
    if (!ui.video.videoWidth) {
      await new Promise((r) => ui.video.addEventListener("loadedmetadata", r, { once: true }));
    }
    const track = stream.getVideoTracks()[0];
    const c = {
      stream, track, stop: false, w: 0, h: 0, fps: mode.f, q: mode.q,
      needConfig: true, forceKey: true, reconfiguring: false, frames: 0,
      target: 0, bitrate: 0, skipped: 0, lastSkipAt: 0, lastDropAt: 0, lastRaiseAt: performance.now(),
      fpsWindowStart: performance.now(), fpsWindowFrames: 0, measuredFps: 0, fpsChecked: false,
    };
    c.enc = new VideoEncoder({ output: (chunk) => onChunk(c, chunk), error: (e) => sendLog("encoder error: " + (e.message || e)) });
    track.addEventListener("ended", () => {
      if (capture !== c) return;
      sendLog("the camera stopped (iOS took it away)");
      stopCapture().then(() => { if (!hiddenPaused) setState(P.STATE.ERROR); });
    });
    try {
      const first = new VideoFrame(ui.video, { timestamp: 0 });
      const g = geometry(first);
      first.close();
      configure(c, 0, g);
      sendLog("picture: " + g.text);
      sendOrient(); // Whether the PC adds the phone's turn depends on the camera's frames.
    } catch (e) {
      sendLog("encoder setup failed: " + (e.message || e));
      stream.getTracks().forEach((t) => t.stop());
      ui.video.srcObject = null;
      setState(P.STATE.ERROR);
      return;
    }
    capture = c;
    zoom = 1;
    torch = false;
    schedule(c);
  }

  // Which way up the picture is. The encoder takes the pixels as they are, so the turn they need goes to the
  // PC, which turns the picture (protocol: CONFIG sensor orientation + ORIENT, as for Android phones).
  //  - A browser that labels its frames (WebCodecs VideoFrame.rotation) gives the turn itself: ORIENT 0.
  //  - Safari on iPhone/iPad (no label, tested on iOS 27): the frames are always the camera's own landscape
  //    pixels, whichever way the phone is held. Like an Android camera, they need the sensor's turn (90° for
  //    both cameras) plus how far the phone is turned, which comes from the screen angle. Safari never turns
  //    its screen for an upside-down iPhone, so that one comes from the motion sensors (if allowed).
  //  - Other browsers (laptop webcams): the frames are upright as they are.
  const appleMobile = /iPhone|iPad|iPod/.test(navigator.userAgent) ||
                      (/Macintosh/.test(navigator.userAgent) && navigator.maxTouchPoints > 1);
  const screenAngle = () => {
    const a = screen.orientation && typeof screen.orientation.angle === "number" ? screen.orientation.angle
            : typeof window.orientation === "number" ? window.orientation : 0;
    return ((a % 360) + 360) % 360; // Counter-clockwise: 90 = turned left (top of the phone to the left).
  };
  let labelled = false; // The current camera's frames carry their own rotation.

  function geometry(frame) {
    const r = frame.visibleRect;
    const w = r ? r.width : frame.codedWidth, h = r ? r.height : frame.codedHeight;
    labelled = frame.rotation !== undefined && frame.rotation !== null;
    let rotation = 0, how = "upright as they come";
    if (labelled) {
      rotation = (((Number(frame.rotation) || 0) % 360) + 360) % 360;
      how = "from the frame's rotation label";
    } else if (appleMobile && w > h) {
      rotation = 90; // Both iPhone cameras (front tested as well: 270 put it upside down in every position).
      how = "camera sensor, plus the phone's turn in ORIENT";
    }
    return { w, h, rotation,
             text: w + "x" + h + ", sensor turn " + rotation + "° (" + how + ")" + (frame.flip ? ", flipped" : "") +
                   "; screen angle " + screenAngle() + "°" };
  }

  // ORIENT: how far the phone is turned clockwise from upright, when the PC must add it (see above).
  function deviceTurn() {
    if (labelled || !appleMobile) return 0;
    if (upsideDown) return 180;
    return (360 - screenAngle()) % 360;
  }

  // Upside-down portrait, from the motion sensors. beta and gamma swing wildly when the phone stands
  // upright (gimbal lock), so they are turned into which way "up" points across the screen: upY = sin(beta)
  // is 1 with the phone upright and -1 upside down, steady at any tilt. With a margin each way so it never
  // flickers near the edge.
  let upsideDown = false;
  function onMotion(e) {
    if (e.beta === null || e.gamma === null) return;
    const rad = Math.PI / 180;
    const upY = Math.sin(e.beta * rad);
    const upX = -Math.sin(e.gamma * rad) * Math.cos(e.beta * rad);
    const was = upsideDown;
    if (!upsideDown && upY < -0.7 && Math.abs(upX) < 0.5) upsideDown = true;
    else if (upsideDown && (upY > -0.4 || Math.abs(upX) > 0.7)) upsideDown = false;
    if (was !== upsideDown) sendOrient();
  }
  let motionAsked = false;
  // iOS asks the user first ("Motion & Orientation Access"); it must be asked straight from a tap.
  function askForMotion() {
    if (motionAsked || !appleMobile || typeof DeviceOrientationEvent === "undefined") return;
    motionAsked = true;
    const listen = () => window.addEventListener("deviceorientation", onMotion);
    if (typeof DeviceOrientationEvent.requestPermission === "function") {
      DeviceOrientationEvent.requestPermission()
        .then((r) => { if (r === "granted") listen(); else sendLog("motion sensors not allowed: upside-down isn't followed"); })
        .catch(() => { /* not asked from a tap: try again on the next Start */ motionAsked = false; });
    } else {
      listen();
    }
  }
  let lastTurn = -1;
  function sendOrient() {
    const t = deviceTurn();
    if (t === lastTurn) return;
    lastTurn = t;
    send(P.orient(t));
    if (helloDone) sendLog("phone turned: screen angle " + screenAngle() + "°" + (upsideDown ? ", upside down (motion sensors)" : "") + ", ORIENT " + t + "°");
  }
  const onTurn = () => { sendOrient(); setTimeout(sendOrient, 300); }; // Some iOS versions report late.
  if (screen.orientation) screen.orientation.addEventListener("change", onTurn);
  window.addEventListener("orientationchange", onTurn);
  window.addEventListener("resize", onTurn);

  function configure(c, bitrate, g) {
    c.w = g.w;
    c.h = g.h;
    c.rotation = g.rotation;
    c.target = P.bitrateFor(c.w, c.h, c.fps);
    c.bitrate = bitrate || c.target;
    c.enc.configure({
      codec: P.codecFor(c.w, c.h, c.fps), width: c.w, height: c.h, framerate: c.fps, bitrate: c.bitrate,
      latencyMode: "realtime", hardwareAcceleration: "prefer-hardware", avc: { format: "annexb" },
    });
    c.needConfig = true;
    c.forceKey = true;
  }

  function schedule(c) {
    if (c.stop) return;
    if (ui.video.requestVideoFrameCallback) ui.video.requestVideoFrameCallback(() => pump(c));
    else requestAnimationFrame(() => pump(c));
  }

  async function pump(c) {
    if (c.stop || c.reconfiguring) return;
    const now = performance.now();
    const frame = new VideoFrame(ui.video, { timestamp: nowUs() });
    const g = geometry(frame);
    // The picture changed (the phone was turned): finish the old frames, configure again, and send a new
    // CONFIG (size and rotation) before the next key frame.
    if (g.w !== c.w || g.h !== c.h || g.rotation !== c.rotation) {
      frame.close();
      c.reconfiguring = true;
      try { await c.enc.flush(); } catch (e) { /* ignore */ }
      if (c.stop) return;
      try { configure(c, c.bitrate, g); } catch (e) { sendLog("encoder reconfigure failed: " + (e.message || e)); }
      sendLog("picture is now " + g.text);
      sendOrient();
      c.reconfiguring = false;
      sendCamera();
      schedule(c);
      return;
    }
    // Back-pressure: never queue seconds of video. If the link is slower than the encoder, skip frames until
    // the next key frame and lower the bitrate; raise it again once the link keeps up.
    const budget = Math.max(256 * 1024, c.bitrate / 8 / 4); // About a quarter of a second of video.
    if (c.enc.encodeQueueSize > 2) { // The encoder is behind (starting up, say): skip this frame, nothing more.
      frame.close();
      schedule(c);
      return;
    }
    if (ws && ws.bufferedAmount > budget) {
      c.skipped++;
      c.forceKey = true;
      c.lastSkipAt = now;
      if (now - c.lastDropAt > 2000 && c.bitrate > 1e6) {
        c.lastDropAt = now;
        const lower = Math.max(1e6, Math.round(c.bitrate * 0.7));
        sendLog("link slower than the camera: bitrate " + (c.bitrate / 1e6).toFixed(1) + " -> " + (lower / 1e6).toFixed(1) + " Mbps");
        try { configure(c, lower, g); } catch (e) { /* keep going */ }
      }
      frame.close();
      schedule(c);
      return;
    }
    if (c.bitrate < c.target && now - c.lastSkipAt > 5000 && now - c.lastRaiseAt > 5000 &&
        (!ws || ws.bufferedAmount < budget / 4)) {
      c.lastRaiseAt = now;
      const higher = Math.min(c.target, Math.round(c.bitrate * 1.15));
      sendLog("link keeps up: bitrate " + (c.bitrate / 1e6).toFixed(1) + " -> " + (higher / 1e6).toFixed(1) + " Mbps");
      try { configure(c, higher, g); } catch (e) { /* keep going */ }
    }
    c.enc.encode(frame, { keyFrame: c.forceKey || c.frames % (c.fps * 2) === 0 }); // A key frame every 2 s.
    frame.close();
    c.forceKey = false;
    c.frames++;
    measureFps(c, now);
    schedule(c);
  }

  // What the camera really delivers. If it can't keep the asked rate, that rate goes from the mask, so the
  // PC stops offering it (e.g. some cameras say 60 fps but give 30 at 4K).
  function measureFps(c, now) {
    c.fpsWindowFrames++;
    const span = now - c.fpsWindowStart;
    if (span < 3000) return;
    const measured = Math.round((c.fpsWindowFrames * 1000) / span);
    c.fpsWindowStart = now;
    c.fpsWindowFrames = 0;
    const changed = Math.abs(measured - c.measuredFps) >= 3;
    c.measuredFps = measured;
    if (!c.fpsChecked) {
      c.fpsChecked = true;
      const cam = caps[facing];
      if (cam && c.fps > 30 && measured < c.fps * 0.75) {
        cam.masks[c.q] &= ~P.FPS_BIT[c.fps];
        sendLog("asked for " + c.fps + " fps, the camera gives " + measured + ": not offering " + c.fps + " fps here");
      }
    }
    if (changed) sendCamera();
  }

  function onChunk(c, chunk) {
    if (c.stop) return;
    const data = new Uint8Array(chunk.byteLength);
    chunk.copyTo(data);
    const key = chunk.type === "key";
    if (c.needConfig) {
      if (!key) return; // CONFIG goes out right before a key frame.
      const spsPps = P.extractSpsPps(data);
      send(P.config(c.w, c.h, c.rotation, facing, spsPps)); // How far the PC turns the pixels (see geometry()).
      c.needConfig = false;
      if (phoneState !== P.STATE.STREAMING) {
        setState(P.STATE.STREAMING);
        sendCamera();
      }
    }
    send(P.frame(key, chunk.timestamp, data));
  }

  async function stopCapture() {
    const c = capture;
    if (!c) return;
    c.stop = true;
    capture = null;
    try { await c.enc.flush(); } catch (e) { /* ignore */ }
    try { c.enc.close(); } catch (e) { /* ignore */ }
    c.stream.getTracks().forEach((t) => t.stop());
    ui.video.srcObject = null;
    render();
  }

  async function restartCapture() {
    const was = !!capture;
    await stopCapture();
    if (was && wantStream) await startCapture();
    sendCamera();
  }

  function requestConfigAndKey() {
    if (capture) { capture.needConfig = true; capture.forceKey = true; }
  }

  function cameraFailed(e) {
    const name = (e && e.name) || "Error";
    sendLog("camera error: " + name + " " + ((e && e.message) || ""));
    cameraBlocked = name === "NotAllowedError" || name === "SecurityError";
    setState(P.STATE.ERROR);
  }
  let cameraBlocked = false;

  async function applyLive(constraint) {
    if (!capture) return false;
    try {
      await capture.track.applyConstraints({ advanced: [constraint] });
      return true;
    } catch (e) {
      sendLog("the camera refused " + JSON.stringify(constraint));
      return false;
    }
  }

  // --- Commands from the PC -----------------------------------------------------------------------
  async function handle(cmd, arg) {
    switch (cmd) {
      case P.CMD.HELLO:
        helloDone = true;
        if (userPaused || hiddenPaused) phoneState = P.STATE.PAUSED;
        send(P.hello());
        sendState();
        lastTurn = -1;
        sendOrient();
        sendCamera();
        sendLog("page: " + navigator.userAgent);
        earlyLogs.splice(0).forEach((t) => send(P.log(t)));
        break;
      case P.CMD.START:
        wantStream = true;
        if (userPaused || hiddenPaused) { phoneState = P.STATE.PAUSED; sendState(); break; }
        if (capture) requestConfigAndKey();
        else await startCapture();
        break;
      case P.CMD.STOP:
        wantStream = false;
        await stopCapture();
        setState(userPaused || hiddenPaused ? P.STATE.PAUSED : P.STATE.IDLE);
        break;
      case P.CMD.KEYFRAME:
        requestConfigAndKey();
        break;
      case P.CMD.SET_FACING:
        await setFacing(arg ? P.FACING.FRONT : P.FACING.BACK);
        break;
      case P.CMD.PAUSE:
        await setUserPaused(true);
        break;
      case P.CMD.RESUME:
        await setUserPaused(false);
        break;
      case P.CMD.SET_QUALITY:
        quality = Math.min(2, arg);
        await restartCapture();
        break;
      case P.CMD.SET_FPS:
        fps = arg >= 120 ? 120 : arg >= 60 ? 60 : 30;
        await restartCapture();
        break;
      case P.CMD.SET_ZOOM: {
        const c = caps[facing];
        if (!c || !c.zoom) break;
        const z = Math.min(c.zoom.max, Math.max(c.zoom.min, arg / 10));
        if (await applyLive({ zoom: z })) zoom = z;
        sendCamera();
        break;
      }
      case P.CMD.SET_TORCH: {
        const c = caps[facing];
        if (!c || !c.torch) break;
        if (await applyLive({ torch: !!arg })) torch = !!arg;
        sendCamera();
        break;
      }
      default:
        break; // Brightness and focus: Safari can't set them, so the PC greys them out.
    }
    render();
  }

  async function setFacing(f) {
    if (f === facing) { sendState(); return; }
    facing = f;
    store.set("mycam.facing", String(f));
    facingInputs.forEach((i) => { i.checked = Number(i.value) === f; });
    await restartCapture();
    sendState();
    render();
  }

  async function setUserPaused(p) {
    userPaused = p;
    store.set("mycam.paused", p ? "1" : "0");
    if (p) {
      await stopCapture();
      setState(P.STATE.PAUSED);
    } else if (hiddenPaused) {
      setState(P.STATE.PAUSED);
    } else if (wantStream) {
      await startCapture();
      if (!capture) render();
    } else {
      setState(P.STATE.IDLE);
    }
  }

  // --- Connection ---------------------------------------------------------------------------------
  async function pair(code) {
    const body = new URLSearchParams({ t: code, name: deviceName() });
    const res = await fetch("/pair", { method: "POST", body, cache: "no-store" });
    if (!res.ok) return false;
    const j = await res.json();
    if (!j.device) return false;
    deviceToken = j.device;
    store.set("mycam.device", deviceToken);
    return true;
  }

  function deviceName() {
    const ua = navigator.userAgent;
    if (/iPad/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1)) return "iPad";
    if (/iPhone/.test(ua)) return "iPhone";
    if (/Android/.test(ua)) return "Android phone";
    return "Web browser";
  }

  function scheduleReconnect(delay) {
    clearTimeout(reconnectTimer);
    if (stopReason || document.hidden) return; // Comes back when the page is visible again.
    reconnectTimer = setTimeout(connect, delay);
  }

  async function connect() {
    clearTimeout(reconnectTimer);
    if (ws || !deviceToken || stopReason || document.hidden) return;
    render();
    let check;
    try {
      const res = await fetch("/check", { method: "POST", body: new URLSearchParams({ d: deviceToken }), cache: "no-store" });
      if (res.status === 403) { forgotten(); return; }
      check = await res.json();
    } catch (e) {
      scheduleReconnect(nextDelay()); // The PC is off, asleep, or on another network.
      render();
      return;
    }
    if (check.busy) {
      busy = true;
      render();
      scheduleReconnect(3000);
      return;
    }
    busy = false;
    const socket = new WebSocket("wss://" + location.host + "/ws?d=" + encodeURIComponent(deviceToken));
    socket.binaryType = "arraybuffer";
    ws = socket;
    socket.onopen = () => {
      connected = true;
      reconnectDelay = 1000;
      render();
    };
    socket.onmessage = (e) => {
      const c = P.parseCommand(e.data);
      if (!c) return;
      queue = queue.then(() => handle(c.cmd, c.arg)).catch((err) => sendLog("command failed: " + (err.message || err)));
    };
    socket.onclose = (e) => {
      if (ws !== socket) return;
      ws = null;
      connected = false;
      helloDone = false;
      wantStream = false;
      stopCapture();
      if (phoneState === P.STATE.STREAMING) phoneState = P.STATE.IDLE;
      if (e.code === 4003) { forgotten(); return; }
      if (e.code === 4002) { stopReason = "replaced"; render(); return; }
      if (e.code === 4001) { busy = true; scheduleReconnect(3000); render(); return; }
      scheduleReconnect(nextDelay());
      render();
    };
  }
  let busy = false;

  function nextDelay() {
    const d = reconnectDelay;
    reconnectDelay = Math.min(10000, reconnectDelay * 2);
    return d;
  }

  function forgotten() {
    deviceToken = null;
    store.del("mycam.device");
    stopReason = "forgotten";
    if (ws) { const s = ws; ws = null; s.close(); }
    stopCapture();
    render();
  }

  // --- Page lifecycle -----------------------------------------------------------------------------
  async function keepAwake() {
    try {
      if ("wakeLock" in navigator && !wake && document.visibilityState === "visible") {
        wake = await navigator.wakeLock.request("screen");
        wake.addEventListener("release", () => { wake = null; });
      }
    } catch (e) { /* not allowed now: tried again when the page is visible */ }
  }

  document.addEventListener("visibilitychange", async () => {
    if (document.hidden) {
      // iOS stops the camera now. Tell the PC while the socket still works: it shows "Camera paused".
      hiddenPaused = true;
      if (capture || wantStream) {
        sendLog("page hidden: iOS stops the camera");
        await stopCapture();
        setState(P.STATE.PAUSED);
      }
      return;
    }
    hiddenPaused = false;
    if (!userPaused && phoneState === P.STATE.PAUSED) phoneState = P.STATE.IDLE; // The PC starts it again if wanted.
    if (!started) return;
    keepAwake();
    if (!ws) {
      reconnectDelay = 1000;
      connect();
      return;
    }
    // Still connected: back to normal. The PC sends START again if an app still wants the camera.
    if (!userPaused) {
      if (wantStream) await startCapture();
      else setState(P.STATE.IDLE);
    }
    render();
  });

  window.addEventListener("pageshow", (e) => { if (e.persisted && started) connect(); });

  async function onStart() {
    askForMotion(); // First, while the tap still counts as the user's own.
    ui.start.disabled = true;
    started = true;
    await keepAwake();
    render();
    try {
      await ensureCaps(facing); // Asks for the camera now, while the user is looking.
    } catch (e) {
      started = false;
      cameraBlocked = e && (e.name === "NotAllowedError" || e.name === "SecurityError");
      ui.start.disabled = false;
      render();
      return;
    }
    cameraBlocked = false;
    ui.start.disabled = false;
    connect();
  }

  // --- Drawing ------------------------------------------------------------------------------------
  function show(el, on) { el.hidden = !on; }

  function view() {
    if (!window.isSecureContext || typeof VideoEncoder === "undefined" || !navigator.mediaDevices) {
      return { head: "This browser can’t do it", text: "MyCam needs Safari on iOS 17 or later (or a recent Chrome or Edge). Update the iPhone and open this page again." };
    }
    if (stopReason === "forgotten" || (!deviceToken && !pairing)) {
      return { head: "Scan the code on your PC", text: "On the PC, open MyCam and choose “Connect an iPhone”. Then point the iPhone’s Camera app at the QR code." };
    }
    if (stopReason === "replaced") {
      return { head: "Open in another tab", text: "MyCam is running in another Safari tab. Use that one, or reload this page." };
    }
    if (pairing) return { head: "Pairing…", text: "Saying hello to your PC.", working: true };
    if (pairError) {
      return { head: "That code didn’t work", text: "Each code works once, for 5 minutes. Show a new code on the PC and scan it again." };
    }
    if (cameraBlocked) {
      return { head: "The camera is blocked", text: "Allow the camera for this page: tap “aA” in the address bar → Website Settings → Camera → Allow. Then tap Start.", start: true };
    }
    if (!started) {
      return { head: "Ready to connect", text: "Tap Start. The camera turns on only while an app on the PC uses MyCam.", start: true };
    }
    if (!connected) {
      if (busy) return { head: "Your PC is busy", text: "Another phone is connected to MyCam. This page connects as soon as it is free.", working: true };
      return { head: "Connecting to your PC…", text: "Keep this page open. The PC and the iPhone must be on the same network.", working: true };
    }
    if (userPaused || hiddenPaused || phoneState === P.STATE.PAUSED) {
      return { head: "Paused", text: "The camera is off. Apps see a “Camera paused” picture.", paused: true, pause: true };
    }
    if (phoneState === P.STATE.ERROR) {
      return { head: "Camera problem", text: "The camera couldn’t start. Close other apps that use the camera, then try again from the PC.", pause: true };
    }
    if (phoneState === P.STATE.STREAMING && capture) {
      const f = capture.measuredFps || capture.fps;
      return { head: "Streaming", text: "An app on the PC is using the camera.", facts: capture.w + " × " + capture.h + " · " + f + " fps", pause: true };
    }
    return { head: "Waiting for your PC", text: "Connected. The camera turns on when an app on the PC uses MyCam.", pause: true };
  }
  let pairing = false;
  let pairError = false;

  function render() {
    const v = view();
    ui.headline.textContent = v.head;
    ui.sentence.textContent = v.text;
    ui.facts.textContent = v.facts || "";
    show(ui.facts, !!v.facts);
    show(ui.progress, !!v.working);
    show(ui.start, !!v.start);
    show(ui.pause, !!v.pause);
    ui.hero.classList.toggle("paused", !!v.paused);
    ui.pauseTitle.textContent = userPaused ? "Resume the camera" : "Pause the camera";
    ui.pauseHint.textContent = userPaused ? "The camera turns back on if an app on the PC still uses it."
                                          : "Apps see “Camera paused” until you resume, here or on the PC.";
    show(ui.cameraGroup, started && !!deviceToken && !stopReason);
    show(ui.tipsGroup, started && !!deviceToken && !stopReason);
    facingInputs.forEach((i) => { i.checked = Number(i.value) === facing; });
  }

  // --- Wiring -------------------------------------------------------------------------------------
  ui.start.addEventListener("click", onStart);
  ui.pause.addEventListener("click", () => {
    queue = queue.then(() => setUserPaused(!userPaused)).then(render);
  });
  facingInputs.forEach((i) => i.addEventListener("change", () => {
    if (!i.checked) return;
    const f = Number(i.value) === 1 ? P.FACING.FRONT : P.FACING.BACK;
    queue = queue.then(() => setFacing(f));
  }));
  ui.dim.addEventListener("click", () => show(ui.dimScreen, true));
  ui.dimScreen.addEventListener("click", () => show(ui.dimScreen, false));
  ui.video.muted = true;
  ui.video.playsInline = true;

  // A QR code's one-time code: trade it for this device's own token, and take it out of the address (and
  // so out of Safari's history) straight away.
  (async function init() {
    const code = new URLSearchParams(location.search).get("t");
    if (code) {
      history.replaceState(null, "", "/");
      pairing = true;
      render();
      try {
        // Already paired with this PC: keep that, rather than being listed twice.
        let known = false;
        if (deviceToken) {
          const r = await fetch("/check", { method: "POST", body: new URLSearchParams({ d: deviceToken }), cache: "no-store" });
          known = r.ok;
        }
        pairError = known ? false : !(await pair(code));
      } catch (e) {
        pairError = true;
      }
      pairing = false;
      if (!pairError) stopReason = "";
      // A device that was paired before can carry on with its old token.
      if (pairError && deviceToken) pairError = false;
    }
    render();
  })();
})();
