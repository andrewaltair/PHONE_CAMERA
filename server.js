'use strict';

// iphone-stream — HTTPS static server + WSS signaling relay.
// Phone (Safari) and PC (browser) each connect over WSS; this process only
// relays SDP offer/answer + ICE between the two. The actual A/V never touches
// this server — it flows peer-to-peer over WebRTC on the LAN.

const fs = require('fs');
const os = require('os');
const path = require('path');
const https = require('https');
const http = require('http');
const { WebSocketServer } = require('ws');

const PORT = process.env.PORT ? Number(process.env.PORT) : 8443;
const ROOT = __dirname;
const PUBLIC = path.join(ROOT, 'public');
const CERT_DIR = path.join(ROOT, 'cert');

// ---- TLS cert (generated once with mkcert — see README Setup) -------------
const certPath = path.join(CERT_DIR, 'cert.pem');
const keyPath = path.join(CERT_DIR, 'key.pem');
if (!fs.existsSync(certPath) || !fs.existsSync(keyPath)) {
  console.error('\n[FATAL] TLS cert/key not found. iOS Safari needs a TRUSTED https cert.');
  console.error('  expected: ' + certPath);
  console.error('            ' + keyPath);
  console.error('  Generate them with mkcert (README -> Setup step 3), then re-run.\n');
  process.exit(1);
}
const tls = { cert: fs.readFileSync(certPath), key: fs.readFileSync(keyPath) };

// ---- static files ---------------------------------------------------------
const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
  '.pem': 'application/x-pem-file',
};

function send(res, code, body, type) {
  res.writeHead(code, { 'Content-Type': type || 'text/plain; charset=utf-8' });
  res.end(body);
}

// shared by the HTTPS server (phone + LAN viewers) and the localhost HTTP mirror (OBS source)
function handleRequest(req, res) {
  let urlPath = decodeURIComponent(req.url.split('?')[0]);
  if (urlPath === '/') urlPath = '/viewer.html';

  // serve the mkcert root CA so the phone can download + trust it
  if (urlPath === '/rootCA.pem') {
    const ca = path.join(CERT_DIR, 'rootCA.pem');
    if (fs.existsSync(ca)) return send(res, 200, fs.readFileSync(ca), MIME['.pem']);
    return send(res, 404, 'rootCA.pem not copied into cert/ yet (see README Setup step 4)');
  }

  const filePath = path.join(PUBLIC, urlPath);
  if (!filePath.startsWith(PUBLIC)) return send(res, 403, 'forbidden'); // path-traversal guard
  fs.readFile(filePath, (err, data) => {
    if (err) return send(res, 404, 'not found: ' + urlPath);
    send(res, 200, data, MIME[path.extname(filePath)] || 'application/octet-stream');
  });
}

const server = https.createServer(tls, handleRequest);

// ---- signaling relay (single 2-slot room, shared across both transports) --
const peers = { phone: null, viewer: null };
const other = (role) => (role === 'phone' ? 'viewer' : 'phone');
function emit(ws, obj) { if (ws && ws.readyState === ws.OPEN) ws.send(JSON.stringify(obj)); }

function attachSignaling(wss) {
  wss.on('connection', (ws) => {
    ws.role = null;

    ws.on('message', (raw) => {
      let msg;
      try { msg = JSON.parse(raw.toString()); } catch { return; }

      if (msg.type === 'join') {
        const role = msg.role === 'phone' ? 'phone' : 'viewer';
        if (peers[role] && peers[role] !== ws) {
          emit(peers[role], { type: 'replaced' });          // a newer tab took this slot
          try { peers[role].close(); } catch {}
        }
        ws.role = role;
        peers[role] = ws;
        console.log('[ws] ' + role + ' joined');
        if (peers.phone && peers.viewer) {                  // both here -> phone starts the offer
          emit(peers.phone, { type: 'peer-ready' });
          console.log('[ws] both peers present -> phone offers');
        }
        return;
      }

      // relay everything else (offer / answer / ice / caps / control) to the other peer
      if (ws.role) emit(peers[other(ws.role)], msg);
    });

    ws.on('close', () => {
      if (ws.role && peers[ws.role] === ws) {
        peers[ws.role] = null;
        console.log('[ws] ' + ws.role + ' left');
        emit(peers[other(ws.role)], { type: 'peer-left' });
      }
    });
  });
}

attachSignaling(new WebSocketServer({ server }));

// localhost-only HTTP mirror — lets OBS Browser Source / local tools load source.html over plain
// ws:// with no cert (receive-only viewer; the phone side stays HTTPS/WSS). Bound to 127.0.0.1 only.
const HTTP_PORT = process.env.HTTP_PORT ? Number(process.env.HTTP_PORT) : 8080;
const httpServer = http.createServer(handleRequest);
attachSignaling(new WebSocketServer({ server: httpServer }));
httpServer.listen(HTTP_PORT, '127.0.0.1');

// ---- LAN IP autodetect + boot ---------------------------------------------
// Apple Personal-Hotspot-over-USB hands the PC a 172.20.10.x IP — a direct WIRED link to the phone
const isUSB = (ip) => ip.startsWith('172.20.10.');

function lanIPs() {
  // virtual adapters (WSL / Hyper-V / VPN) hand out IPs the phone can't reach — drop them
  const skip = /vethernet|wsl|hyper-v|default switch|loopback|virtual|vmware|virtualbox/i;
  const collect = (filterNames) => {
    const out = [];
    const ifs = os.networkInterfaces();
    for (const name of Object.keys(ifs)) {
      if (filterNames && skip.test(name)) continue;
      for (const ni of ifs[name] || []) {
        if (ni.family === 'IPv4' && !ni.internal) out.push(ni.address);
      }
    }
    return out;
  };
  let out = collect(true);
  if (!out.length) out = collect(false); // fallback if name-skip removed everything
  // best path first: USB link, then 192.168.* , then 10.* , then the rest
  const rank = (ip) => (isUSB(ip) ? -1 : ip.startsWith('192.168.') ? 0 : ip.startsWith('10.') ? 1 : 2);
  return out.sort((a, b) => rank(a) - rank(b));
}

server.listen(PORT, '0.0.0.0', () => {
  const ips = lanIPs();
  console.log('\n  iphone-stream up  (HTTPS + WSS, port ' + PORT + ')\n');
  console.log('  PC viewer : https://localhost:' + PORT + '/viewer.html');
  console.log('  OBS source: http://localhost:' + HTTP_PORT + '/source.html   (Browser Source — no cert)');
  if (ips.length) {
    console.log('\n  On the iPhone (same Wi-Fi, OR plugged in via USB), open in Safari:');
    for (const ip of ips)
      console.log('    phone   : https://' + ip + ':' + PORT + '/phone.html' + (isUSB(ip) ? '   <-- USB cable (lowest jitter)' : ''));
    console.log('\n  First time on the phone? Trust the CA first: https://' + ips[0] + ':' + PORT + '/rootCA.pem');
    if (ips.some(isUSB)) console.log('  USB link detected — most stable latency; set the viewer jitter buffer to 0.');
    // QR of the primary phone URL — scan with the iPhone camera to open it without typing
    const phoneUrl = 'https://' + ips[0] + ':' + PORT + '/phone.html';
    try {
      console.log('\n  Scan with the iPhone camera to open ' + phoneUrl + ' :');
      require('qrcode-terminal').generate(phoneUrl, { small: true });
    } catch (e) { /* qrcode-terminal not installed — skip the QR, URLs above still work */ }
  } else {
    console.log('  (no external IPv4 found — is Wi-Fi connected?)');
  }
  console.log('');
});
