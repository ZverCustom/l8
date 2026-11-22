// Offline tests execute the actual embedded UI, including asynchronous races.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const source=readFileSync(new URL('../src/web/index.html',import.meta.url),'utf8').split('<script>')[1].split('</script>')[0];
const clone=x=>JSON.parse(JSON.stringify(x));
const config={schema:5,global:{brightness:30,speed:60,autoRetry:true},network:{ssid:'test',password:'',keepPassword:true},can:{mode:2,bitrate:1},inputs:Array.from({length:3},()=>({})),presets:Array.from({length:3},()=>({used:false,name:''})),strips:Array.from({length:8},()=>({count:60,maxAmps:3,color:0xff0000,speed:60,brightness:30}))};
const state={master:true,configRevision:10,savedRevision:10,bootId:'boot-a',previewActive:false,channels:Array.from({length:8},()=>({amps:.2,pins:[1,10,26]})),inputs:[],imu:{ok:true}};
const normal=url=>url.endsWith('session')?{token:'session'}:url.endsWith('status')?clone(state):clone(config);
const flush=()=>new Promise(resolve=>setImmediate(resolve));
function harness(handler,{push=true}={}){
 const elements=new Map(),timers=new Map(),calls=[],listeners={},events=[];let id=0,revision=10,saved=10;
 const element=name=>{if(!elements.has(name))elements.set(name,{style:{},classList:{add(){},remove(){}},querySelector(){return null},textContent:'',innerHTML:'',disabled:false});return elements.get(name)};
 class EventSource{constructor(url){this.url=url;this.listeners={};events.push(this)}addEventListener(name,fn){this.listeners[name]=fn}close(){this.closed=true}}
 const context=vm.createContext({AbortController,console,...(push?{EventSource}:{}),document:{getElementById:element,querySelector(){return null},addEventListener(name,fn){(listeners[name]??=[]).push(fn)}},setTimeout(fn,ms){timers.set(++id,{fn,ms});return id},clearTimeout(id){timers.delete(id)},fetch:async(url,options)=>{
  calls.push({url,options});let result=handler?await handler(url,options):undefined;
  if(result===undefined)result=options.method==='POST'?(url==='/api/touch'||url==='/api/action'?{ok:true,master:true,revision,savedRevision:saved}:(()=>{const r=++revision;if(url==='/api/save')saved=r;return {ok:true,master:true,revision:r,savedRevision:saved}})()):normal(url);
  return {ok:!result.status||result.status===200,status:result.status||200,text:async()=>JSON.stringify(result.body??result)};
 }});
 vm.runInContext(source,context);
 return {context,element,timers,calls,events,run:code=>vm.runInContext(code,context),emit:(name,event={})=>(listeners[name]??[]).forEach(fn=>fn(event)),tick:async ms=>{const t=[...timers].find(([,t])=>t.ms===ms);assert.ok(t,'timer '+ms);timers.delete(t[0]);t[1].fn();await flush()},tickIf:async ms=>{const t=[...timers].find(([,t])=>t.ms===ms);if(!t)return false;timers.delete(t[0]);t[1].fn();await flush();return true}};
}
const h=harness();await flush();assert.match(h.element('view').innerHTML,/Восемь каналов/);assert.equal(h.events.length,1);
// Coalesce slider inputs into one small patch; Save sends only a revision.
// First input goes out at once; inputs during the interval coalesce to the latest values.
h.run("set('strips.1.color',255);set('strips.1.brightness',20);set('strips.1.brightness',21)");await flush();
let calls=h.calls.filter(c=>c.options.method==='POST');assert.equal(calls.length,1);assert.equal(calls[0].url,'/api/preview');assert.deepEqual(JSON.parse(calls[0].options.body).patch,{color:255});
await h.tick(100);calls=h.calls.filter(c=>c.options.method==='POST');assert.equal(calls.length,2);
const patch=JSON.parse(calls[1].options.body);assert.deepEqual(patch.patch,{brightness:21});assert.equal(patch.strip,1);assert.ok(calls[1].options.body.length<250);
let before=h.calls.length;await h.element('save').onclick();assert.equal(h.calls.length-before,1);assert.equal(h.calls.at(-1).url,'/api/save');assert.equal(JSON.parse(h.calls.at(-1).options.body).config,undefined);assert.equal(h.run('dirty'),false);assert.equal(h.run('state.master'),true);
// Structural edits apply in RAM first; write is a distinct checked revision.
h.run("set('global.brightness',25)");before=h.calls.length;await h.element('save').onclick();assert.deepEqual(h.calls.slice(before).map(c=>c.url),['/api/apply','/api/save']);
// New edits while flash save is pending survive its acknowledgement.
let release,hold=false;const concurrent=harness(async(url)=>{if(hold&&url==='/api/save')return new Promise(r=>release=r)});
await flush();concurrent.run("set('strips.0.color',255)");await concurrent.tick(100);hold=true;const saving=concurrent.element('save').onclick();await flush();concurrent.run("set('strips.0.color',65280)");release({ok:true,master:true,revision:12});await saving;
assert.equal(concurrent.run('dirty'),true);assert.equal(concurrent.run('config.strips[0].color'),65280);assert.equal(concurrent.element('save').disabled,false);
// Startup timeout retries, but ambiguous writes never retry automatically.
let stall=true;const retry=harness((url,options)=>stall?new Promise((_,reject)=>options.signal.addEventListener('abort',()=>reject(Object.assign(Error('aborted'),{name:'AbortError'})))):undefined);
await flush();await retry.tick(8000);assert.match(retry.element('view').innerHTML,/Повторяем подключение/);stall=false;await retry.tick(2000);assert.match(retry.element('view').innerHTML,/Восемь каналов/);
// A lost write is never replayed by timers; only an explicit Save retries, after reading status.
let writes=0;const lost=harness((url,options)=>{if(options.method==='POST'){writes++;throw Error('connection lost')}});await flush();lost.run("set('strips.0.color',255)");await flush();
assert.equal(writes,1);assert.ok(lost.run('previewError'));await lost.tick(100);lost.run("set('strips.0.color',254)");await flush();assert.equal(writes,1);
let mark=lost.calls.length;await lost.element('save').onclick();assert.deepEqual(lost.calls.slice(mark).map(c=>c.url),['/api/status','/api/apply']);assert.equal(writes,2);assert.equal(lost.run('dirty'),true);assert.ok(lost.run('previewError'));
// Explicit 403 renews the token once; rejected revision is not overwritten.
let rejects=0;const expired=harness((url,options)=>options.method==='POST'&&++rejects===1?{status:403,body:{error:'Session expired'}}:undefined);
await flush();expired.run("set('strips.0.color',255)");await expired.element('save').onclick();assert.equal(rejects,3);assert.equal(expired.run('dirty'),false);
const conflict=harness((url,options)=>options.method==='POST'?{status:409,body:{error:'Configuration changed'}}:undefined);await flush();conflict.run("set('strips.0.color',255)");await conflict.tick(100);assert.equal(conflict.calls.filter(c=>c.options.method==='POST').length,1);assert.equal(conflict.run('dirty'),true);
// A validation error can be corrected, with no automatic retry of bad data.
let invalid=true;const validation=harness((url,options)=>options.method==='POST'&&invalid?{status:400,body:{error:'Invalid setting'}}:undefined);await flush();validation.run("set('global.maxRpm',0)");await validation.tick(100);assert.equal(validation.run('previewError'),null);invalid=false;validation.run("set('global.maxRpm',7000)");await validation.tick(100);assert.equal(validation.run('previewOpen'),true);
// SSE status delivery needs no poll; hidden tabs close the stream and lease.
const visibility=harness();await flush();const stream=visibility.events.at(-1);stream.listeners.status({data:JSON.stringify({...state,master:false})});assert.equal(visibility.run('state.master'),false);
visibility.run('document.hidden=true');visibility.emit('visibilitychange');assert.equal(stream.closed,true);const count=visibility.calls.length;await visibility.tick(5000);assert.equal(visibility.calls.length,count);
visibility.run('document.hidden=false');visibility.emit('visibilitychange');assert.equal(visibility.events.length,2);await visibility.tick(5000);assert.equal(visibility.calls.length,count);
// No EventSource support: a five-second fallback recovers without losing edits.
const fallback=harness(undefined,{push:false});await flush();fallback.run("set('global.brightness',19)");await fallback.tick(5000);assert.equal(fallback.run('config.global.brightness'),19);assert.equal(fallback.run('dirty'),true);
// Schema 4 / saved presets inherit their old global animation speed.
const legacy=clone(config);legacy.schema=4;legacy.global.speed=42;legacy.presets[0]={used:true,name:'old',global:{speed:17},strips:clone(legacy.strips)};
h.context.legacy=legacy;const migrated=h.run('migrateConfig(legacy)');assert.equal(migrated.schema,5);assert.equal(migrated.strips[0].speed,42);assert.equal(migrated.presets[0].strips[0].speed,17);
// channelState: the firmware reason is authoritative; flags only add detail.
const cs=c=>{h.context.ch=c;return h.run('channelState(ch)')};h.run('state.ioFault=false;state.totalBrightnessScale=1');
const pending={on:false,fault:'overcurrent',retryPending:true,retryInMs:0,retries:0};
for(const [reason,text] of [['novbat','Нет 12 В'],['ign','Ожидание IGN'],['master','Master выключен'],['inhibit','Выходы заблокированы'],['disabled','Канал запрещён'],['engine','Двигатель заглушен']]){const out=cs({...pending,reason});assert.ok(out.startsWith(text),reason+': '+out);assert.doesNotMatch(out,/ожидает разрешения/)}
assert.equal(cs({...pending,reason:'retry',retryInMs:4200,retries:1}),'Повтор 2/3 через 5 с');
assert.equal(cs({on:false,fault:'switch fault',retryPending:false,reason:'fault'}),'Защита силового ключа');
assert.equal(cs({on:false,fault:'',retryPending:false,reason:'fault'}),'Защита канала');
assert.equal(cs({on:true,fault:'',reason:'on',brightnessScale:1}),'Включён');
assert.equal(cs({on:true,fault:'',reason:'on',brightnessScale:.5}),'Ограничение тока · 50% яркости');
assert.equal(cs({on:false,fault:'',reason:'io'}),'Состояние неизвестно');
for(const on of [true,false]){const out=cs({on,fault:'',reason:'future_code',brightnessScale:1});assert.equal(out,'Неизвестное состояние (future_code)');assert.doesNotMatch(out,/Включён/)}
assert.equal(cs({on:false,fault:'overcurrent',retryPending:true,retryInMs:0,retries:0}),'Повтор ожидает разрешения питания');
assert.equal(cs({on:false,fault:'',retryPending:false}),'Выключен');assert.equal(cs({on:true,fault:'',brightnessScale:1}),'Включён');
h.run('state.ioFault=true');assert.equal(cs({on:false,fault:''}),'Состояние неизвестно');assert.equal(cs({on:false,fault:'',reason:'novbat'}),'Нет 12 В');h.run('state.ioFault=false');
// ---- P1-D live preview ----
const posts=x=>x.calls.filter(c=>c.options.method==='POST');
const bodyOf=c=>JSON.parse(c.options.body);
// 1. Continuous input faster than the interval for >1 s: sends keep coming, each carries the latest value.
{const x=harness();await flush();let v=0;
 for(let step=0;step<40;step++){x.run(`set('strips.2.brightness',${++v})`);await flush();if(step%3===2)await x.tickIf(100)} // 30 ms input pitch, 40 inputs = 1.2 s
 await x.tickIf(100);const sent=posts(x);assert.ok(sent.length>=13&&sent.length<=15,'sends during continuous input: '+sent.length);
 assert.equal(bodyOf(sent.at(-1)).patch.brightness,40);for(let i=1;i<sent.length;i++)assert.ok(bodyOf(sent[i]).patch.brightness>bodyOf(sent[i-1]).patch.brightness);}
