"""Text the developers left where the game never looks: ASCII runs in the
bytes a trace neither ran nor read (cov.bin), per game.

    python tools/strings.py build/trace "Game Boy" [--min 16] [--only KEY]

Repetitive filler (a run of one character, a short pattern repeated) is skipped;
what stays is printed with the game's key and the ROM offset.
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from gbkit import cart as cartmod  # noqa: E402

RUN = re.compile(rb'[\x20-\x7e]{%d,}')


def boring(s):
    t = s.strip()
    if len(set(t)) < 6:
        return True
    for n in range(1, 9):                       # a short pattern repeated
        if len(t) >= 3 * n and t == (t[:n] * (len(t) // n + 1))[:len(t)]:
            return True
    letters = sum(ch.isalpha() or ch == ' ' for ch in t)
    if letters < len(t) * 0.8:
        return True
    words = [w for w in re.split(r'[^A-Za-z]+', t) if len(w) >= 3]
    # words that read as words: a vowel, not all one case-flip soup
    real = [w for w in words if re.search(r'[aeiouAEIOU]', w) and (w.isupper() or w.islower() or w.istitle())]
    return len(real) < 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tracedir')
    ap.add_argument('romdir')
    ap.add_argument('--min', type=int, default=16)
    ap.add_argument('--only')
    a = ap.parse_args()
    roms = {}
    for p in glob.glob(os.path.join(a.romdir, '**', '*'), recursive=True):
        if not p.lower().endswith(cartmod.EXTS):
            continue
        c = cartmod.Cart.load(p)
        roms[c.key] = c
    rx = re.compile(rb'[\x20-\x7e]{%d,}' % a.min)
    for key in sorted(roms):
        if a.only and a.only not in key:
            continue
        covp = os.path.join(a.tracedir, key, 'cov.bin')
        if not os.path.exists(covp):
            continue
        cov = open(covp, 'rb').read()
        data = roms[key].data
        for m in rx.finditer(data):
            if m.start() < 0x150:                 # the header
                continue
            if any(cov[o] for o in range(m.start(), m.end()) if o < len(cov)):
                continue
            s = m.group().decode('ascii')
            if not boring(s):
                print(f'{key:24} {m.start():07X}  {s}')


if __name__ == '__main__':
    main()
