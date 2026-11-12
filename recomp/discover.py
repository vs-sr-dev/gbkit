"""Code discovery for Game Boy cartridges.

    python -m gbkit.recomp.discover ROM.gb [--trace DIR] [--report] [--sym FILE.sym]

The SM83 sees the cartridge's ROM in two windows: $0000-$3FFF (bank 0, but
for an MBC1 in mode 1) and $4000-$7FFF (the bank the mapper selects).  What
an instruction at $4000-$7FFF is depends on the bank mapped when it runs --
the same address holds a different program in every bank -- so code is
gathered by region: one window with one ROM bank in it (tg16kit's rule, code
valid only where it is mapped).  A region is one C++ function in the end
(several, for a large one), entered by a switch on the address it is called
at: every instruction is emitted once, and every branch or jump inside the
region is a goto.  At run time the function is looked up by the window and
the bank mapped there now; a region's code checks after anything that may
have switched its bank (a write to the mapper, a call, an interrupt) that it
is still mapped, and leaves the rest to `gb_call` if not.

A region holds everything control reaches from its entries: both ways at a
conditional jump, JP and JR, the traced targets of a JP HL, the traced
places a call's caller went on at instead of after it (an RST whose
arguments follow it inline: its routine returns past them).  A walk stops
at RET and RETI, at undefined opcodes -- the sign of a walk into data -- and
at a transfer out of the window: where that goes depends on the bank at run
time, so it goes through `gb_call`, which finds the native code for what is
mapped then, or interprets.  An instruction that runs over the end of its
window stops the walk too.  Code in RAM (the OAM DMA routine in HRAM, the
odd routine in WRAM) stays interpreted.

Entries: the reset ($0100 in bank 0), and from a trace (tracer/gbtrace
output) every address that was the target of a jump, a call, an interrupt or
a computed return, for every bank the tracer ran its window with.  Native
code translates the bytes at an address whatever discovery thought of them,
so an entry in the wrong place costs code, never correctness.

Besides its entries, a region is registered at every jump target inside it
and every address a call returns to: the ways back in for the interpreter,
which carries on after native code has unwound and hands over at the first
of them it meets.

With --sym and --src (an RGBDS symbol file and the source tree of a
disassembly built locally -- pret's pokered, say), the instructions
discovered are checked against the labels, each code or data by the file
that defines it (data/, gfx/, text/, maps/, the music: data; home/,
engine/: code): an instruction that starts inside a data label is
reported, and the code labels discovery reached are counted.
"""
import argparse
import os
import re
import sys

from .. import cart as cartmod
from .. import sm83

R_OP, R_ARG, R_READ, R_JTGT, R_RET = 1, 2, 4, 8, 16
BANK = 0x4000


class Region:
    __slots__ = ('win', 'bank', 'entries', 'resumes', 'code', 'calls', 'indirect', 'alt_ret', 'problems', 'work')

    def __init__(self, win, bank):
        self.win, self.bank = win, bank
        self.entries = set()
        self.resumes = set()      # ways in for the interpreter too: jump targets, return addresses
        self.code = {}            # address -> Insn
        self.calls = set()        # targets of calls and RSTs
        self.indirect = {}        # address of a JP HL -> [traced targets in this region]
        self.alt_ret = {}         # address after a call -> [where its caller went on instead, in this region]
        self.problems = []
        self.work = []

    @property
    def key(self):
        return (self.win, self.bank)

    @property
    def name(self):
        return f'r{self.win}_{self.bank:03X}'

    def ways_in(self):
        return sorted(self.entries | self.resumes)

    def contains(self, a):
        return (a >> 14) == self.win


def _parse_loc(s):
    """'BBB:AAAA' -> (window, bank, address); 'RAM:AAAA' -> (None, None, address)."""
    m = re.match(r'(RAM|[0-9A-F]+):([0-9A-F]{4})', s)
    if not m:
        return None
    a = int(m.group(2), 16)
    if m.group(1) == 'RAM' or a >= 0x8000:
        return None, None, a
    return a >> 14, int(m.group(1), 16), a


def traces_for(build_dir, key):
    """The trace directories of a game: build/trace/KEY (the monkey's) and build/trace/KEY.* (recorded
    sessions, traced with --replay), those that hold a report."""
    import glob
    base = os.path.join(build_dir, 'trace', key)
    out = [d for d in [base] + sorted(glob.glob(base + '.*')) if os.path.exists(os.path.join(d, 'report.txt'))]
    return out


