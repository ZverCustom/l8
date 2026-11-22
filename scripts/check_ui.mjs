// Offline tests execute the actual embedded UI, including asynchronous races.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const source=readFileSync(new URL('../src/web/index.html',import.meta.url),'utf8').split('<script>')[1].split('</script>')[0];
const clone=x=>JSON.parse(JSON.stringify(x));
const config={schema:5,global:{brightness:30,speed:60,autoRetry:true},network:{ssid:'test',password:'',keepPassword:true},can:{mode:2,bitrate:1},inputs:Array.from({length:3},()=>({})),presets:Array.from({length:3},()=>({used:false,name:''})),strips:Array.from({length:8},()=>({count:60,maxAmps:3,color:0xff0000,speed:60,brightness:30}))};
const state={master:true,configRevision:10,channels:Array.from({length:8},()=>({amps:.2,pins:[1,10,26]})),inputs:[],imu:{ok:true}};
const normal=url=>url.endsWith('session')?{token:'session'}:url.endsWith('status')?clone(state):clone(config);
const flush=()=>new Promise(resolve=>setImmediate(resolve));
function harness(handler,{push=true}={}){
 const elements=new Map(),timers=new Map(),calls=[],listeners={},events=[];let id=0,revision=10;
 const element=name=>{if(!elements.has(name))elements.set(name,{style:{},classList:{add(){},remove(){}},querySelector(){return null},textContent:'',innerHTML:'',disabled:false});return elements.get(name)};
 class EventSource{constructor(url){this.url=url;this.listeners={};events.push(this)}addEventListener(name,fn){this.listeners[name]=fn}close(){this.closed=true}}
 const context=vm.createContext({AbortController,console,...(push?{EventSource}:{}),document:{getElementById:element,querySelector(){return null},addEventListener(name,fn){(listeners[name]??=[]).push(fn)}},setTimeout(fn,ms){timers.set(++id,{fn,ms});return id},clearTimeout(id){timers.delete(id)},fetch:async(url,options)=>{
  calls.push({url,options});let result=handler?await handler(url,options):undefined;
  if(result===undefined)result=options.method==='POST'?{ok:true,master:true,revision:++revision}:normal(url);
  return {ok:!result.status||result.status===200,status:result.status||200,text:async()=>JSON.stringify(result.body??result)};
 }});
 vm.runInContext(source,context);
 return {context,element,timers,calls,events,run:code=>vm.runInContext(code,context),emit:(name,event={})=>(listeners[name]??[]).forEach(fn=>fn(event)),tick:async ms=>{const t=[...timers].find(([,t])=>t.ms===ms);assert.ok(t,'timer '+ms);timers.delete(t[0]);t[1].fn();await flush()}};
}
const h=harness();await flush();assert.match(h.element('view').innerHTML,/Восемь каналов/);assert.equal(h.events.length,1);
// Coalesce slider inputs into one small patch; Save sends only a revision.
h.run("set('strips.1.color',255);set('strips.1.brightness',20);set('strips.1.brightness',21)");
await h.tick(100);let calls=h.calls.filter(c=>c.options.method==='POST');assert.equal(calls.length,1);assert.equal(calls[0].url,'/api/preview');
const patch=JSON.parse(calls[0].options.body);assert.deepEqual(patch.patch,{color:255,brightness:21});assert.equal(patch.strip,1);assert.ok(calls[0].options.body.length<250);
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
let writes=0;const lost=harness((url,options)=>{if(options.method==='POST'){writes++;throw Error('connection lost')}});await flush();lost.run("set('strips.0.color',255)");await lost.element('save').onclick();assert.equal(writes,1);assert.equal(lost.run('dirty'),true);assert.ok(lost.run('previewError'));
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
console.log('PASS: small preview/coalescing, explicit save, structural apply, concurrent edits, startup retry, token renewal, ambiguous-write stop, revision conflict, validation recovery, SSE, hidden tabs, fallback, migration');
