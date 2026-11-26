#!/usr/bin/env python3
"""Pinned firmware image sets for the E1/WDT session (old 09e092a vs the Stream-yield fix).

collect: copy one built RMT image set (bootloader, partitions, boot_app0, firmware.bin/.elf)
         into a private directory and write manifest.json (0600) with source commit, sizes,
         SHA256, framework/toolchain versions, flash arguments and whether the compiled
         Stream::timedRead calls delay() (the d4b9127 patch), read from the ELF disassembly.
verify:  re-hash a manifest's files and check the expected Stream property per role:
         old = no delay() in Stream::timedRead, new = delay() present.
The orchestrator flashes only image sets that pass verify."""
import argparse, hashlib, json, os, re, shutil, subprocess, sys
from pathlib import Path

FILES = ['bootloader.bin', 'partitions.bin', 'boot_app0.bin', 'firmware.bin']
# ESP32-S3 offsets, as PlatformIO espressif32@6.8.0 uploads an Arduino image.
OFFSETS = {'bootloader.bin': '0x0', 'partitions.bin': '0x8000', 'boot_app0.bin': '0xe000', 'firmware.bin': '0x10000'}
EXPECT_YIELD = {'old': False, 'new': True}


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''): h.update(chunk)
    return h.hexdigest()


def package_version(core, name):
    try: return json.loads((Path(core) / 'packages' / name / 'package.json').read_text()).get('version')
    except (OSError, ValueError): return None


def timed_read_calls_delay(disasm):
    """From objdump -d text: True if Stream::timedRead calls delay(), False if not, None if absent."""
    m = re.search(r'^[0-9a-f]+ <_ZN6Stream9timedReadEv>:\n(.*?)(?=^\n|^[0-9a-f]+ <|\Z)', disasm, re.M | re.S)
    if not m: return None
    return bool(re.search(r'call\w*\s+[0-9a-f]+ <delay>', m.group(1)))


def stream_yield(objdump, elf):
    out = subprocess.run([objdump, '-d', '--no-show-raw-insn', str(elf)], capture_output=True, text=True, timeout=300).stdout
    return timed_read_calls_delay(out)


def compiler_ids(objdump, elf):
    """Compiler identities recorded in the ELF .comment section (what really built it)."""
    out = subprocess.run([objdump, '-s', '-j', '.comment', str(elf)], capture_output=True, text=True, timeout=120).stdout
    text = ''.join(l[43:].rstrip('\n') for l in out.splitlines() if re.match(r'^ [0-9a-f]{4} ', l))
    return sorted(set(re.findall(r'GCC: \([^)]*\) [0-9.]+', text)))


def objdump_path(core):
    found = sorted(Path(core).glob('packages/toolchain-xtensa-esp32s3*/bin/xtensa-esp32s3-elf-objdump'))
    if not found: raise FileNotFoundError('xtensa-esp32s3 objdump not found in ' + str(core))
    return found[-1]


def flash_args(manifest, image_dir):
    f = manifest['flash']
    args = ['--chip', 'esp32s3', '--baud', str(f['baud']), '--before', 'default_reset', '--after', 'hard_reset',
            'write_flash', '-z', '--flash_mode', f['mode'], '--flash_freq', f['freq'], '--flash_size', f['size']]
    for name in FILES: args += [OFFSETS[name], str(Path(image_dir) / name)]
    return args


def collect(role, build, framework, core, commit, out, flash):
    out = Path(out); out.mkdir(parents=True, exist_ok=True, mode=0o700)
    sources = {n: Path(build) / n for n in FILES if n != 'boot_app0.bin'}
    sources['boot_app0.bin'] = Path(framework) / 'tools/partitions/boot_app0.bin'
    sources['firmware.elf'] = Path(build) / 'firmware.elf'
    files = {}
    for name, src in sources.items():
        dst = out / name; shutil.copyfile(src, dst); os.chmod(dst, 0o600)
        files[name] = {'size': dst.stat().st_size, 'sha256': sha256(dst)}
    objdump = objdump_path(core)
    m = {'role': role, 'sourceCommit': commit, 'env': 'l8_v3 (RMT, no DMA)', 'files': files,
         'framework': package_version(core, 'framework-arduinoespressif32'),
         'compiler': compiler_ids(objdump, out / 'firmware.elf'),
         'esptool': package_version(core, 'tool-esptoolpy'), 'core': str(core),
         'streamTimedReadCallsDelay': stream_yield(objdump, out / 'firmware.elf'), 'flash': flash}
    path = out / 'manifest.json'
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f: json.dump(m, f, indent=1)
    return m


def verify(manifest_path):
    """(ok, reasons, manifest)."""
    try: m = json.loads(Path(manifest_path).read_text())
    except (OSError, ValueError) as e: return False, [f'manifest unreadable: {e}'], None
    d = Path(manifest_path).parent; why = []
    for name in FILES:
        meta = m.get('files', {}).get(name)
        p = d / name
        if not meta or not p.exists(): why.append(f'{name} missing'); continue
        if p.stat().st_size != meta['size'] or sha256(p) != meta['sha256']: why.append(f'{name} hash/size mismatch')
    role = m.get('role')
    if role not in EXPECT_YIELD: why.append(f'unknown role {role!r}')
    elif m.get('streamTimedReadCallsDelay') is not EXPECT_YIELD[role]:
        why.append(f"{role} image: Stream::timedRead calls delay = {m.get('streamTimedReadCallsDelay')}, expected {EXPECT_YIELD[role]}")
    if not m.get('sourceCommit'): why.append('no source commit')
    return not why, why, m


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='mode', required=True)
    c = sub.add_parser('collect')
    for k in ('--role', '--build', '--framework', '--core', '--commit', '--out'): c.add_argument(k, required=True)
    c.add_argument('--flash-mode', default='dio'); c.add_argument('--flash-freq', default='80m')
    c.add_argument('--flash-size', default='16MB'); c.add_argument('--baud', type=int, default=460800)
    v = sub.add_parser('verify'); v.add_argument('manifest')
    a = ap.parse_args(argv)
    os.umask(0o077)
    if a.mode == 'collect':
        m = collect(a.role, a.build, a.framework, a.core, a.commit, a.out,
                    {'mode': a.flash_mode, 'freq': a.flash_freq, 'size': a.flash_size, 'baud': a.baud})
        print(json.dumps({k: m[k] for k in ('role', 'sourceCommit', 'framework', 'compiler', 'streamTimedReadCallsDelay')}))
        ok, why, _ = verify(Path(a.out) / 'manifest.json')
        print('verify', 'ok' if ok else why); return 0 if ok else 1
    ok, why, m = verify(a.manifest)
    print('verify', 'ok' if ok else why); return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