// 2. Response slower than the interval: one change request in flight, then only the latest values.
{let open=0,maxOpen=0,hold=[];const x=harness((url,o)=>{if(o.method!=='POST')return;open++;maxOpen=Math.max(maxOpen,open);return new Promise(r=>hold.push(()=>{open--;r(undefined)}))});
 await flush();x.run("set('strips.0.color',1)");await flush();for(let v=2;v<=9;v++){x.run(`set('strips.0.color',${v})`);x.run(`set('strips.3.speed',${v})`);await flush();if(v%3===0)await x.tickIf(100)}
 assert.equal(posts(x).length,1);hold.shift()();await flush();assert.equal(posts(x).length,2);
 const next=posts(x).map(bodyOf);assert.deepEqual(next.slice(1).map(b=>[b.strip,b.patch]),[[0,{color:9}]]);
 hold.shift()();await flush();assert.deepEqual(bodyOf(posts(x)[2]).patch,{speed:9});assert.equal(maxOpen,1);}
// 3. Full apply mixed with patches keeps the freshest values; old responses do not roll back local edits.
{let hold;const x=harness((url,o)=>{if(o.method==='POST'&&!hold)return new Promise(r=>hold=r)});await flush();
 x.run("set('strips.0.color',5)");await flush();x.run("set('global.brightness',44);set('strips.1.brightness',7)");await flush();
 hold(undefined);await flush();await x.tick(100);const last=posts(x).at(-1);assert.equal(last.url,'/api/apply');const cfg=bodyOf(last).config;
 assert.equal(cfg.global.brightness,44);assert.equal(cfg.strips[1].brightness,7);assert.equal(cfg.strips[0].color,5);assert.equal(x.run('config.strips[0].color'),5);}
