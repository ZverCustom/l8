#!/usr/bin/env python3
"""Capture USB Wi-Fi events immediately on open (opening USB may reset the board).
Read-only status polling; no settings changes. Log contains SSIDs: keep it private.
"""
import argparse,json,os,time
from datetime import datetime,timezone
from pathlib import Path
import serial
p=argparse.ArgumentParser();p.add_argument('--port',default='/dev/cu.usbmodem1101');p.add_argument('--seconds',type=int,default=90);p.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'logs'/('wifi-boot-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')+'.log'));a=p.parse_args()
if not 15<=a.seconds<=300:p.error('--seconds must be between 15 and 300')
s=serial.Serial();s.port=a.port;s.baudrate=115200;s.timeout=.2;s.write_timeout=2;s.dtr=False;s.rts=False
a.output.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
fd=os.open(a.output,os.O_CREAT|os.O_TRUNC|os.O_WRONLY,0o600);os.fchmod(fd,0o600)
with os.fdopen(fd,'w') as log:
 try:
  s.open();start=time.monotonic();last=start
  while time.monotonic()-start<a.seconds:
   now=time.monotonic()
   if now-last>=3:s.write(b'{"cmd":"status"}\n');last=now
   line=s.readline().decode(errors='replace').rstrip()
   if not line:continue
   log.write(f'{now-start:.3f} {line}\n');log.flush()
   if 'Wi-Fi' in line or 'AP started' in line:print(line,flush=True)
   elif line.startswith('L8 {'):
    try:
     st=json.JSONDecoder().raw_decode(line[3:])[0]
     print(json.dumps({k:st.get(k) for k in ['firmware','uptimeMs','wifi','ap','master']},ensure_ascii=False),flush=True)
    except ValueError:pass
 finally:s.close()
print('Private log:',a.output)
