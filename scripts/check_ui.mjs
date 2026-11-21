// Offline regression checks for the actual embedded UI. No controller network access.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const source=readFileSync(new URL('../src/web/index.html',import.meta.url),'utf8').split('<script>')[1].split('</script>')[0];
const clone=x=>JSON.parse(JSON.stringify(x));
const config={schema:4,global:{brightness:30},network:{ssid:'test',password:'',keepPassword:true},can:{mode:2,bitrate:1},inputs:Array.from({length:3},()=>({})),presets:Array.from({length:3},()=>({used:false,name:''})),strips:Array.from({length:8},()=>({count:60,maxAmps:3,color:0xff0000}))};
const state={master:true,channels:Array.from({length:8},()=>({amps:.2,pins:[1,10,26]})),inputs:[],imu:{ok:true}};
function harness(handler){
 const elements=new Map(),timers=new Map(),calls=[];let id=0;
 const element=name=>{if(!elements.has(name))elements.set(name,{style:{},classList:{add(){},remove(){}},querySelector(){return null},textContent:'',innerHTML:'',disabled:false});return elements.get(name)};
 const context=vm.createContext({AbortController,console,document:{getElementById:element,querySelector(){return null},addEventListener(){}},setTimeout(fn,ms){timers.set(++id,{fn,ms});return id},clearTimeout(id){timers.delete(id)},fetch:async(url,options)=>{calls.push({url,options});const result=await handler(url,options);return {ok:!result.status||result.status===200,status:result.status||200,text:async()=>JSON.stringify(result.body??result)}}});
 vm.runInContext(source,context);
 return {context,element,timers,calls,run:code=>vm.runInContext(code,context),tick:async ms=>{const t=[...timers].find(([,t])=>t.ms===ms);assert.ok(t,'timer '+ms);timers.delete(t[0]);t[1].fn();await flush()}};
}
const flush=()=>new Promise(resolve=>setImmediate(resolve));
const normal=url=>url.endsWith('session')?{token:'session'}:url.endsWith('status')?clone(state):clone(config);
let release,hold=false;
const h=harness(async(url,options)=>options.method==='POST'?(hold?await new Promise(r=>release=r):{ok:true,master:true}):normal(url));
await flush();assert.match(h.element('view').innerHTML,/Восемь каналов/);assert.equal(h.element('master').disabled,false);
h.run("set('global.brightness',25)");let before=h.calls.length;await h.element('save').onclick();
assert.equal(h.calls.length-before,1);assert.equal(h.run('dirty'),false);assert.equal(h.run('state.master'),true);assert.equal(h.element('save').disabled,true);
h.run("set('global.brightness',20)");hold=true;const save=h.element('save').onclick();await flush();h.run("set('global.brightness',15)");release({ok:true,master:true});await save;
assert.equal(h.run('dirty'),true);assert.equal(h.run('config.global.brightness'),15);assert.equal(h.element('save').disabled,false);hold=false;
// A stalled boot request times out and the complete startup retries automatically.
let stall=true;
const retry=harness((url,options)=>stall?new Promise((_,reject)=>options.signal.addEventListener('abort',()=>reject(Object.assign(Error('aborted'),{name:'AbortError'})))):normal(url));
await flush();await retry.tick(8000);assert.match(retry.element('view').innerHTML,/Повторяем подключение/);stall=false;await retry.tick(2000);assert.match(retry.element('view').innerHTML,/Восемь каналов/);
// A rejected write gets a fresh session once; ambiguous failures are not retried.
let rejects=0;const expired=harness((url,options)=>options.method==='POST'?(++rejects===1?{status:403,body:{error:'Session expired'}}:{ok:true,master:false}):normal(url));
await flush();expired.run("set('global.brightness',20)");await expired.element('save').onclick();assert.equal(rejects,2);assert.equal(expired.run('state.master'),false);
let writes=0;const lost=harness((url,options)=>{if(options.method==='POST'){writes++;throw Error('connection lost')}return normal(url)});await flush();lost.run("set('global.brightness',20)");await lost.element('save').onclick();assert.equal(writes,1);assert.equal(lost.run('dirty'),true);
// Poll failures recover without discarding unsaved local edits.
let offline=false;const polling=harness(url=>{if(offline)throw Error('offline');return normal(url)});await flush();polling.run("set('global.brightness',17)");offline=true;await polling.tick(1000);offline=false;await polling.tick(1000);assert.equal(polling.run('config.global.brightness'),17);assert.equal(polling.run('dirty'),true);
// A hidden configurator must not keep extending the AP activity window.
const visibility=harness(normal);await flush();visibility.run("set('global.brightness',19)");
visibility.run('document.hidden=true');const count=visibility.calls.length;await visibility.tick(1000);
assert.equal(visibility.calls.length,count);assert.equal(visibility.run('dirty'),true);
visibility.run('document.hidden=false');await visibility.tick(1000);
assert.equal(visibility.calls.length,count+1);assert.equal(visibility.run('config.global.brightness'),19);
console.log('PASS: startup, single-request save, Master on/off, edits during save, startup timeout/retry, session renewal, no ambiguous write retry, reconnect preserves edits, hidden-tab polling pause');
