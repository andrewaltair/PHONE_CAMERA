'use strict';

// Browser-behaviour tests for the three pages.  They intentionally use small
// WebRTC/WebSocket fakes instead of reproducing implementation details: the
// assertions cover privacy, signaling races, safe DOM construction, and the
// observable telemetry that a real browser exposes.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const ROOT = path.resolve(__dirname, '..');

class FakeElement {
  constructor(id, attrs = {}) {
    this.id = id;
    this.style = {};
    this.disabled = false;
    this.textContent = '';
    this.value = '';
    this.srcObject = null;
    this.src = '';
    this.dataset = { ...attrs };
    this.children = [];
    this.firstChild = null;
    this.classList = { toggle() {}, add() {}, remove() {} };
    this._innerHTML = '';
  }
  appendChild(child) {
    this.children.push(child);
    this.firstChild = this.children[0] || null;
    return child;
  }
  removeChild(child) {
    const index = this.children.indexOf(child);
    if (index >= 0) this.children.splice(index, 1);
    this.firstChild = this.children[0] || null;
    return child;
  }
  replaceChildren(...children) {
    this.children = children;
    this.firstChild = this.children[0] || null;
  }
  removeAttribute(name) {
    if (name === 'src') this.src = '';
  }
  load() {}
  pause() {}
  play() { return Promise.resolve(); }
  getVideoPlaybackQuality() { return null; }
  set innerHTML(value) { this._innerHTML = String(value); }
  get innerHTML() { return this._innerHTML; }
}

class FakeTrack {
  constructor(kind, settings = {}) {
    this.kind = kind;
    this.readyState = 'live';
    this.settings = settings;
    this.stopped = false;
    this.contentHint = '';
  }
  getSettings() { return { ...this.settings }; }
  getCapabilities() { return { zoom: { min: 1, max: 4, step: 0.1 }, torch: true }; }
  applyConstraints() { return Promise.resolve(); }
  stop() { this.stopped = true; this.readyState = 'ended'; }
}

class FakeStream {
  constructor(index) {
    this.video = new FakeTrack('video', {
      width: 1280, height: 720, frameRate: 30, facingMode: 'environment',
      deviceId: 'camera-' + index,
    });
    this.audio = new FakeTrack('audio', { deviceId: 'mic-' + index });
  }
  getTracks() { return [this.video, this.audio]; }
  getVideoTracks() { return [this.video]; }
  getAudioTracks() { return [this.audio]; }
}

class FakeSender {
  constructor(track) { this.track = track; this.parameters = { encodings: [{}] }; }
  getParameters() { return this.parameters; }
  setParameters(parameters) {
    this.parameters = parameters;
    if (!FakeSender.deferParameters) return Promise.resolve();
    return new Promise((resolve) => FakeSender.pendingParameters.push(resolve));
  }
  replaceTrack(track) {
    if (!FakeSender.deferReplace) { this.track = track; return Promise.resolve(); }
    return new Promise((resolve) => FakeSender.pendingReplacements.push(() => { this.track = track; resolve(); }));
  }
}
FakeSender.deferParameters = false;
FakeSender.pendingParameters = [];
FakeSender.deferReplace = false;
FakeSender.pendingReplacements = [];

