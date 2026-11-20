#!/usr/bin/env python3
"""USB-only regression: edits and restores config/presets, then reboots L8 v3."""
import argparse
import serial,time,json,copy
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port',required=True)
parser.add_argument('--report',type=Path)
args=parser.parse_args()
original=None
s=serial.Serial();s.port=args.port;s.baudrate=115200;s.timeout=.25;s.write_timeout=8;s.dtr=False;s.rts=False;s.open()
log=[];results=[]
def cmd(data,timeout=12):
 s.write((json.dumps(data,separators=(',',':'))+'\n').encode());s.flush()
 until=time.monotonic()+timeout
 while time.monotonic()<until:
  line=s.readline().decode(errors='replace').strip()
  if line: log.append(line)
  if line.startswith('L8 {'):
   return json.loads(line[3:])
 raise TimeoutError(str(data.get('cmd')))
def check(name,value):
 assert value,name
 results.append(name);print('PASS',name,flush=True)
try:
 time.sleep(7)
 state=cmd({'cmd':'status'})
 check('USB-only test precondition',state['vbat']<1 and not state['ign'] and not state['hold'])
 check('healthy safe boot',state['ioOk'] and not state['ioFault'] and state['imu']['ok'] and state['otaConfirmed'] and not state['master'] and all(not c['on'] and not c['data'] for c in state['channels']))
 original=cmd({'cmd':'get_config'})
 check('safe configuration',not original['global']['autoStart'] and not original['global']['sensor5v'])
 check('schema 4 / eight strips / three inputs',original['schema']==4 and len(original['strips'])==8 and len(original['inputs'])==3)
 check('Wi-Fi password redacted',original['network']['password']=='' and original['network']['keepPassword'])
 for name,mutate in [
  ('wrong schema',lambda c:c.update(schema=2)),
  ('invalid IGN policy',lambda c:c['global'].update(allowWithoutIgn='true')),
  ('missing strip',lambda c:c['strips'].pop()),
  ('excessive current limit',lambda c:c['strips'][7].update(maxAmps=100)),
  ('invalid current calibration',lambda c:c['strips'][4].update(currentCal=None)),
  ('invalid input range',lambda c:c['inputs'][2].update(high=0)),
  ('out of bounds center',lambda c:c['strips'][2].update(center=1000)),
  ('invalid preset content',lambda c:c['presets'][1].update(used=True)),
 ]:
  bad=copy.deepcopy(original);mutate(bad)
  out=cmd({'cmd':'set_config','config':bad})
  check('reject '+name,out.get('ok') is False and 'error' in out)
  check('atomic reject '+name,cmd({'cmd':'get_config'})==original)
 for action in [{'action':'master','on':'true'},{'action':'reset','channel':8},{'action':'preset_load','slot':9},{'action':'nonexistent'}]:
  out=cmd({'cmd':'action',**action});check('reject action '+str(action),out.get('ok') is False)
 for allow_without_ign in [False,True]:
  changed=copy.deepcopy(original);changed['global']['allowWithoutIgn']=allow_without_ign
  check('apply IGN policy '+str(allow_without_ign),cmd({'cmd':'set_config','config':changed}).get('ok') is True)
  check('read back IGN policy '+str(allow_without_ign),cmd({'cmd':'get_config'})==changed)
  check('enable Master on USB',cmd({'cmd':'action','action':'master','on':True}).get('ok') is True)
  time.sleep(.1)
  state=cmd({'cmd':'status'})
  check('USB blocks strip power regardless of IGN policy '+str(allow_without_ign),state['allowWithoutIgn']==allow_without_ign and state['master'] and not state['ign'] and not state['hold'] and not state['ledPowerAllowed'] and all(not c['on'] and not c['data'] for c in state['channels']))
  check('disable Master on USB',cmd({'cmd':'action','action':'master','on':False}).get('ok') is True)
 changed=copy.deepcopy(original);changed['global']['brightness']=31;changed['strips'][7]['color']=0x4488ff
 check('apply config',cmd({'cmd':'set_config','config':changed}).get('ok') is True)
 check('read back config',cmd({'cmd':'get_config'})==changed)
 check('save preset',cmd({'cmd':'action','action':'preset_save','slot':0}).get('ok') is True)
 changed=cmd({'cmd':'get_config'});check('preset contains eight strips',len(changed['presets'][0]['strips'])==8)
 changed['global']['brightness']=19
 check('apply temporary change',cmd({'cmd':'set_config','config':changed}).get('ok') is True)
 check('load preset',cmd({'cmd':'action','action':'preset_load','slot':0}).get('ok') is True)
 check('preset restored brightness',cmd({'cmd':'get_config'})['global']['brightness']==31)
 check('restore initial config',cmd({'cmd':'set_config','config':original}).get('ok') is True)
 check('reboot command',cmd({'cmd':'action','action':'reboot'}).get('ok') is True)
 time.sleep(9)
 check('config persists after reboot',cmd({'cmd':'get_config'})==original)
 state=cmd({'cmd':'status'})
 check('safe healthy reboot',state['ioOk'] and state['imu']['ok'] and state['otaConfirmed'] and not state['master'] and not state['sensorOn'] and all(not c['on'] and not c['data'] for c in state['channels']))
 # Oversize input must discard the remainder, including a syntactically valid tail.
 s.write(b' ' * 24001 + b'{"cmd":"action","action":"master","on":true}\n');s.flush()
 time.sleep(1);s.reset_input_buffer()
 check('oversize line discarded through newline',cmd({'cmd':'status'})['master'] is False)
 state.pop('network',None);state.pop('ip',None)
 if args.report: args.report.write_text(json.dumps({'passed':len(results),'checks':results,'finalStatus':state},indent=2)+'\n')
 print('ALL',len(results),'CHECKS PASSED',flush=True)
finally:
 if original is not None:
  try:
   cmd({'cmd':'action','action':'master','on':False})
   cmd({'cmd':'set_config','config':original})
   time.sleep(2)  # deferred save
  except Exception as error:
   print('Could not restore configuration:',error)
 s.close()