class Trace:
    """What gbtrace saw: per ROM byte flags, the banks each window ran code with, the indirect
    jumps, the computed returns, the transfers between the windows."""

    def __init__(self, paths, cart):
        """paths: one trace directory or several (the monkey's and recorded sessions'), merged."""
        self.cart = cart
        if isinstance(paths, str):
            paths = [paths]
        self.where = {}           # window -> {banks}
        self.ind = {}             # (win, bank, addr) of a JP HL -> {(win, bank, addr)}
        self.pushret = {}         # (win, bank, addr) of a RET no call pushed for -> {(win, bank, addr)}
        self.alt = {}             # (win, bank, addr) a call pushed -> {(win, bank, addr) its caller went on at}
        self.targets = set()      # every traced target in ROM
        cov = None
        for path in paths:
            c = open(os.path.join(path, 'cov.bin'), 'rb').read()
            cov = bytearray(c) if cov is None else bytearray(a | b for a, b in zip(cov, c))
            self._read(open(os.path.join(path, 'report.txt'), encoding='utf8', errors='replace').read())
        self.cov = bytes(cov or b'')

    def _read(self, rep):
        for w, b in re.findall(r'^where win (\d) bank ([0-9A-F]+)', rep, re.M):
            self.where.setdefault(int(w), set()).add(int(b, 16))
        for k, s, d in re.findall(r'^edge (\w) (\S+) -> (\S+) x\d+', rep, re.M):
            to = _parse_loc(d)
            if not to or to[0] is None:
                continue
            self.targets.add(to)
            fr = _parse_loc(s)
            if k == 'i' and fr and fr[0] is not None:
                self.ind.setdefault(fr, set()).add(to)
            if k in 'ra' and fr and fr[0] is not None:
                self.alt.setdefault(fr, set()).add(to)
            if k == 'p' and fr and fr[0] is not None:
                self.pushret.setdefault(fr, set()).add(to)

    def flag(self, off):
        return self.cov[off] if off < len(self.cov) else 0


