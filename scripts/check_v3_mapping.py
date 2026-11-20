#!/usr/bin/env python3
"""Check channel metadata and schema 3 migration over USB with Master OFF throughout."""
import argparse
import copy
import json
import time
from pathlib import Path
import serial

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port',required=True)
parser.add_argument('--report',type=Path)
parser.add_argument('--previous-config',type=Path,help='Private JSON snapshot with a config key, captured before flashing')
args=parser.parse_args()
order=[1,0,5,4,3,2,7,6]
pins=[[1,10,26],[2,11,27],[3,12,28],[4,13,29],[6,15,31],[7,16,32],[8,17,33],[9,25,34]]
original=None
checks=[]
s=serial.Serial();s.port=args.port;s.baudrate=115200;s.timeout=.25;s.write_timeout=8;s.dtr=False;s.rts=False;s.open()
decoder=json.JSONDecoder()
def cmd(data):
    s.write((json.dumps(data,separators=(',',':'))+'\n').encode());s.flush()
    until=time.monotonic()+12
    while time.monotonic()<until:
        line=s.readline().decode(errors='replace').strip()
        if line.startswith('L8 {'):
            try:return decoder.raw_decode(line[3:])[0]
            except json.JSONDecodeError:pass
    raise TimeoutError(data['cmd'])
def check(name,value):
    assert value,name
    checks.append(name);print('PASS',name,flush=True)
def disarmed():
    time.sleep(.1)
    st=cmd({'cmd':'status'})
    return not st['master'] and all(not c['on'] and not c['data'] for c in st['channels'])
try:
    time.sleep(7)
    check('Master OFF precondition',disarmed())
    original=cmd({'cmd':'get_config'})
    check('no automatic loads',not original['global']['autoStart'] and not original['global']['sensor5v'])
    check('schema 4 exported',original['schema']==4)
    if args.previous_config:
        previous=json.loads(args.previous_config.read_text())['config']
        assert previous['schema']==3
        previous['schema']=4;previous['strips']=[previous['strips'][i] for i in order]
        for p in previous['presets']:
            if p['used']:p['strips']=[p['strips'][i] for i in order]
        check('flash migrated user settings without changing physical outputs',original==previous)
    st=cmd({'cmd':'status'})
    check('connector pins in order',[c['pins'] for c in st['channels']]==pins)
    check('legacy indexes match hardware',[c['legacyIndex'] for c in st['channels']]==order)
    legacy=copy.deepcopy(original);legacy['schema']=3
    for i,ch in enumerate(legacy['strips']):
        ch.update(enabled=bool(i%2),count=64+i,color=(i+1)*0x10101,brightness=70+i,maxAmps=1+i*.25,currentCal=.7+i*.1)
    preset_strips=[{k:v for k,v in ch.items() if k not in ['maxAmps','currentCal']} for ch in legacy['strips']]
    legacy['presets'][0]={'used':True,'name':'Migration test','global':{k:legacy['global'][k] for k in ['minRpm','maxRpm','speed','brightness']},'strips':preset_strips}
    expected=copy.deepcopy(legacy);expected['schema']=4
    expected['strips']=[expected['strips'][i] for i in order]
    for p in expected['presets']:
        if p['used']:p['strips']=[p['strips'][i] for i in order]
    check('legacy config accepted',cmd({'cmd':'set_config','config':legacy}).get('ok') is True)
    actual=cmd({'cmd':'get_config'})
    # Float encoding may round at the last digit; compare calibrated values separately.
    for got,want in zip(actual['strips'],expected['strips']):
        check('calibration follows output '+str(want['count']),abs(got['currentCal']-want['currentCal'])<.00001)
        want['currentCal']=got['currentCal']
    check('settings, limits and presets follow physical outputs',actual==expected)
    check('migration leaves outputs OFF',disarmed())
    check('schema 4 accepted without another reorder',cmd({'cmd':'set_config','config':actual}).get('ok') is True and cmd({'cmd':'get_config'})==actual)
    invalid=copy.deepcopy(legacy);invalid['strips'][7]['maxAmps']=100
    check('invalid legacy config rejected',cmd({'cmd':'set_config','config':invalid}).get('ok') is False)
    check('invalid legacy config makes no partial changes',cmd({'cmd':'get_config'})==actual)
    check('reboot command',cmd({'cmd':'action','action':'reboot'}).get('ok') is True)
    time.sleep(9)
    check('schema 4 persists without repeated reorder',cmd({'cmd':'get_config'})==actual)
    check('reboot keeps outputs OFF',disarmed())
    check('restore user configuration',cmd({'cmd':'set_config','config':original}).get('ok') is True)
    check('user configuration readback',cmd({'cmd':'get_config'})==original)
    if args.report:args.report.write_text(json.dumps({'passed':len(checks),'checks':checks,'pins':pins,'legacyIndexes':order},indent=2)+'\n')
    print('ALL',len(checks),'CHECKS PASSED',flush=True)
finally:
    if original is not None:
        try:
            cmd({'cmd':'action','action':'master','on':False})
            cmd({'cmd':'set_config','config':original});time.sleep(2)
        except Exception as error:print('Could not restore configuration:',error)
    s.close()