// 4. Final change/pointerup: no extra request for an already sent value; while busy it waits and is sent next.
{let hold=null;const x=harness((url,o)=>{if(o.method==='POST'&&hold!==null)return new Promise(r=>hold=r)});await flush();
 x.run("set('strips.4.brightness',50)");await flush();await x.tick(100);const n=posts(x).length;x.run("set('strips.4.brightness',50)");await flush();assert.equal(posts(x).length,n);
 hold=true;x.run("set('strips.4.brightness',60)");await flush();assert.equal(posts(x).length,n+1);x.run("set('strips.4.brightness',61)");x.run("set('strips.4.brightness',62)");await flush();
 assert.equal(posts(x).length,n+1);const r=hold;hold=null;r(undefined);await flush();await x.tick(100);assert.equal(bodyOf(posts(x).at(-1)).patch.brightness,62);}
// 5. Save while a preview is in flight, and edits made during Save stay unsaved.
{let holdPreview=null,holdSave=null;const x=harness((url,o)=>{if(url==='/api/preview'&&holdPreview===true)return new Promise(r=>holdPreview=r);if(url==='/api/save'&&holdSave===true)return new Promise(r=>holdSave=r)});
 await flush();holdPreview=true;x.run("set('strips.0.color',100)");await flush();x.run("set('strips.0.color',101)");
 holdSave=true;const saving=x.element('save').onclick();await flush();x.run("set('strips.0.color',202)");
 const p=holdPreview;holdPreview=null;p(undefined);await flush();
 const seq=posts(x).map(c=>[c.url,c.url==='/api/preview'?bodyOf(c).patch.color:null]);assert.deepEqual(seq,[['/api/preview',100],['/api/preview',101],['/api/save',null]]);
 const s1=holdSave;holdSave=null;s1(undefined);await saving;await flush();
 assert.deepEqual(posts(x).slice(3).map(c=>[c.url,bodyOf(c).patch.color]),[['/api/preview',202]]);assert.equal(x.run('dirty'),true);
 assert.notEqual(x.element('dirty').textContent,'Записано');assert.equal(x.run('config.strips[0].color'),202);}
