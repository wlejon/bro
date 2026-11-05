#!/usr/bin/env node
// The receiver bro's crash reports are sent to (docs/apps.md, "Crash
// reports"): a folder app whose bro.json names
//   "crashReports": "http://<host>:8787/crash"
// has the next launch after a crash POST the crashed run's log, then its
// minidump, here. Plain Node, no dependencies:
//
//   node scripts/crash-receiver.js [--port 8787] [--dir ~/crash-reports]
//
//   POST /crash?report=<name>&file=log|dmp   stores <dir>/<app>/<name>/<file>, + meta.json
//   GET  /                                   the reports, newest first
//   GET  /r/<app>/<name>/<file>              one file (log as text, dmp, meta.json)
//
// Reports are kept as they arrive; nothing is deleted. Anyone who can reach
// the port can send or read reports, so run it on a private network only.

'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const os = require('os');

const args = process.argv.slice(2);
const opt = (name, dflt) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : dflt; };
const PORT = Number(opt('--port', process.env.CRASH_RECEIVER_PORT || 8787));
const DIR = path.resolve(opt('--dir', process.env.CRASH_RECEIVER_DIR || path.join(os.homedir(), 'crash-reports')));
const LIMITS = { log: 8 << 20, dmp: 80 << 20 };
const NAME = /^[A-Za-z0-9._-]{1,160}$/;

fs.mkdirSync(DIR, { recursive: true });

const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const send = (res, status, type, body) => { res.writeHead(status, { 'Content-Type': type }); res.end(body); };

function receive(req, res, url) {
  const report = url.searchParams.get('report') || '';
  const file = url.searchParams.get('file') || '';
  const app = req.headers['x-bro-app'] || 'unknown';
  if (!NAME.test(report) || !NAME.test(app) || !(file in LIMITS)) return send(res, 400, 'text/plain', 'bad report\n');
  const chunks = [];
  let size = 0;
  req.on('data', (c) => {
    size += c.length;
    if (size > LIMITS[file]) { send(res, 413, 'text/plain', 'too large\n'); req.destroy(); return; }
    chunks.push(c);
  });
  req.on('end', () => {
    if (res.writableEnded) return;
    const dir = path.join(DIR, app, report);
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, file), Buffer.concat(chunks));
    const metaPath = path.join(dir, 'meta.json');
    const meta = fs.existsSync(metaPath) ? JSON.parse(fs.readFileSync(metaPath, 'utf8')) : {};
    Object.assign(meta, {
      report, app,
      appName: req.headers['x-bro-app-name'] || '',
      appVersion: req.headers['x-bro-app-version'] || '',
      engine: req.headers['x-bro-engine'] || '',
      host: req.headers['x-bro-host'] || '',
      kind: req.headers['x-bro-report-kind'] || '',
      from: req.socket.remoteAddress,
      received: meta.received || new Date().toISOString(),
    });
    meta.files = Array.from(new Set([...(meta.files || []), file]));
    fs.writeFileSync(metaPath, JSON.stringify(meta, null, 2) + '\n');
    console.log(`${new Date().toISOString()} ${app} ${report} ${file} ${size} bytes from ${meta.host || meta.from}`);
    send(res, 200, 'text/plain', 'ok\n');
  });
}

function listReports() {
  const out = [];
  for (const app of fs.readdirSync(DIR)) {
    const appDir = path.join(DIR, app);
    if (!fs.statSync(appDir).isDirectory()) continue;
    for (const report of fs.readdirSync(appDir)) {
      try { out.push(JSON.parse(fs.readFileSync(path.join(appDir, report, 'meta.json'), 'utf8'))); } catch { /* partial */ }
    }
  }
  return out.sort((a, b) => (a.received < b.received ? 1 : -1));
}

function index(res) {
  const rows = listReports().map((m) => {
    const base = `/r/${encodeURIComponent(m.app)}/${encodeURIComponent(m.report)}`;
    const files = (m.files || []).map((f) => `<a href="${base}/${f}">${f}</a>`).join(' ');
    return `<tr><td>${esc(m.received)}</td><td>${esc(m.appName || m.app)} ${esc(m.appVersion)}</td><td>${esc(m.kind)}</td>` +
      `<td>${esc(m.host)}</td><td>${files} <a href="${base}/meta.json">meta</a></td><td><code>${esc(m.engine)}</code></td></tr>`;
  }).join('\n');
  send(res, 200, 'text/html; charset=utf-8', `<!doctype html><meta charset="utf-8"><title>Crash reports</title>
<style>body{font:14px system-ui;margin:24px}td,th{padding:4px 10px;text-align:left;border-bottom:1px solid #ddd}code{font-size:12px}</style>
<h1>Crash reports</h1><table><tr><th>Received</th><th>App</th><th>Kind</th><th>Computer</th><th>Files</th><th>Engine</th></tr>
${rows || '<tr><td colspan="6">None yet.</td></tr>'}</table>`);
}

function serveFile(res, parts) {
  const [app, report, file] = parts.map(decodeURIComponent);
  if (!NAME.test(app) || !NAME.test(report) || !['log', 'dmp', 'meta.json'].includes(file)) return send(res, 404, 'text/plain', 'not found\n');
  const p = path.join(DIR, app, report, file);
  if (!fs.existsSync(p)) return send(res, 404, 'text/plain', 'not found\n');
  const type = file === 'dmp' ? 'application/octet-stream' : file === 'meta.json' ? 'application/json' : 'text/plain; charset=utf-8';
  const headers = { 'Content-Type': type };
  if (file === 'dmp') headers['Content-Disposition'] = `attachment; filename="${report}.dmp"`;
  res.writeHead(200, headers);
  fs.createReadStream(p).pipe(res);
}

http.createServer((req, res) => {
  const url = new URL(req.url, 'http://receiver');
  if (req.method === 'POST' && url.pathname === '/crash') return receive(req, res, url);
  if (req.method === 'GET' && url.pathname === '/') return index(res);
  const parts = url.pathname.split('/').filter(Boolean);
  if (req.method === 'GET' && parts[0] === 'r' && parts.length === 4) return serveFile(res, parts.slice(1));
  send(res, 404, 'text/plain', 'not found\n');
}).listen(PORT, '0.0.0.0', () => console.log(`crash receiver on :${PORT}, reports in ${DIR}`));