class FakePeerConnection {
  constructor() {
    this.connectionState = 'new';
    this.iceConnectionState = 'new';
    this.localDescription = null;
    this.remoteDescription = null;
    this.senders = [];
    this.addedCandidates = [];
    FakePeerConnection.instances.push(this);
  }
  addTrack(track) { const sender = new FakeSender(track); this.senders.push(sender); return sender; }
  getSenders() { return this.senders; }
  getReceivers() { return []; }
  getTransceivers() { return []; }
  createOffer() { return Promise.resolve({ type: 'offer', sdp: 'offer' }); }
  createAnswer() { return Promise.resolve({ type: 'answer', sdp: 'answer' }); }
  setLocalDescription(description) { this.localDescription = description; return Promise.resolve(); }
  setRemoteDescription(description) { this.remoteDescription = description; return Promise.resolve(); }
  addIceCandidate(candidate) { this.addedCandidates.push(candidate); return Promise.resolve(); }
  getStats() {
    return Promise.resolve(new Map([
      ['inbound', {
        type: 'inbound-rtp', kind: 'video', framesPerSecond: 29,
        frameWidth: 1280, frameHeight: 720, packetsLost: 0,
        jitterBufferDelay: 0.5, jitterBufferEmittedCount: 10,
      }],
      ['pair', { type: 'candidate-pair', nominated: true, currentRoundTripTime: 0.02 }],
    ]));
  }
  close() { this.closed = true; this.connectionState = 'closed'; }
}
FakePeerConnection.instances = [];

class FakeWebSocket {
  constructor() {
    this.readyState = FakeWebSocket.OPEN;
    this.sent = [];
    FakeWebSocket.instances.push(this);
  }
  send(value) { this.sent.push(JSON.parse(value)); }
  close() { this.readyState = FakeWebSocket.CLOSED; if (this.onclose) this.onclose(); }
}
FakeWebSocket.OPEN = 1;
FakeWebSocket.CLOSED = 3;
FakeWebSocket.instances = [];

function createDocument(ids, dataButtons = []) {
  const elements = new Map(ids.map((id) => [id, new FakeElement(id)]));
  const buttons = dataButtons.map((ms) => new FakeElement('jb-' + ms, { ms: String(ms) }));
  return {
    elements,
    getElementById(id) { return elements.get(id); },
    createElement(tag) { return new FakeElement(tag); },
    querySelectorAll(selector) { return selector === '[data-ms]' ? buttons : []; },
  };
}

function loadPage(name, { autoRun = true } = {}) {
  FakeSender.deferParameters = false;
  FakeSender.pendingParameters = [];
  FakeSender.deferReplace = false;
  FakeSender.pendingReplacements = [];
  const html = fs.readFileSync(path.join(ROOT, 'public', name), 'utf8');
  const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
  const ids = name === 'phone.html'
    ? ['preview', 'status', 'start', 'stop', 'flip']
    : name === 'viewer.html'
      ? ['remote', 'hint', 'cam', 'quality', 'bitrate', 'lensWrap', 'lens', 'zoomWrap', 'zoom', 'torch', 'hud', 'status', 'actual', 'retry']
      : ['v'];
  const document = createDocument(ids, name === 'viewer.html' ? [0, 30, 80] : []);
  let timerId = 0;
  const timers = [];
  const streams = [];
  let deferredCapture = null;
  let captureError = null;
  const mediaDevices = {
    getUserMedia: async () => {
      if (captureError) {
        const error = captureError;
        captureError = null;
        throw error;
      }
      if (deferredCapture) return new Promise((resolve) => deferredCapture.push(resolve));
      const stream = new FakeStream(streams.length + 1); streams.push(stream); return stream;
    },
    enumerateDevices: async () => [
      { kind: 'videoinput', deviceId: 'camera-1', label: 'Back camera' },
      { kind: 'videoinput', deviceId: 'camera-2', label: 'Front camera' },
    ],
    addEventListener() {},
  };
  const context = {
    console: { log() {}, error() {} },
    document,
    window: {},
    location: { protocol: 'https:', host: 'localhost:8443' },
    navigator: { mediaDevices },
    WebSocket: FakeWebSocket,
    RTCPeerConnection: FakePeerConnection,
    RTCRtpSender: { getCapabilities: () => ({ codecs: [] }) },
    setInterval() { return 0; },
    clearInterval() {},
    setTimeout(fn) { timers.push(fn); return ++timerId; },
    clearTimeout() {},
    Promise,
    Map,
    Number,
    Math,
    isNaN,
  };
  context.window = context;
  vm.createContext(context);
  if (autoRun) vm.runInContext(script, context, { filename: name });
  return {
    context,
    document,
    elements: document.elements,
    sockets: FakeWebSocket.instances,
    streams,
    timers,
    deferNextCapture() { deferredCapture = []; return deferredCapture; },
    resolveDeferredCapture() {
      const waiters = deferredCapture || [];
      deferredCapture = null;
      const stream = new FakeStream(streams.length + 1);
      streams.push(stream);
      for (const resolve of waiters) resolve(stream);
      return stream;
    },
    rejectNextCapture(error = new Error('camera busy')) { captureError = error; },
    deferNextParameters() {
      FakeSender.deferParameters = true;
      FakeSender.pendingParameters = [];
      return FakeSender.pendingParameters;
    },
    resolveDeferredParameters() {
      const waiters = FakeSender.pendingParameters;
      FakeSender.deferParameters = false;
      FakeSender.pendingParameters = [];
      for (const resolve of waiters) resolve();
    },
    deferNextReplace() {
      FakeSender.deferReplace = true;
      FakeSender.pendingReplacements = [];
      return FakeSender.pendingReplacements;
    },
    resolveDeferredReplace() {
      const waiters = FakeSender.pendingReplacements;
      FakeSender.deferReplace = false;
      FakeSender.pendingReplacements = [];
      for (const resolve of waiters) resolve();
    },
    runTimers() { while (timers.length) timers.shift()(); },
  };
}