class Program:
    def __init__(self, cart, trace=None, far=True, tables=True):
        self.cart, self.trace, self.far, self.tables = cart, trace, far, tables
        self.far_found = self.table_found = 0
        self.regions = {}
        self.data = cart.data
        self.banks = cart.banks

    def off(self, bank, a):
        return ((bank % self.banks) << 14) + (a & 0x3FFF)

    def reader(self, r):
        d = self.data

        def read(a):
            if (a >> 14) != r.win:
                raise IndexError(a)
            o = self.off(r.bank, a)
            return d[o] if o < len(d) else 0xFF
        return read

    def region(self, win, bank):
        k = (win, bank % self.banks)
        if k not in self.regions:
            self.regions[k] = Region(*k)
        return self.regions[k]

    def entry(self, win, bank, a):
        if win is None or a >= 0x8000:
            return
        r = self.region(win, bank)
        if a not in r.entries:
            r.entries.add(a)
            r.work.append(a)

    def _everywhere(self, a):
        """Entries at a for every bank the tracer ran its window with and ran an instruction at a."""
        if a >= 0x8000:
            return
        w = a >> 14
        if not self.trace:
            if w == 0:
                self.entry(0, 0, a)
            return
        for b in self.trace.where.get(w, ()):
            if self.trace.flag(self.off(b, a)) & R_OP:
                self.entry(w, b, a)

    def _goto(self, r, t):
        if r.contains(t):
            r.work.append(t)
            r.resumes.add(t)
        else:
            self._everywhere(t)

    def _on(self, r, nxt):
        """Control falls through to nxt: walked on, not a way in of its own."""
        if r.contains(nxt):
            r.work.append(nxt)
        else:
            self._everywhere(nxt)

    def _site(self, r, a):
        return (r.win, r.bank, a)

    def _step(self, r):
        a = r.work.pop()
        if a in r.code or not r.contains(a):
            return
        try:
            i = sm83.decode(self.reader(r), a)
        except IndexError:
            r.problems.append(a)
            return
        if i.kind == 'bad':
            r.problems.append(a)
            return
        if (a + i.length - 1) >> 14 != r.win:         # runs over the end of the window
            r.problems.append(a)
            return
        r.code[a] = i
        nxt, t, k, tr = i.next, i.target, i.kind, self.trace
        if k in ('jump', 'branch'):
            self._goto(r, t)
            if k == 'branch':
                self._on(r, nxt)
        elif k in ('call', 'ccall', 'rst'):
            r.calls.add(t)
            if r.contains(t):
                self.entry(r.win, r.bank, t)
            else:
                self._everywhere(t)
            self._goto(r, nxt)                  # where the call returns: the interpreter comes back there
            for w, b, x in (tr.alt.get(self._site(r, nxt), ()) if tr else ()):
                if (w, b % self.banks) == r.key:
                    r.alt_ret.setdefault(nxt, set()).add(x)
                    self._goto(r, x)
                else:
                    self.entry(w, b, x)
        elif k == 'jphl':
            seen = tr.ind.get(self._site(r, a), set()) if tr else set()
            r.indirect[a] = sorted(x for w, b, x in seen if (w, b % self.banks) == r.key)
            for w, b, x in seen:
                if (w, b % self.banks) == r.key:
                    self._goto(r, x)
                else:
                    self.entry(w, b, x)
        elif k in ('ret', 'reti'):
            pass
        else:                                   # falls through (a conditional return too)
            if k in ('halt', 'stop'):
                r.resumes.add(nxt)
            self._on(r, nxt)

    def discover(self):
        self.entry(0, 0, 0x0100)
        if self.trace:
            for w, b, a in self.trace.targets:
                self.entry(w, b, a)
            # every traced jump target and return address, in each bank its window ran with
            for o, fl in enumerate(self.trace.cov):
                if fl & (R_JTGT | R_RET) and fl & R_OP:
                    b, lo = o >> 14, o & 0x3FFF
                    for w in (0, 1):
                        if b in self.trace.where.get(w, ()):
                            self.entry(w, b, (w << 14) | lo)
        self._walk()
        if self.tables and self.trace:                # the rest of the pointer tables the trace saw entries of
            from . import tables
            sites = dict(self.trace.ind)
            for k, v in self.trace.pushret.items():
                sites.setdefault(k, set()).update(v)
            new = tables.word_tables(self, sites) | tables.bank_tables(self, self.trace.targets)
            for w, b, a in new:
                before = len(self.region(w, b).entries)
                self.entry(w, b, a)
                self.table_found += len(self.region(w, b).entries) - before
            self._walk()
        if self.far:                                  # calls into a bank, by the constants at the call
            from .farcalls import Resolver
            res, done = Resolver(self), set()
            while True:
                new = []
                for r in list(self.regions.values()):
                    for e in res.resolve(r):
                        if e not in done:
                            done.add(e)
                            new.append(e)
                if not new:
                    break
                for w, b, a in new:
                    if b % self.banks or w == 0:
                        before = len(self.region(w, b).entries)
                        self.entry(w, b, a)
                        self.far_found += len(self.region(w, b).entries) - before
                self._walk()
        for r in self.regions.values():               # entries on data: left to the interpreter
            r.entries = {a for a in r.entries if a in r.code}
            r.resumes = {a for a in r.resumes if a in r.code}
        self.regions = {k: r for k, r in self.regions.items() if r.entries}
        return self

    def _walk(self):
        busy = True
        while busy:
            busy = False
            for r in list(self.regions.values()):
                while r.work:
                    busy = True
                    self._step(r)

    def coverage(self):
        """Executed instruction starts (per the trace) that some region holds."""
        if not self.trace:
            return None
        held = set()
        for r in self.regions.values():
            for a in r.code:
                held.add(self.off(r.bank, a))
        ran = [o for o, x in enumerate(self.trace.cov) if x & R_OP]
        return sum(1 for o in ran if o in held), len(ran)

    # in a pret-style disassembly, where a label is defined says what it is
    DATA_DIRS = ('data/', 'gfx/', 'text/', 'audio/music/', 'audio/sfx/', 'audio/headers/', 'maps/', 'text.asm')
    CODE_DIRS = ('home/', 'engine/', 'home.asm', 'audio/engine')

    def check_sym(self, path, src=None):
        """The instructions discovered against an RGBDS .sym and the disassembly's source (src):
        a label is data or code by the file that defines it.  Returns (instructions starting inside
        a data label's span, [(code label, reached)] for the code labels in ROM)."""
        syms = []
        for line in open(path, encoding='utf8', errors='replace'):
            m = re.match(r'([0-9a-fA-F]{2,3}):([0-9a-fA-F]{4}) (\S+)', line)
            if m and '.' not in m.group(3):
                syms.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
        syms.sort()
        where = {}
        if src:
            for d, _, fs in os.walk(src):
                for f in fs:
                    if not f.endswith('.asm'):
                        continue
                    p = os.path.join(d, f)
                    rel = os.path.relpath(p, src).replace('\\', '/')
                    for line in open(p, encoding='utf8', errors='replace'):
                        m = re.match(r'([A-Za-z_][\w@#]*)::?', line)
                        if m:
                            where.setdefault(m.group(1), rel)
        held = {}
        for r in self.regions.values():
            for a, i in r.code.items():
                held[self.off(r.bank, a)] = i
        bad, code = [], []
        for n, (b, a, name) in enumerate(syms):
            if (b == 0 and a >= 0x4000) or a >= 0x8000 or (b and a < 0x4000):
                continue
            f = where.get(name, '')
            k = n + 1                           # the span: up to the next label at a higher address
            while k < len(syms) and syms[k][0] == b and syms[k][1] <= a:
                k += 1
            end = syms[k][1] if k < len(syms) and syms[k][0] == b else (a | 0x3FFF) + 1
            if f.startswith(self.DATA_DIRS) and not name.endswith('End'):    # an ...End label marks where data stops
                for x in range(a, min(end, (a | 0x3FFF) + 1)):
                    o = self.off(b, x)
                    if o in held:
                        bad.append((b, x, name, held[o], f))
                        break
            elif f.startswith(self.CODE_DIRS):
                code.append((b, a, name, self.off(b, a) in held, f))
        return bad, code


