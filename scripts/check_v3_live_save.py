#!/usr/bin/env python3
"""Live bench regression: only existing enabled strips, no added loads.
Requires an explicit --live flag, a stable 12 V supply and the already tested wiring.
Tests at up to 30% global brightness, restores the full configuration with Master off.
"""
import argparse,copy,json,time,statistics
from pathlib import Path
import serial
p=argparse.ArgumentParser();p.add_argument('--port',default='/dev/cu.usbmodem1101');p.add_argument('--live',action='store_true');p.add_argument('--report',type=Path);a=p.parse_args()
if not a.live:p.error('--live is required for powered strip tests')
s=serial.Serial();s.port=a.port;s.baudrate=115200;s.timeout=.25;s.write_timeout=8;s.dtr=False;s.rts=False
checks=[];samples=[];latencies=[];original=None;complete=False

def cmd(x):
 s.write((json.dumps(x,separators=(',',':'))+'\n').encode());until=time.monotonic()+12
 while time.monotonic()<until:
  line=s.readline().decode(errors='replace').strip()
  if line.startswith('L8 {'):
   try:return json.JSONDecoder().raw_decode(line[3:])[0]
   except ValueError:pass
 raise TimeoutError(x['cmd'])
def check(name,ok):
 assert ok,name
 checks.append(name);print('PASS',name,flush=True)
def master(on):check('Master '+str(on),cmd({'cmd':'action','action':'master','on':on}).get('ok'))
def apply(c):
 start=time.monotonic();r=cmd({'cmd':'set_config','config':c});latencies.append(round((time.monotonic()-start)*1000,2));check('configuration accepted',r.get('ok'))
def status(on,outputs=True):
 time.sleep(.12);v=cmd({'cmd':'status'});samples.append({k:v[k] for k in ['firmware','uptimeMs','master','vbat','ioOk','ioFault','powerStatus','network','channels']})
 check('Master preserved '+str(on),v['master']==on)
 check('supply and protection healthy',10.5<v['vbat']<13 and v['ioOk'] and not v['ioFault'] and not any(c['fault'] for c in v['channels']))
 check('only configured outputs active',[c['on'] for c in v['channels']]==[bool(on and outputs and c['enabled']) for c in original['strips']])
 return v
try:
 s.open();time.sleep(8);original=cmd({'cmd':'get_config'});st=cmd({'cmd':'status'})
 check('expected firmware',st['firmware']=='0.3.3-dev')
 for _ in range(12):
  if st['otaConfirmed']:break
  time.sleep(1);st=cmd({'cmd':'status'})
 print('Preconditions:',json.dumps({'master':st['master'],'otaConfirmed':st['otaConfirmed'],'ign':st['ign'],'allowWithoutIgn':original['global']['allowWithoutIgn'],'autoStart':original['global']['autoStart'],'sensor5v':original['global']['sensor5v'],'enabled':[i+1 for i,c in enumerate(original['strips']) if c['enabled']],'brightness':original['global']['brightness'],'stripBrightness':[c['brightness'] for c in original['strips'][:2]]}),flush=True)
 check('bench preconditions',st['otaConfirmed'] and not st['ign'] and original['global']['allowWithoutIgn'] and not original['global']['autoStart'] and not original['global']['sensor5v'] and [i for i,c in enumerate(original['strips']) if c['enabled']]==[0,1])
 master(False)
 bench=copy.deepcopy(original);bench['global']['brightness']=min(30,original['global']['brightness'])
 for c in bench['strips']:
  c['count']=min(60,c['count']);c['center']=min(c['center'],c['count']-1)
 apply(bench);status(False)
 master(True);status(True)
 for brightness in [20,25,15,30,20,30]:
  c=copy.deepcopy(bench);c['global']['brightness']=min(brightness,bench['global']['brightness']);apply(c);status(True)
 invalid=copy.deepcopy(bench);invalid['strips'][7]['maxAmps']=100
 check('invalid settings rejected while Master stays on',cmd({'cmd':'set_config','config':invalid}).get('ok') is False);status(True)
 c=copy.deepcopy(bench);c['global']['allowWithoutIgn']=False;apply(c);status(True,False)
 apply(bench);status(True)
 check('save preset',cmd({'cmd':'action','action':'preset_save','slot':2}).get('ok'));status(True)
 check('load preset',cmd({'cmd':'action','action':'preset_load','slot':2}).get('ok'));status(True)
 master(False);apply(original);status(False)
 check('configuration restored',cmd({'cmd':'get_config'})==original)
 check('reboot for persistence',cmd({'cmd':'action','action':'reboot'}).get('ok'));time.sleep(10)
 check('configuration persisted',cmd({'cmd':'get_config'})==original);status(False)
 for _ in range(20):
  network=cmd({'cmd':'status'})['network']
  if network.startswith('STA '+original['network']['ssid']+' '):break
  time.sleep(2)
 check('selected network connected after reboot',network.startswith('STA '+original['network']['ssid']+' '))
 complete=True
 print('Apply latency over USB, ms: median',statistics.median(latencies),'max',max(latencies),flush=True)

finally:
 if a.report:a.report.write_text(json.dumps({'complete':complete,'passed':len(checks),'checks':checks,'usbApplyMs':latencies,'samples':samples},ensure_ascii=False,indent=2)+'\n')
 if original is not None:
  try:
   if not complete:cmd({'cmd':'action','action':'master','on':False})
   cmd({'cmd':'set_config','config':original});time.sleep(2)
  except Exception as e:print('Restore failed:',e,flush=True)
 s.close()
