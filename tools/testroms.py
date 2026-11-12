"""The interpreter's regression battery: the public test ROMs, each read the
way it reports.

    python tools/testroms.py TESTDIR [--exe tracer/gbtrace.exe] [--jobs 14] [--only NAME]

TESTDIR holds the suites as they are downloaded (not distributed with gbkit):

    blargg/gb-test-roms-master/   blargg's: the verdict on the serial port ("Passed"),
                                  at $A000 (0 after the signature DE B0 61 at $A001),
                                  or on the screen alone (halt_bug, interrupt_time: the
                                  text is in the BG map, a tile per ASCII code)
    mooneye/*/                    Gekkio's mooneye test suite: registers B C D E H L =
                                  3 5 8 13 21 34 for a pass; the model from the name's
                                  suffix (C: a CGB; G, dmgABC or none: a DMG).  A test for a
                                  CGB in a cartridge with no CGB flag is for the CGB's
                                  compatibility mode, which gbkit does not run (a DMG game
                                  runs as a DMG): skipped and counted apart
    dmg-acid2.gb, cgb-acid2.gbc   Matt Currie's: the picture against the reference
                                  (dmg-acid2-ref.png, cgb-acid2-ref.png)
"""
import argparse
import concurrent.futures
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FIB = '03 05 08 0D 15 22'


def mooneye_model(name):
    """'dmg' / 'cgb' / None (a model gbkit does not run) from a mooneye ROM's suffix."""
    m = re.search(r'-([A-Za-z0-9]+)$', os.path.splitext(name)[0])
    if not m:
        return 'dmg'
    s = m.group(1)
    if s in ('dmgABC', 'dmgABCmgb') or 'G' in s:
        return 'dmg'
    if 'C' in s and s != 'dmgABC':
        return 'cgb'
    return None


def tests(root):
    out = []
    b = os.path.join(root, 'blargg', 'gb-test-roms-master')
    for p in sorted(glob.glob(os.path.join(b, '**', '*.gb'), recursive=True)):
        rel = os.path.relpath(p, b).replace('\\', '/')
        model = 'cgb' if rel.startswith(('cgb_sound', 'interrupt_time')) else 'dmg'
        out.append(('blargg/' + rel, p, model, 'blargg', 6000))
    for p in sorted(glob.glob(os.path.join(root, 'mooneye', '*', '**', '*.gb'), recursive=True)):
        rel = os.path.relpath(p, os.path.join(root, 'mooneye')).replace('\\', '/').split('/', 1)[1]
        if rel.startswith(('manual-only', 'utils', 'madness')):
            continue
        model = mooneye_model(os.path.basename(p))
        if model == 'cgb' and not open(p, 'rb').read()[0x143] & 0x80:
            model = 'compat'
        if model:
            out.append(('mooneye/' + rel, p, model, 'mooneye', 600))
    for n, model in (('dmg-acid2.gb', 'dmg'), ('cgb-acid2.gbc', 'cgb')):
        p = os.path.join(root, n)
        if os.path.exists(p):
            out.append((n, p, model, 'acid2', 120))
    return out


def read_ppm(path):
    d = open(path, 'rb').read()
    parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3][:w * h * 3]


def acid_verdict(ppm, ref):
    from PIL import Image
    w, h, px = read_ppm(ppm)
    im = Image.open(ref).convert('RGB')
    if im.size != (w, h):
        return False, 'size'
    rp = im.tobytes()
    if 'dmg' in os.path.basename(ref):
        # four greys: compare the shade's rank
        def ranks(b):
            lv = sorted(set(b[i] for i in range(0, len(b), 3)))
            return [lv.index(b[i]) + (4 - len(lv)) if len(lv) < 4 else lv.index(b[i]) for i in range(0, len(b), 3)]
        bad = sum(1 for x, y in zip(ranks(px), ranks(rp)) if x != y)
    else:
        bad = sum(1 for i in range(0, len(px), 3) if any((px[i + k] >> 3) != (rp[i + k] >> 3) for k in range(3)))
    return bad == 0, f'{bad} pixels differ' if bad else ''


def run(exe, t, root):
    name, path, model, kind, frames = t
    with tempfile.TemporaryDirectory() as d:
        cmd = [exe, path, '--interp', '--frames', str(frames), '--quiet', '--out', d, f'--{model}']
        if kind == 'blargg':
            cmd += ['--serial', '--peek', 'A000:4', '--peek', '9800:576']
        if kind == 'mooneye':
            cmd += ['--regs']
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=300, errors='replace')
        except subprocess.TimeoutExpired:
            return name, model, False, 'timeout'
        o = r.stdout
        if kind == 'blargg':
            ser = re.search(r'serial: (.*)', o, re.S)
            ser = ser.group(1) if ser else ''
            pk = re.search(r'peek A000: (\w\w) (\w\w) (\w\w) (\w\w)', o)
            if 'Passed' in ser:
                return name, model, True, ''
            if pk and pk.group(2, 3, 4) == ('DE', 'B0', '61'):
                code = int(pk.group(1), 16)
                return name, model, code == 0, '' if code == 0 else f'result {code:02X}'
            bg = re.search(r'peek 9800: ((?:\w\w ?){576})', o)
            if bg:
                b = bytes(int(x, 16) for x in bg.group(1).split())
                text = ' '.join(''.join(chr(c) if 32 <= c < 127 else ' ' for c in b[r * 32:r * 32 + 20]).strip()
                                for r in range(18))
                if 'Passed' in text:
                    return name, model, True, ''
                if 'Failed' in text:
                    return name, model, False, 'screen: ' + ' '.join(text.split())[-60:]
            return name, model, False, ('serial: ' + ' '.join(ser.split())[-60:]) if ser.strip() else 'no verdict'
        if kind == 'mooneye':
            m = re.search(r'regs A=\w\w F=\w\w B=(\w\w) C=(\w\w) D=(\w\w) E=(\w\w) H=(\w\w) L=(\w\w)', o)
            regs = ' '.join(m.groups()) if m else ''
            return name, model, regs == FIB, '' if regs == FIB else (f'regs {regs}' if regs else r.stderr[-80:])
        ref = os.path.join(root, name.split('.')[0] + '-ref.png')
        ok, why = acid_verdict(os.path.join(d, 'last.ppm'), ref)
        return name, model, ok, why


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('root')
    ap.add_argument('--exe', default=os.path.join(os.path.dirname(HERE), 'tracer', 'gbtrace.exe'))
    ap.add_argument('--jobs', type=int, default=14)
    ap.add_argument('--only')
    ap.add_argument('--failed', action='store_true', help='list only the failures')
    a = ap.parse_args(argv)
    ts = [t for t in tests(a.root) if not a.only or a.only in t[0]]
    compat = [t for t in ts if t[2] == 'compat']
    ts = [t for t in ts if t[2] != 'compat']
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        res = list(ex.map(lambda t: run(a.exe, t, a.root), ts))
    per = {}
    for name, model, ok, why in res:
        suite = name.split('/')[0] if '/' in name else 'acid2'
        p = per.setdefault(suite, [0, 0])
        p[0] += ok; p[1] += 1
        if not ok or not a.failed:
            print(f'{"pass" if ok else "FAIL"}  {model}  {name}  {why}')
    for t in compat:
        if not a.failed:
            print(f'skip  cgb  {t[0]}  a DMG cartridge on a CGB (compatibility mode)')
    print('  '.join(f'{k} {v[0]}/{v[1]}' for k, v in sorted(per.items()))
          + (f"  ({len(compat)} for the CGB's compatibility mode skipped)" if compat else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
