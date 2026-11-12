"""Play a game, recompiled: find it by name, build it if needed, open a window.

    python -m gbkit.play "tetris"              # the first match, Game Boy before Color
    python -m gbkit.play "link's awakening dx" --scale 4
    python -m gbkit.play "azure dreams" --interp   # the interpreter instead
    python -m gbkit.play "pokemon red" --record red1.bin   # and the session traced for discovery

A recorded session is replayed by the tracer into build/trace/KEY.<name>
(red1 here) once the window closes; the next recompilation merges it with
the monkey's trace, so the code that session reached runs native too.

Keys: arrows, X = A, Z = B, Enter = Start, Backspace = Select, F12 a
screenshot, Esc quits.
"""
import argparse
import os
import subprocess
import sys

from . import cart as cartmod
from .recomp import __main__ as recomp
from .recomp import discover
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
    flags = [*cartmod.run_flags(c), '--window', '--scale', a.scale] + (['--record', os.path.abspath(a.record)] if a.record else [])
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
        files, _ = recomp.generate(rom, discover.traces_for(os.path.join(ROOT, 'build'), c.key), out)
        recomp.build(out, files)
    rc = subprocess.run([exe, os.path.abspath(rom), *flags], cwd=out).returncode
    if a.record and os.path.exists(os.path.abspath(a.record)):
        name = os.path.splitext(os.path.basename(a.record))[0]
        tdir = os.path.join(ROOT, 'build', 'trace', f'{c.key}.{name}')
        os.makedirs(tdir, exist_ok=True)
        print(f'  tracing the session into {tdir}...')
        subprocess.run([TRACER, rom, '--trace', tdir, '--out', tdir, '--quiet', '--replay', os.path.abspath(a.record),
                        *cartmod.run_flags(c)])
        os.remove(exe)                   # the next play recompiles with it
    return rc


if __name__ == '__main__':
    sys.exit(main())