// 6. «Записано» only with savedRevision matching the saved revision.
{const x=harness();await flush();x.run("set('strips.0.color',7)");await flush();await x.element('save').onclick();await flush();assert.equal(x.element('dirty').textContent,'Записано');
 const lag=harness((url,o)=>url==='/api/save'?{ok:true,master:true,revision:20,savedRevision:15}:undefined);await flush();lag.run("set('strips.0.color',7)");await flush();await lag.element('save').onclick();
 assert.notEqual(lag.element('dirty').textContent,'Записано');assert.match(lag.element('toast').textContent,/не подтверждена/);}
// 7. Conflict / foreign editor: reconcile reads status and does not override a foreign lease.
{let stage=0;const x=harness((url,o)=>{if(url==='/api/preview'&&stage===0){stage=1;return {status:409,body:{error:'Configuration changed or another editor is active; reload'}}}if(url==='/api/status'&&stage===1)return {...clone(state),previewActive:true,previewOwner:'other-tab'}});
 await flush();x.run("set('strips.0.color',9)");await flush();assert.ok(x.run('previewError'));const m=x.calls.length;await x.element('save').onclick();
 assert.deepEqual(x.calls.slice(m).map(c=>c.url),['/api/status']);assert.equal(x.run('dirty'),true);assert.match(x.element('toast').textContent,/другой редактор/);}