async function test(name, fn) {
  try {
    await fn();
    process.stdout.write('ok - ' + name + '\n');
  } catch (error) {
    process.stderr.write('not ok - ' + name + '\n' + error.stack + '\n');
    process.exitCode = 1;
  }
}

async function flushMicrotasks(count = 8) {
  while (count-- > 0) await Promise.resolve();
}

(async () => {
  await test('all pages default to LAN-only ICE', () => {
    for (const page of ['phone.html', 'viewer.html', 'source.html']) {
      const source = fs.readFileSync(path.join(ROOT, 'public', page), 'utf8');
      assert.match(source, /iceServers\s*:\s*\[\s*\]/, page + ' must not configure public ICE servers');
      assert.doesNotMatch(source, /stun:/i, page + ' must not contain a hidden STUN fallback');
    }
  });

  await test('phone stop releases camera and microphone and permits an explicit restart', async () => {
    const app = loadPage('phone.html');
    await app.elements.get('start').onclick();
    const first = app.streams[0];
    assert.ok(first, 'first start should acquire a stream');
    assert.equal(app.elements.get('stop').disabled, false);
    app.elements.get('stop').onclick({ type: 'click' });
    assert.ok(first.getTracks().every((track) => track.stopped), 'stop must stop every media track');
    assert.match(app.elements.get('status').textContent, /camera and microphone released/i, 'stop must show a useful status');
    assert.equal(app.elements.get('preview').srcObject, null, 'stop must clear the preview');
    const socketCount = app.sockets.length;
    app.runTimers();
    assert.equal(app.sockets.length, socketCount, 'a stopped phone must not reconnect invisibly');
    await app.elements.get('start').onclick();
    assert.equal(app.streams.length, 2, 'restart must require and perform a fresh capture');
  });

  await test('phone reports getSettings and bounds signaling recovery', async () => {
    const app = loadPage('phone.html');
    await app.elements.get('start').onclick();
    const socket = app.sockets.at(-1);
    socket.onopen();
    await app.context.reportCaps();
    const capsMessage = socket.sent.find((message) => message.type === 'caps');
    assert.equal(capsMessage.caps.settings.width, 1280);
    assert.equal(capsMessage.caps.settings.height, 720);
    assert.equal(capsMessage.caps.settings.frameRate, 30);
    const before = app.sockets.length;
    socket.onclose();
    app.runTimers();
    assert.equal(app.sockets.length, before + 1, 'an unexpected signaling drop gets one bounded retry');
    app.elements.get('stop').onclick();
  });

  await test('phone closes a stale camera operation that resolves after Stop', async () => {
    const app = loadPage('phone.html');
    await app.elements.get('start').onclick();
    const pending = app.deferNextCapture();
    const flipPromise = app.elements.get('flip').onclick();
    await flushMicrotasks();
    assert.equal(pending.length, 1, 'flip should be waiting on the deferred capture');
    app.elements.get('stop').onclick();
    const stale = app.resolveDeferredCapture();
    await flipPromise;
    assert.ok(stale.getTracks().every((track) => track.stopped), 'all tracks from a stale capture must be stopped');
    assert.equal(app.elements.get('preview').srcObject, null, 'stale capture must not replace the cleared preview');
  });

  await test('phone closes a stale remote device switch that resolves after Stop', async () => {
    const app = loadPage('phone.html');
    await app.elements.get('start').onclick();
    const pending = app.deferNextCapture();
    const devicePromise = app.context.applyControl({ action: 'device', deviceId: 'camera-2' });
    await flushMicrotasks();
    assert.equal(pending.length, 1, 'remote device switch should wait on the deferred capture');
    app.elements.get('stop').onclick();
    const stale = app.resolveDeferredCapture();
    await devicePromise;
    assert.ok(stale.getTracks().every((track) => track.stopped), 'all tracks from a stale device switch must be stopped');
    assert.equal(app.elements.get('preview').srcObject, null, 'stale device switch must not restore the preview');
  });

  await test('phone Stop synchronously releases a stream blocked in replaceTrack', async () => {
    const app = loadPage('phone.html');
    await app.elements.get('start').onclick();
    const socket = app.sockets.at(-1);
    socket.onopen();
    await socket.onmessage({ data: JSON.stringify({ type: 'peer-ready' }) });
    const pending = app.deferNextReplace();
    const flipPromise = app.elements.get('flip').onclick();
    await flushMicrotasks();
    assert.equal(pending.length, 1, 'replaceTrack should be held open for the race test');
    const next = app.streams[1];
    app.elements.get('stop').onclick();
    try {
      assert.ok(next.getTracks().every((track) => track.stopped), 'Stop must release the in-flight replacement immediately');
    } finally {
      app.resolveDeferredReplace();
      await flipPromise;
    }
  });

  await test('phone Flip does not cancel the pending offer after tuneSender', async () => {
    const app = loadPage('phone.html');
    await app.elements.get('start').onclick();
    const socket = app.sockets.at(-1);
    socket.onopen();
    const pending = app.deferNextParameters();
    const offerPromise = socket.onmessage({ data: JSON.stringify({ type: 'peer-ready' }) });
    await flushMicrotasks();
    assert.equal(pending.length, 1, 'tuneSender should be held open for the race test');
    const flipPromise = app.elements.get('flip').onclick();
    await flipPromise;
    try {
      app.resolveDeferredParameters();
      await offerPromise;
      assert.equal(socket.sent.filter((message) => message.type === 'offer').length, 1, 'the offer must still be sent after Flip');
    } finally {
      app.resolveDeferredParameters();
    }
  });

  await test('ended camera track stops capture and restores an actionable Start button', async () => {
    const app = loadPage('phone.html');
    await app.elements.get('start').onclick();
    const stream = app.streams[0];
    stream.video.onended();
    assert.ok(stream.getTracks().every((track) => track.stopped), 'camera end must release both camera and microphone');
    assert.equal(app.elements.get('start').disabled, false, 'Start must be actionable after camera end');
    assert.equal(app.elements.get('stop').disabled, true);
    assert.match(app.elements.get('status').textContent, /ended|released|Start streaming/i);
  });

  await test('Flip resynchronizes the latest peer after the old PC swap resolves', async () => {
    const app = loadPage('phone.html');
    await app.elements.get('start').onclick();
    const socket = app.sockets.at(-1);
    socket.onopen();
    await socket.onmessage({ data: JSON.stringify({ type: 'peer-ready' }) });
    const pending = app.deferNextReplace();
    const flipPromise = app.elements.get('flip').onclick();
    await flushMicrotasks();
    assert.equal(pending.length, 1, 'old peer swap should be held open');
    const next = app.streams[1];
    await socket.onmessage({ data: JSON.stringify({ type: 'peer-ready' }) });
    const latestPeer = FakePeerConnection.instances.at(-1);
    app.resolveDeferredReplace();
    await flipPromise;
    assert.equal(latestPeer.getSenders()[0].track, next.video, 'latest peer must use the successfully selected stream');
  });

  await test('double Flip serializes camera changes and preserves live sender after second rejection', async () => {
    const app = loadPage('phone.html');
    await app.elements.get('start').onclick();
    const socket = app.sockets.at(-1);
    socket.onopen();
    await socket.onmessage({ data: JSON.stringify({ type: 'peer-ready' }) });
    const pending = app.deferNextReplace();
    const firstFlip = app.elements.get('flip').onclick();
    await flushMicrotasks();
    assert.equal(pending.length, 1, 'first Flip should hold replaceTrack open');
    const firstNext = app.streams[1];
    app.rejectNextCapture();
    const secondFlip = app.elements.get('flip').onclick();
    app.resolveDeferredReplace();
    await firstFlip;
    await secondFlip;
    const latestPeer = FakePeerConnection.instances.at(-1);
    assert.ok(firstNext.getTracks().every((track) => !track.stopped), 'first successful Flip stream must stay live');
    assert.equal(latestPeer.getSenders()[0].track, firstNext.video, 'current peer must retain the live successful stream');
  });

  await test('remote device switches serialize and preserve the live first stream after rejection', async () => {
    const app = loadPage('phone.html');
    await app.elements.get('start').onclick();
    const socket = app.sockets.at(-1);
    socket.onopen();
    await socket.onmessage({ data: JSON.stringify({ type: 'peer-ready' }) });
    const pending = app.deferNextReplace();
    const firstDevice = app.context.applyControl({ action: 'device', deviceId: 'camera-2' });
    await flushMicrotasks();
    assert.equal(pending.length, 1, 'first device switch should hold replaceTrack open');
    const firstNext = app.streams[1];
    app.rejectNextCapture();
    const secondDevice = app.context.applyControl({ action: 'device', deviceId: 'camera-1' });
    app.resolveDeferredReplace();
    await firstDevice;
    await secondDevice;
    const latestPeer = FakePeerConnection.instances.at(-1);
    assert.ok(firstNext.getTracks().every((track) => !track.stopped), 'first successful device stream must stay live');
    assert.equal(latestPeer.getSenders()[0].track, firstNext.video, 'current peer must retain the live device stream');
  });

  await test('replaced phone stops capture and never reconnects automatically', async () => {
    const app = loadPage('phone.html');
    await app.elements.get('start').onclick();
    const stream = app.streams[0];
    const socket = app.sockets.at(-1);
    await socket.onmessage({ data: JSON.stringify({ type: 'replaced' }) });
    const before = app.sockets.length;
    socket.onclose();
    app.runTimers();
    assert.equal(app.sockets.length, before, 'replaced phone must not rejoin and evict the newer phone');
    assert.ok(stream.getTracks().every((track) => track.stopped), 'replacement must release phone capture');
    assert.match(app.elements.get('status').textContent, /replaced|retry|Start streaming/i);
  });

  await test('replaced viewer requires explicit retry instead of reconnecting', async () => {
    const app = loadPage('viewer.html');
    const socket = app.sockets.at(-1);
    await socket.onmessage({ data: JSON.stringify({ type: 'replaced' }) });
    const before = app.sockets.length;
    socket.onclose();
    app.runTimers();
    assert.equal(app.sockets.length, before, 'replaced viewer must stop automatic reconnect');
    assert.match(app.elements.get('status').textContent, /replaced|retry|reload/i);
    app.elements.get('retry').onclick();
    assert.equal(app.sockets.length, before + 1, 'viewer retry must be an explicit new connection');
  });

  await test('phone releases capture and unlocks a fresh start after retry limit', async () => {
    const app = loadPage('phone.html');
    await app.elements.get('start').onclick();
    const stream = app.streams[0];
    for (let attempt = 0; attempt <= 6; attempt += 1) {
      app.sockets.at(-1).onclose();
      app.runTimers();
    }
    assert.equal(app.elements.get('start').disabled, false, 'retry exhaustion must unlock explicit Start');
    assert.equal(app.elements.get('stop').disabled, true);
    assert.ok(stream.getTracks().every((track) => track.stopped), 'retry exhaustion must release capture');
    assert.match(app.elements.get('status').textContent, /retry|released|Start streaming/i);
    await app.elements.get('start').onclick();
    assert.equal(app.streams.length, 2, 'explicit Start must create a fresh capture after retry exhaustion');
    app.elements.get('stop').onclick();
  });

  await test('replaced OBS source stops retrying until the page is reloaded', async () => {
    const app = loadPage('source.html');
    const socket = app.sockets.at(-1);
    await socket.onmessage({ data: JSON.stringify({ type: 'replaced' }) });
    const before = app.sockets.length;
    socket.onclose();
    app.runTimers();
    assert.equal(app.sockets.length, before, 'replaced OBS source must not evict the newer source');
  });

  await test('viewer queues ICE that arrives before the offer', async () => {
    FakePeerConnection.instances.length = 0;
    const app = loadPage('viewer.html');
    const socket = app.sockets[app.sockets.length - 1];
    await socket.onmessage({ data: JSON.stringify({ type: 'ice', candidate: { candidate: 'early' } }) });
    await socket.onmessage({ data: JSON.stringify({ type: 'offer', sdp: { type: 'offer', sdp: 'offer' } }) });
    const peer = FakePeerConnection.instances.at(-1);
    assert.equal(peer.addedCandidates.length, 1, 'early ICE must be applied after remote description');
    assert.equal(peer.addedCandidates[0].candidate, 'early');
  });

  await test('viewer creates device options with textContent, never label HTML', () => {
    const app = loadPage('viewer.html');
    app.context.applyCaps({ zoom: null, torch: false }, [
      { deviceId: 'safe', label: '<img src=x onerror=alert(1)>' },
      { deviceId: 'second', label: 'Front' },
    ]);
    const options = app.elements.get('lens').children;
    assert.equal(options.length, 2);
    assert.equal(options[0].textContent, '<img src=x onerror=alert(1)>');
    assert.equal(options[0].innerHTML, '', 'untrusted labels must not become HTML');
  });

  await test('viewer displays measured inbound resolution and fps', async () => {
    FakePeerConnection.instances.length = 0;
    const app = loadPage('viewer.html');
    const socket = app.sockets[app.sockets.length - 1];
    await socket.onmessage({ data: JSON.stringify({ type: 'offer', sdp: { type: 'offer', sdp: 'offer' } }) });
    await app.context.pollStats();
    assert.match(app.elements.get('hud').textContent, /1280[×x]720/);
    assert.match(app.elements.get('hud').textContent, /29 fps/);
  });

  await test('OBS source clears the last frame when signaling drops', async () => {
    const app = loadPage('source.html');
    const socket = app.sockets[app.sockets.length - 1];
    await socket.onmessage({ data: JSON.stringify({ type: 'offer', sdp: { type: 'offer', sdp: 'offer' } }) });
    const peer = FakePeerConnection.instances.at(-1);
    peer.ontrack({ streams: [{ id: 'phone-stream' }] });
    assert.ok(app.elements.get('v').srcObject, 'source should display the received stream');
    socket.onclose();
    assert.equal(app.elements.get('v').srcObject, null, 'source must not retain a stale frame after a drop');
  });
})();
