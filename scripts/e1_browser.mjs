#!/usr/bin/env node
// E1 browser part: drives the REAL embedded web UI of the board in headless Chrome via
// the DevTools protocol (no extra packages; Node's built-in WebSocket/fetch).
// Usage: node scripts/e1_browser.mjs --base http://192.168.4.1 [--out result.json] [--chrome PATH]
// Changes configuration through the UI (brightness of channel 1, then saved): the caller
// must back up the configuration first and restore it afterwards (e1_ap_run.py does it over USB).
import {spawn} from 'node:child_process';
import {mkdtempSync, writeFileSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';

const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const BASE = arg('--base', 'http://192.168.4.1').replace(/\/$/, '');
const OUT = arg('--out', null);
const CHROME = arg('--chrome', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome');
const PORT = +arg('--port', '9333');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const results = [];
const check = (name, ok, detail = {}) => { results.push({name, ok: !!ok, ...detail}); console.log(ok ? 'PASS' : 'FAIL', name, JSON.stringify(detail)); };

const profile = mkdtempSync(join(tmpdir(), 'l8-e1-chrome-'));
const chrome = spawn(CHROME, ['--headless=new', `--remote-debugging-port=${PORT}`, `--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check', '--disable-background-networking', 'about:blank'], {stdio: 'ignore'});
let ws, nextId = 0; const pending = new Map(), listeners = [];
const send = (method, params = {}) => new Promise((resolve, reject) => { const id = ++nextId; pending.set(id, {resolve, reject}); ws.send(JSON.stringify({id, method, params})); });
const evaluate = async (expression) => { const r = await send('Runtime.evaluate', {expression, awaitPromise: true, returnByValue: true}); if (r.exceptionDetails) throw Error('page: ' + (r.exceptionDetails.exception?.description || r.exceptionDetails.text)); return r.result.value; };
const waitFor = async (expr, ms, step = 200) => { const end = Date.now() + ms; while (Date.now() < end) { try { if (await evaluate(expr)) return true; } catch {} await sleep(step); } return false; };

// request log for change requests
const reqs = new Map();
const changes = () => [...reqs.values()].filter(r => r.method === 'POST' && /\/api\/(preview|apply|save|discard|touch)$/.test(r.url));
const maxConcurrent = list => { const ev = []; for (const r of list) { ev.push([r.start, 1]); ev.push([r.end ?? Infinity, -1]); } ev.sort((a, b) => a[0] - b[0] || a[1] - b[1]); let c = 0, m = 0; for (const [, d] of ev) { c += d; m = Math.max(m, c); } return m; };
const since = t => changes().filter(r => r.start >= t);
const now = () => performance.now();

async function main() {
  let target;
  for (let i = 0; i < 50 && !target; i++) { try { target = await (await fetch(`http://127.0.0.1:${PORT}/json/new?about:blank`, {method: 'PUT'})).json(); } catch { await sleep(200); } }
  if (!target) throw Error('Chrome DevTools not reachable');
  ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  ws.onmessage = m => { const d = JSON.parse(m.data); if (d.id && pending.has(d.id)) { const p = pending.get(d.id); pending.delete(d.id); d.error ? p.reject(Error(d.error.message)) : p.resolve(d.result); } else if (d.method) for (const l of listeners) l(d); };
  listeners.push(({method, params}) => {
    if (method === 'Network.requestWillBeSent') reqs.set(params.requestId, {url: params.request.url, method: params.request.method, start: now(), body: params.request.postData});
    if (method === 'Network.loadingFinished' || method === 'Network.loadingFailed') { const r = reqs.get(params.requestId); if (r) { r.end = now(); r.failed = method === 'Network.loadingFailed'; } }
  });
  await send('Network.enable'); await send('Page.enable'); await send('Runtime.enable');
  await send('Page.navigate', {url: BASE + '/'});
  // 1. page, config and SSE come up from the board alone
  const up = await waitFor("typeof config==='object'&&config&&document.getElementById('connection').textContent==='На связи'", 30000);
  const sse = await waitFor("!!statusEvents&&statusEvents.readyState===1", 15000);
  check('page, config and SSE load over AP without internet', up && sse, {sse});
  if (!up) throw Error('page did not load; remaining browser scenarios not run');
  const masterBefore = await evaluate('state.master');
  await evaluate("strip=0;page=1;render();true");
  const slider = "document.querySelector('[data-path=\"strips.0.brightness\"]')";
  if (!await evaluate(`!!${slider}`)) throw Error('brightness slider not found');
  const move = (from, to, pitch, final = true) => evaluate(`(async()=>{const el=${slider};for(let v=${from};v!==${to};v+=Math.sign(${to}-${from})){el.value=String(v);el.dispatchEvent(new Event('input',{bubbles:true}));await new Promise(r=>setTimeout(r,${pitch}))}el.value=String(${to});el.dispatchEvent(new Event('input',{bubbles:true}));${final ? "el.dispatchEvent(new Event('change',{bubbles:true}));" : ''}return true})()`);
  const effective = () => evaluate("(async()=>{const r=await fetch('/api/command',{method:'POST',headers:{'Content-Type':'application/json','X-L8-Token':token},body:JSON.stringify({v:1,id:'e1-browser',cmd:'get_config',view:'effective',client:editorId})});return (await r.json()).result?.config?.strips?.[0]?.brightness})()");
  const idle = () => waitFor('!busy&&!previewInFlight', 15000);

  // 2. continuous movement > 1 s: an intermediate value is APPLIED on the device before release
  const baseline = () => evaluate("(async()=>{const r=await fetch('/api/command',{method:'POST',headers:{'Content-Type':'application/json','X-L8-Token':token},body:JSON.stringify({v:1,id:'e1-browser-b',cmd:'get_config'})});return (await r.json()).result?.config?.strips?.[0]?.brightness})()");
  let t0 = now(); const moving = move(10, 50, 30, false); await sleep(700);
  const mid = await effective(); await moving; const before = since(t0).filter(r => r.url.endsWith('/api/preview')).length;
  await move(50, 51, 1); await idle(); await sleep(400);
  const eff = await effective();
  check('continuous slider (>1 s): intermediate value applied before release, final value applied',
        typeof mid === 'number' && mid > 10 && mid < 50 && before >= 5 && eff === 51, {effectiveMidMovement: mid, previewsBeforeRelease: before, effectiveFinal: eff});
  check('one change request in flight (normal link)', maxConcurrent(since(t0)) === 1, {max: maxConcurrent(since(t0))});

  // 3. slow responses: latency emulation, still one request in flight and the final value delivered
  await send('Network.emulateNetworkConditions', {offline: false, latency: 400, downloadThroughput: -1, uploadThroughput: -1});
  t0 = now(); await move(51, 80, 30); await idle(); await sleep(600);
  const slowList = since(t0); const effSlow = await effective();
  check('slow link: one request in flight, latest delivered', maxConcurrent(slowList) === 1 && effSlow === 80, {requests: slowList.length, max: maxConcurrent(slowList), effectiveBrightness: effSlow});

  // 4. Save while a request is in flight: the click-time snapshot (60) is what gets saved,
  //    the later edit (33) is only previewed and stays dirty; a second Save then saves 33.
  t0 = now(); await move(80, 60, 5, false);
  await evaluate("document.getElementById('save').click();true"); await move(60, 33, 5);
  await idle(); await sleep(800);
  const seq = since(t0).map(r => r.url.replace(BASE, '') + (r.body && r.url.endsWith('/api/preview') ? ':' + JSON.parse(r.body).patch?.brightness : ''));
  const saveAt = seq.findIndex(s => s === '/api/save'); const lateAfter = seq.slice(saveAt + 1).some(s => s.startsWith('/api/preview'));
  const savedFirst = await baseline(), effAfterFirst = await effective();
  const dirtyText = await evaluate("document.getElementById('dirty').textContent"); const dirtyFlag = await evaluate('dirty');
  check('Save during in-flight request saves the click-time snapshot; later edit only previewed and dirty',
        saveAt >= 0 && lateAfter && savedFirst === 60 && effAfterFirst === 33 && dirtyFlag === true && dirtyText !== 'Записано',
        {sequence: seq, savedBaseline: savedFirst, effective: effAfterFirst, dirtyText});
  await send('Network.emulateNetworkConditions', {offline: false, latency: 0, downloadThroughput: -1, uploadThroughput: -1});
  await evaluate("document.getElementById('save').click();true"); await idle(); await sleep(1500);
  const st = await evaluate("(async()=>{const r=await fetch('/api/status');return await r.json()})()");
  const written = await waitFor("document.getElementById('dirty').textContent==='Записано'", 5000); const savedSecond = await baseline();
  check('second Save: 33 saved, "Записано" with savedRevision == configRevision, Master unchanged',
        written && savedSecond === 33 && st.savedRevision === st.configRevision && st.master === masterBefore && !st.previewActive,
        {savedBaseline: savedSecond, savedRevision: st.savedRevision, configRevision: st.configRevision, master: st.master});

  // 5. lost RESPONSE: the device applies the preview, the reply is dropped on the way back
  //    (DevTools Fetch, response stage). No automatic resend; explicit Save reads status first.
  let dropped = 0;
  listeners.push(({method, params}) => { if (method === 'Fetch.requestPaused') { if (!dropped++) send('Fetch.failRequest', {requestId: params.requestId, errorReason: 'ConnectionReset'}); else send('Fetch.continueRequest', {requestId: params.requestId}); } });
  await send('Fetch.enable', {patterns: [{urlPattern: '*/api/preview', requestStage: 'Response'}]});
  t0 = now(); await evaluate(`(()=>{const el=${slider};el.value='44';el.dispatchEvent(new Event('input',{bubbles:true}));el.dispatchEvent(new Event('change',{bubbles:true}));return true})()`);
  await sleep(2500); await send('Fetch.disable');
  const appliedOnDevice = await effective(); const sentOnce = since(t0).length; const err = await evaluate('!!previewError');
  const t1 = now(); await sleep(6000); const autoAfter = since(t1).length;
  check('lost response: device applied it, client did not resend automatically', dropped === 1 && appliedOnDevice === 44 && sentOnce === 1 && autoAfter === 0 && err,
        {dropped, appliedOnDevice, sentOnce, autoAfter, previewError: err});
  const t2 = now(); const allT2 = () => [...reqs.values()].filter(r => r.start >= t2).map(r => r.method + ' ' + r.url.replace(BASE, ''));
  await evaluate("document.getElementById('save').click();true"); await idle(); await sleep(1500);
  const order = allT2(); const iStatus = order.indexOf('GET /api/status'), iApply = order.indexOf('POST /api/apply'), iSave = order.indexOf('POST /api/save');
  const writtenAgain = await waitFor("document.getElementById('dirty').textContent==='Записано'", 5000); const savedLost = await baseline();
  check('explicit Save after lost response: status read before apply and save, 44 saved, "Записано"', iStatus >= 0 && iStatus < iApply && iApply < iSave && writtenAgain && savedLost === 44, {order, savedBaseline: savedLost});
  check('Master unchanged through the browser run', (await evaluate("(async()=>(await (await fetch('/api/status')).json()).master)()")) === masterBefore, {masterBefore});
  results.push({name: 'hidden/visible tab', ok: null, note: 'NOT RUN in headless Chrome (visibility cannot be changed reliably); covered by check_ui.mjs'});
}

let code = 1;
try { await main(); code = results.every(r => r.ok !== false) ? 0 : 1; }
catch (e) { console.log('ERROR', e.message); results.push({name: 'browser run', ok: false, error: e.message}); }
finally {
  try { ws?.close(); } catch {}
  chrome.kill('SIGTERM'); await sleep(500); try { rmSync(profile, {recursive: true, force: true}); } catch {}
  if (OUT) writeFileSync(OUT, JSON.stringify({base: BASE, results}, null, 1), {mode: 0o600});
  process.exit(code);
}
