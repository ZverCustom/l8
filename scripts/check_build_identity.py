#!/usr/bin/env python3
"""Verify the linked binary, not only the generated header (no device needed)."""
import json
import re
import subprocess
from pathlib import Path
root = Path(__file__).resolve().parents[1]
revision = subprocess.check_output(['git', 'describe', '--tags', '--always', '--dirty'], cwd=root, text=True).strip()
version = root.joinpath('VERSION').read_text().strip() + '+' + revision
for name in ('l8_v3', 'l8_v3_dma', 'l8_v3_dma_flash'):
    build = root / '.pio/build' / name
    header = build.joinpath('generated/l8_version.h').read_text()
    values = dict((key, json.loads(value)) for key, value in re.findall(r'^#define (\w+) (".*")$', header, re.M))
    assert values['GIT_REV'] == revision and values['L8_FW_VERSION'] == version, name
    binary = build.joinpath('firmware.bin').read_bytes()
    assert version.encode() + b'\0' in binary, f'{name}: stale linked firmware identity'
    assert revision.encode() + b'\0' in binary, f'{name}: stale build revision'
    print(f'PASS {name}: {version} in linked binary')
