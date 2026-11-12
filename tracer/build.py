"""Build tracer/gbtrace.exe: the runtime with no native code -- the
interpreter, the tracer (--trace DIR) and a plain emulator (--window).

    python -m gbkit.tracer.build
"""
import os
import shutil
import sys

from ..recomp import build as B

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    out = os.path.join(os.path.dirname(HERE), 'build', 'tracer')
    os.makedirs(out, exist_ok=True)
    exe = B.build(out, [os.path.join(B.RUNTIME, 'none.cpp')], opt='-O2')
    for name in ('gb.exe', 'SDL2.dll'):
        src = os.path.join(out, name)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(HERE, 'gbtrace.exe' if name == 'gb.exe' else name))
    print(f'  {os.path.join(HERE, "gbtrace.exe")}')
    return 0 if exe else 1


if __name__ == '__main__':
    sys.exit(main())