// 8. Hidden tab: no lease renewal; on return a lost lease stops pending edits.
{const x=harness((url,o)=>url==='/api/status'&&x.context.__expired?{...clone(state),previewActive:false}:undefined);await flush();
 x.run("set('strips.0.color',3)");await flush();assert.equal(x.run('previewOpen'),true);x.run('document.hidden=true');x.emit('visibilitychange');
 const n=x.calls.length;await x.tick(10000);assert.equal(x.calls.length,n);x.run("set('strips.0.color',4)");await flush();assert.equal(posts(x).length,1);
 x.context.__expired=true;x.run('document.hidden=false');x.emit('visibilitychange');await flush();
 assert.ok(x.run('previewError'));assert.equal(posts(x).length,1);assert.equal(x.run('dirty'),true);}
// 9. Reboot observed (bootId change) stops the old chain; old edits are not shown as saved.
{const x=harness();await flush();x.run("set('strips.0.color',3)");await flush();const stream=x.events.at(-1);
 stream.listeners.status({data:JSON.stringify({...state,bootId:'boot-b',configRevision:1,savedRevision:1})});assert.ok(x.run('previewError'));
 x.run("set('strips.0.color',4)");await flush();assert.equal(posts(x).length,1);assert.notEqual(x.element('dirty').textContent,'Записано');assert.equal(x.run('previewOpen'),false);}
// 10. Master is untouched by preview and Save (no action request, state kept).
{const x=harness();await flush();x.run("set('strips.0.color',3)");await flush();await x.element('save').onclick();
 assert.equal(x.calls.filter(c=>c.url==='/api/action').length,0);assert.equal(x.run('state.master'),true);for(const c of posts(x))assert.equal('master' in bodyOf(c),false);}
// ---- P1-006 review: R5 (refused snapshot is not "saved"), R6 (reboot cancels the old batch), real DOM events ----
// R5a: Save while a structural apply is in flight, apply refused with 400.
{let hold;const x=harness((url,o)=>{if(url==='/api/apply'&&!hold)return new Promise(r=>hold=r)});await flush();
 x.run("set('global.brightness',44)");await flush();const saving=x.element('save').onclick();await flush();
 const before=posts(x).length;hold({status:400,body:{error:'Invalid global setting'}});await saving;await flush();
 // Save re-applies the click snapshot as a full apply (accepted this time) and only then saves it.
 const urls=posts(x).map(c=>c.url);assert.deepEqual(urls,['/api/apply','/api/apply','/api/save']);assert.equal(bodyOf(posts(x)[1]).config.global.brightness,44);}
