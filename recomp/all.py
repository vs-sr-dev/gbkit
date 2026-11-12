"""Every cartridge through the whole route: trace, recompile, build, lockstep.

    python -m gbkit.recomp.all ROMDIR [--frames 6000] [--only NAME] [--jobs N] [--resume | --native]

For each ROM: tracer/gbtrace (6000 frames of the monkey pad) unless its
trace is there (recorded sessions traced to build/trace/KEY.* are merged
in), `recomp` to build/recomp/KEY, g++, then `gb.exe --lockstep`
-- the interpreter and the native code side by side, the machine compared
at every frame -- and one line of results.  A cartridge whose mapper the
runtime does not cover is reported as such.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

from .. import cart as cartmod
from ..tracer.survey import roms_in, trace
from . import __main__ as recomp
from . import discover

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def one(rom, frames, build_dir, resume=False, native=False):
    c = cartmod.Cart.load(rom)
    k = c.key
    out = os.path.join(build_dir, 'recomp', k)
    done = os.path.join(out, 'result.txt')
    if resume and os.path.exists(done):
        res = open(done, encoding='utf8', errors='replace').read()
        if f'lockstep: identical for {frames} frames' in res and (not native or 'interpreted insns 0 ' in res):
            return k, c, res, []
    tdir = os.path.join(build_dir, 'trace', k)
    if not os.path.exists(os.path.join(tdir, 'report.txt')):
        trace(rom, tdir, frames)
    logs = []
    try:
        files, _ = recomp.generate(rom, discover.traces_for(build_dir, k), out, log=logs.append)
        recomp.build(out, files, log=logs.append)
    except Exception as e:                       # noqa: BLE001 -- reported per game
        return k, c, f'BUILD FAILED: {str(e)[:300]}', logs
    r = subprocess.run([os.path.join(out, 'gb.exe'), os.path.abspath(rom), '--lockstep',
                        '--frames', str(frames), '--out', out, *cartmod.run_flags(c)],
                       capture_output=True, text=True, cwd=out, errors='replace')
    res = (r.stdout + r.stderr).strip()
    with open(done, 'w', encoding='utf8') as f:
        f.write(res + '\n')
    return k, c, res, logs


def main(argv=None):
    ap = argparse.ArgumentParser(prog='gbkit.recomp.all')
    ap.add_argument('romdir')
    ap.add_argument('--frames', type=int, default=6000)
    ap.add_argument('--only')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--build-dir', default=os.path.join(ROOT, 'build'))
    ap.add_argument('--resume', action='store_true', help='skip the games already identical')
    ap.add_argument('--native', action='store_true', help='skip the games already identical without the interpreter')
    a = ap.parse_args(argv)
    roms = roms_in(a.romdir)
    if a.only:
        roms = [r for r in roms if a.only.lower() in r.lower()]
    ok = native = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for k, c, res, logs in ex.map(lambda r: one(r, a.frames, a.build_dir, a.resume or a.native, a.native), roms):
            same = f'lockstep: identical for {a.frames} frames' in res
            ok += same
            m = re.search(r'interpreted insns (\d+)\s+native calls (\d+)\s+fallbacks (\d+)\s+unwinds (\d+)\s+max depth (\d+)', res)
            speed = re.search(r'\((\d+)x\)', res)
            if m and same and int(m.group(1)) == 0:
                native += 1
            info = (f'interp {int(m.group(1)):>9}  fallbacks {m.group(3):>6}  unwinds {m.group(4):>5}  '
                    f'depth {m.group(5):>3}  {speed.group(1) if speed else "?":>4}x') if m else ''
            print(f'{"OK  " if same else "FAIL"} {k:24} {info}', flush=True)
            if not same:
                for line in (res.splitlines() + logs)[:10]:
                    print('       ' + line)
    print(f'{ok}/{len(roms)} identical in lockstep, {native} without the interpreter')
    return 0 if ok == len(roms) else 1


if __name__ == '__main__':
    sys.exit(main())