def main(argv=None):
    ap = argparse.ArgumentParser(prog='gbkit.recomp.discover')
    ap.add_argument('rom')
    ap.add_argument('--trace', action='append', help='a trace directory (more than one: merged)')
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--sym', help="an RGBDS .sym of the same ROM (a disassembly's, built locally)")
    ap.add_argument('--src', help="the disassembly's source tree: where a label is defined says code or data")
    ap.add_argument('--missed', action='store_true', help='list the code labels discovery did not reach')
    ap.add_argument('--no-far', action='store_true', help='no static resolution of calls into a bank')
    ap.add_argument('--no-tables', action='store_true', help='no completion of the traced pointer tables')
    a = ap.parse_args(argv)
    c = cartmod.Cart.load(a.rom)
    tr = Trace(a.trace, c) if a.trace else None
    p = Program(c, tr, far=not a.no_far, tables=not a.no_tables).discover()
    insns = sum(len(r.code) for r in p.regions.values())
    ents = sum(len(r.entries) for r in p.regions.values())
    print(f'{c.key}: {len(p.regions)} regions, {ents} entries, {insns} instructions'
          + f' ({p.far_found} entries from calls into a bank, {p.table_found} from pointer tables)')
    cov = p.coverage()
    if cov:
        print(f'  traced instructions held by a region: {cov[0]}/{cov[1]}')
    probs = sum(len(r.problems) for r in p.regions.values())
    ind = sum(len(r.indirect) for r in p.regions.values())
    alt = sum(len(r.alt_ret) for r in p.regions.values())
    print(f'  JP HL sites {ind}, calls going on elsewhere {alt}, walks into data or off the window {probs}')
    if a.report:
        for k in sorted(p.regions):
            r = p.regions[k]
            print(f'  {r.name:10} {len(r.code):6} insns  {len(r.entries):4} entries  {len(r.calls):4} calls'
                  + (f'  stops at {",".join("%04X" % x for x in r.problems[:6])}' if r.problems else ''))
    if a.sym:
        bad, code = p.check_sym(a.sym, a.src)
        print(f'  instructions inside data labels: {len(bad)}')
        for b, x, name, i, f in bad[:40]:
            print(f'    {b:02X}:{x:04X} {name:32} {i.text():24} ({f})')
        if code:
            got = sum(1 for c in code if c[3])
            print(f'  code labels reached: {got}/{len(code)}')
            if a.missed:
                for b, x, name, ok, f in code:
                    if not ok:
                        print(f'    not reached {b:02X}:{x:04X} {name} ({f})')


if __name__ == '__main__':
    sys.exit(main())