// R5b: same race, the device keeps refusing: nothing is declared written, dirty stays, Save stays possible.
{let first;const x=harness((url,o)=>{if(url==='/api/apply'){if(!first)return new Promise(r=>first=r);return {status:400,body:{error:'Invalid global setting'}}}});await flush();
 x.run("set('global.brightness',44)");await flush();const saving=x.element('save').onclick();await flush();first({status:400,body:{error:'Invalid global setting'}});await saving;await flush();
 assert.equal(x.run('dirty'),true);assert.notEqual(x.element('dirty').textContent,'Записано');assert.equal(x.element('save').disabled,false);
 assert.equal(posts(x).filter(c=>c.url==='/api/save').length,0);assert.equal(x.run('config.global.brightness'),44);assert.doesNotMatch(x.element('toast').textContent,/Изменений для записи нет/);
}
// R5b continued: after the user corrects the value it is applied and saved normally.
{let refuse=true,first;const x=harness((url,o)=>{if(url==='/api/apply'&&refuse){if(!first)return new Promise(r=>first=r);return {status:400,body:{error:'bad'}}}});await flush();
 x.run("set('global.brightness',44)");await flush();const saving=x.element('save').onclick();await flush();first({status:400,body:{error:'bad'}});await saving;await flush();
 refuse=false;x.run("set('global.brightness',45)");await flush();await x.tickIf(100);await x.element('save').onclick();await flush();
 assert.equal(x.run('dirty'),false);assert.equal(x.element('dirty').textContent,'Записано');const applied=posts(x).filter(c=>c.url==='/api/apply').map(c=>bodyOf(c).config.global.brightness);assert.equal(applied.at(-1),45);}
// R5c: an older preview is open; the newer apply is refused while Save waits: the old preview must not be saved as the new snapshot.
{let hold;const x=harness((url,o)=>{if(url==='/api/apply'){if(!hold)return new Promise(r=>hold=r);return {status:400,body:{error:'bad'}}}});await flush();
 x.run("set('strips.0.color',7)");await flush();await x.tickIf(100);assert.equal(x.run('previewOpen'),true);
 x.run("set('global.brightness',44)");await flush();const saving=x.element('save').onclick();await flush();hold({status:400,body:{error:'bad'}});await saving;await flush();
 assert.equal(posts(x).filter(c=>c.url==='/api/save').length,0);assert.equal(x.run('dirty'),true);assert.notEqual(x.element('dirty').textContent,'Записано');}
// R6a: reboot observed while a multi-channel batch waits for a reply: the rest is not sent, the late reply changes nothing.
{const holds=[];const x=harness((url,o)=>{if(url==='/api/preview')return new Promise(r=>holds.push(r))});await flush();
 x.run("set('strips.0.color',1)");await flush();x.run("set('strips.1.color',2);set('strips.2.color',3)");holds.shift()(undefined);await flush();await x.tickIf(100);
 assert.equal(posts(x).length,2);assert.equal(bodyOf(posts(x)[1]).strip,1);const revBefore=x.run('serverRevision');
 x.events.at(-1).listeners.status({data:JSON.stringify({...state,bootId:'boot-b',configRevision:1,savedRevision:1})});
 holds.shift()({ok:true,master:true,revision:77,savedRevision:10});await flush();await x.tickIf(100);await x.tickIf(100);
 assert.equal(posts(x).length,2);assert.equal(x.run('previewOpen'),false);assert.ok(x.run('previewError'));assert.notEqual(x.run('serverRevision'),77);assert.equal(x.run('serverRevision'),revBefore);
 assert.equal(x.run('config.strips[2].color'),3);assert.equal(x.run('dirty'),true);}
// R6b: Save already pressed when the reboot is observed: no save of the old session.
{const holds=[];const x=harness((url,o)=>{if(url==='/api/preview')return new Promise(r=>holds.push(r))});await flush();
 x.run("set('strips.0.color',1)");await flush();x.run("set('strips.1.color',2)");const saving=x.element('save').onclick();await flush();
 x.events.at(-1).listeners.status({data:JSON.stringify({...state,bootId:'boot-b',configRevision:1,savedRevision:1})});
 holds.shift()({ok:true,master:true,revision:55,savedRevision:10});await saving;await flush();
 assert.deepEqual(posts(x).map(c=>c.url),['/api/preview']);assert.equal(x.run('dirty'),true);assert.notEqual(x.element('dirty').textContent,'Записано');}
