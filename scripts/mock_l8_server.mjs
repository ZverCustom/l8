#!/usr/bin/env node
// Local stand-in for the board's HTTP API, used only to validate e1_browser.mjs offline.
// Serves the real src/web/index.html; mimics legacy preview/apply/save/discard/touch,
// status/config/session/events and v1 get_config (effective). Not a firmware model.
import http from 'node:http';
import {readFileSync} from 'node:fs';
// Negative-control flags: --ui FILE serves another page; --save-shift stores a wrong value on save.
const PORT = +(process.argv[2] || 8799);
const opt = k => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : null; };
const html = readFileSync(opt('--ui') || new URL('../src/web/index.html', import.meta.url));
const SAVE_SHIFT = process.argv.includes('--save-shift');
const strips = Array.from({length: 8}, () => ({enabled: true, reverse: false, mode: 0, animation: 0, speed: 60, color: 0xff2400, brightness: 100, count: 60, order: 1, source: 0, middle: false, center: 0, maxAmps: 3, currentCal: 1}));
let saved = {schema: 5, global: {minRpm: 2000, maxRpm: 7000, speed: 60, brightness: 30, autoRetry: true}, strips, inputs: [{}, {}, {}], can: {mode: 2, bitrate: 1}, network: {ssid: 'x', password: '', keepPassword: true}, presets: [{used: false, name: ''}, {used: false, name: ''}, {used: false, name: ''}]};
let current = structuredClone(saved), base = null, owner = null, revision = 1, savedRevision = 1;
const token = 'mock-token', boot = 'mockboot0000000000000000000000000';
const status = () => ({configRevision: revision, savedRevision, bootId: boot, previewActive: !!owner, previewOwner: owner, master: false, vbat: 0, ign: false, totalAmps: 0, powerStatus: 'USB', firmware: 'mock', ip: '127.0.0.1', channels: Array.from({length: 8}, () => ({amps: 0, on: false, reason: 'novbat', pins: [1, 10, 26]})), inputs: [{mv: 0, value: 0}, {mv: 0, value: 0}, {mv: 0, value: 0}], imu: {ok: true}, dataFresh: false, canStatus: 'mock', canBitrate: '500k', network: 'mock', flash: 16777216, psram: 2097152, heap: 200000});
const json = (res, code, body) => { res.writeHead(code, {'Content-Type': 'application/json', 'Cache-Control': 'no-store'}); res.end(JSON.stringify(body)); };
const sse = new Set();
setInterval(() => { for (const r of sse) r.write('event: status\ndata: ' + JSON.stringify(status()) + '\n\n'); }, 500);
http.createServer((req, res) => {
  let body = ''; req.on('data', c => body += c); req.on('end', () => {
    const url = new URL(req.url, 'http://x'); const p = url.pathname;
    if (p === '/') { res.writeHead(200, {'Content-Type': 'text/html'}); return res.end(html); }
    if (p === '/api/session') return json(res, 200, {token});
    if (p === '/api/status') return json(res, 200, status());
    if (p === '/api/config' && req.method === 'GET') return json(res, 200, owner ? base : saved);
    if (p === '/api/events') { res.writeHead(200, {'Content-Type': 'text/event-stream'}); res.write('retry: 3000\n\n'); sse.add(res); req.on('close', () => sse.delete(res)); return; }
    if (req.method !== 'POST') return json(res, 404, {error: 'Not found'});
    if (req.headers['x-l8-token'] !== token) return json(res, 403, {error: 'Session expired'});
    let d; try { d = JSON.parse(body); } catch { return json(res, 400, {error: 'Invalid JSON'}); }
    if (p === '/api/command') return json(res, 200, {v: 1, id: d.id, ok: true, revision, savedRevision, result: {view: d.view || 'baseline', config: d.view === 'effective' && (!owner || owner === d.client) ? current : (owner ? base : saved)}});
    const ok = () => json(res, 200, {ok: true, revision, savedRevision, master: false});
    const conflict = () => json(res, 409, {error: 'Configuration changed or another editor is active; reload'});
    if ((owner && owner !== d.client) || d.revision !== revision) return conflict();
    if (p === '/api/preview' || p === '/api/apply') {
      if (!owner) { base = structuredClone(current); owner = d.client; }
      if (p === '/api/preview') Object.assign(current.strips[d.strip], d.patch); else current = structuredClone(d.config);
      revision++; return ok();
    }
    if (p === '/api/save') { if (!owner) return conflict(); if (SAVE_SHIFT) current.strips[0].brightness += 1; saved = structuredClone(current); owner = null; revision++; savedRevision = revision; return ok(); }
    if (p === '/api/discard') { if (owner) { current = base; owner = null; revision++; if (JSON.stringify(current) === JSON.stringify(saved)) savedRevision = revision; } return ok(); }
    if (p === '/api/touch') return owner ? ok() : conflict();
    json(res, 404, {error: 'Not found'});
  });
}).listen(PORT, '127.0.0.1', () => console.log('mock on', PORT));
