"""Play a game, recompiled: find it by name, build it if needed, open a window.

    python -m gbkit.play "tetris"              # the first match, Game Boy before Color
    python -m gbkit.play "link's awakening dx" --scale 4
    python -m gbkit.play "azure dreams" --interp   # the interpreter instead
    python -m gbkit.play "pokemon red" --record pad.bin

Keys: arrows, X = A, Z = B, Enter = Start, Backspace = Select, F12 a
screenshot, Esc quits.
"""
import argparse
import os
import subprocess
import sys

from . import cart as cartmod
from .recomp import __main__ as recomp
from .tracer.survey import roms_in, trace

ROOT = os.path.dirname(os.path.abspath(__file__))
TRACER = os.path.join(ROOT, 'tracer', 'gbtrace.exe')


def main(argv=None):
    ap = argparse.ArgumentParser(prog='gbkit.play')
    ap.add_argument('name')
    ap.add_argument('--interp', action='store_true')
    ap.add_argument('--scale', default='3')
    ap.add_argument('--record')
    ap.add_argument('--romdir', default=os.path.join(ROOT, 'Game Boy'))
    a = ap.parse_args(argv)
    want = a.name.lower()
    hits = []
    for r in roms_in(a.romdir):
        c = cartmod.Cart.load(r)
        if want in os.path.basename(r).lower() or want in c.title.lower() or want == (c.short or ''):
            hits.append((c.list != 'gameboy', '(' in os.path.basename(r), r, c))
    if not hits:
        print(f'no ROM matches {a.name!r}')
        return 1
    hits.sort(key=lambda h: h[:3])
    rom, c = hits[0][2], hits[0][3]
    flags = [*cartmod.run_flags(c), '--window', '--scale', a.scale] + (['--record', a.record] if a.record else [])
    print(f'{os.path.basename(rom)}  ({c.key}: {c.title}, {c.model_class}, {c.mapper})')
    if a.interp:
        return subprocess.run([TRACER, rom, *flags]).returncode
    out = os.path.join(ROOT, 'build', 'recomp', c.key)
    exe = os.path.join(out, 'gb.exe')
    if not os.path.exists(exe):
        tdir = os.path.join(ROOT, 'build', 'trace', c.key)
        if not os.path.exists(os.path.join(tdir, 'report.txt')):
            print('  tracing...')
            trace(rom, tdir, 6000)
        print('  recompiling...')
        files, _ = recomp.generate(rom, tdir, out)
        recomp.build(out, files)
    return subprocess.run([exe, os.path.abspath(rom), *flags], cwd=out).returncode


if __name__ == '__main__':
    sys.exit(main())