// R6c: without a reboot the same batch is applied to the end (latest values).
{const x=harness();await flush();x.run("set('strips.0.color',1)");await flush();x.run("set('strips.1.color',2);set('strips.2.color',3)");await x.tickIf(100);
 assert.deepEqual(posts(x).map(c=>bodyOf(c).strip),[0,1,2]);assert.ok(!x.run('previewError'));}
// Real DOM handlers: range input events then the final change event deliver the final value once.
{const x=harness();await flush();const slider=v=>({dataset:{path:'strips.5.brightness'},type:'range',value:String(v)});
 for(const v of [10,20,30])x.emit('input',{target:slider(v)});await flush();await x.tickIf(100);
 const n=posts(x).length;assert.deepEqual(posts(x).map(c=>bodyOf(c).patch.brightness),[10,30]);
 x.emit('change',{target:slider(30)});await flush();assert.equal(posts(x).length,n); // pointer released on the sent value: no request
 x.emit('input',{target:slider(31)});x.emit('change',{target:slider(32)});await flush();await x.tickIf(100);assert.equal(bodyOf(posts(x).at(-1)).patch.brightness,32);
 const color={dataset:{path:'strips.5.color'},type:'color',value:'#00ff00'};x.emit('input',{target:color});await flush();await x.tickIf(100);assert.equal(bodyOf(posts(x).at(-1)).patch.color,0x00ff00);}
// ---- P1-007 review: R6 path through the 403 token refresh ----
// R6d: preview gets 403, the token refresh is delayed, a reboot is observed meanwhile: no second preview.
{let holdSession=null,first=true;const x=harness((url,o)=>{
  if(url==='/api/preview'&&first){first=false;return {status:403,body:{error:'Session expired'}}}
  if(url==='/api/session'&&holdSession===true)return new Promise(r=>holdSession=r)});
 await flush();holdSession=true;x.run("set('strips.0.color',9)");await flush();
 assert.equal(posts(x).length,1);x.events.at(-1).listeners.status({data:JSON.stringify({...state,bootId:'boot-b',configRevision:1,savedRevision:1})});
 const r=holdSession;holdSession=null;r({token:'new'});await flush();await x.tickIf(100);
 assert.equal(posts(x).length,1);assert.equal(x.run('previewOpen'),false);assert.equal(x.run('dirty'),true);assert.ok(x.run('previewError'));}
// R6e: Save's own save request gets 403 and the session changes during the refresh: save is not resent.
{let holdSession=null,saveSeen=0;const x=harness((url,o)=>{
  if(url==='/api/save'&&++saveSeen===1)return {status:403,body:{error:'Session expired'}};
  if(url==='/api/session'&&holdSession===true)return new Promise(r=>holdSession=r)});
 await flush();x.run("set('strips.0.color',9)");await flush();holdSession=true;const saving=x.element('save').onclick();await flush();
 x.events.at(-1).listeners.status({data:JSON.stringify({...state,bootId:'boot-b',configRevision:1,savedRevision:1})});
 const r=holdSession;holdSession=null;r({token:'new'});await saving;await flush();
 assert.equal(saveSeen,1);assert.equal(x.run('dirty'),true);assert.notEqual(x.element('dirty').textContent,'Записано');}
// R6f (control): without a reboot an explicit 403 is refreshed once and the write goes through.
{let first=true;const x=harness((url,o)=>{if(url==='/api/preview'&&first){first=false;return {status:403,body:{error:'Session expired'}}}});
 await flush();x.run("set('strips.0.color',9)");await flush();assert.equal(posts(x).length,2);assert.equal(x.run('previewOpen'),true);assert.ok(!x.run('previewError'));}
console.log('PASS: small preview/coalescing, explicit save, structural apply, concurrent edits, startup retry, token renewal, ambiguous-write stop, revision conflict, validation recovery, SSE, hidden tabs, fallback, migration, channel reason, live preview (continuous, slow reply, mixed apply, final value, save snapshot, savedRevision, conflict, hidden tab, reboot, master), review R5/R6, DOM input/change, R6 token-refresh path');
