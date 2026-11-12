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
window stops the walk too.

Code in RAM -- the OAM DMA routine nearly every game copies to HRAM, the
routines some copy to WRAM -- is one more region, `rram`, made of the bytes
the trace saw each instruction there run with: the instructions that ran
with one version of their bytes, and those whose versions differ in their
operands only (code that patches its own constants and jump targets as it
runs: the operands are read from RAM when the instruction runs, and a
jump's or call's targets are the ones the trace saw it take).  Code whose
opcodes change stays interpreted.  The rest is valid while RAM holds the
bytes it was compiled from: the runtime checks them before it enters (and
at the slow polls, and after a write that may reach them), and interprets
otherwise.

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
RAM = 2                   # the window of the RAM region


def ram_ok(a):
    """Where compiled RAM code may be: WRAM ($C000-$DFFF) and HRAM ($FF80-$FFFE)."""
    return 0xC000 <= a < 0xE000 or 0xFF80 <= a < 0xFFFF


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

    def holds(self, a, n):
        """The n bytes from a are all this region's to read."""
        return (a + n - 1) >> 14 == self.win


class RamRegion(Region):
    """The code in RAM, as the bytes it ran with: valid while RAM holds them (the operands that changed
    as it ran excepted: read when the instruction runs)."""
    __slots__ = ('image', 'var', 'varops')

    def __init__(self, image, var=()):
        super().__init__(RAM, 0)
        self.image = image        # address -> byte (the opcodes, and the operands that never changed)
        self.var = set(var)       # the operand bytes that changed
        self.varops = {}          # address of an instruction -> [offsets of its operand bytes that change]

    @property
    def name(self):
        return 'rram'

    def contains(self, a):
        return a in self.image

    def holds(self, a, n):
        return a + n - 1 <= 0xFFFF and a in self.image and all(
            (a + k) in self.image or (a + k) in self.var for k in range(1, n))

    def components(self):
        """The routines in RAM: the instructions joined by falling through, jumping or calling, as
        {address: n} (n from 0, by the lowest address).  Each holds while its own bytes do: the OAM DMA
        routine copied at the reset is not held back by a routine copied next to it later."""
        up = {a: a for a in self.code}

        def find(a):
            while up[a] != a:
                up[a] = up[up[a]]
                a = up[a]
            return a

        def join(a, b):
            if b in up:
                up[find(a)] = find(b)
        for a, i in self.code.items():
            if not i.ends:
                join(a, i.next)
            if i.target is not None:
                join(a, i.target)
            for t in self.indirect.get(a, ()):
                join(a, t)
            for t in self.alt_ret.get(i.next, ()):
                join(a, t)
        roots = {}
        for a in sorted(self.code):
            roots.setdefault(find(a), len(roots))
        return {a: roots[find(a)] for a in self.code}

    def runs(self, addrs=None):
        """The bytes the code (or the instructions at addrs) reads as its own: [(start, bytes)] in address
        order, a run within a 4 KB page."""
        code = self.code if addrs is None else {a: self.code[a] for a in addrs}
        used = sorted({a + k for a, i in code.items() for k in range(i.length) if (a + k) in self.image})
        out = []
        for a in used:
            if out and out[-1][0] + len(out[-1][1]) == a and (a & 0xF000) == (out[-1][0] & 0xF000):
                out[-1][1].append(self.image[a])
            else:
                out.append((a, [self.image[a]]))
        return [(a, bytes(b)) for a, b in out]


def _parse_loc(s):
    """'BBB:AAAA' -> (window, bank, address); 'RAM:AAAA' -> (RAM, 0, address)."""
    m = re.match(r'(RAM|[0-9A-F]+):([0-9A-F]{4})', s)
    if not m:
        return None
    a = int(m.group(2), 16)
    if m.group(1) == 'RAM' or a >= 0x8000:
        return RAM, 0, a
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
        self.ram_targets = set()  # ... and in RAM
        self.ramcode = {}         # address in RAM -> {the bytes of the instruction run there} ('*...': too many)
        self.ram_edges = {}       # address in RAM of a jump or call -> {(win, bank, addr) it went to}
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
            if not to:
                continue
            if to[0] == RAM:
                self.ram_targets.add(to[2])
            else:
                self.targets.add(to)
            fr = _parse_loc(s)
            if not fr:
                continue
            if k == 'i':
                self.ind.setdefault(fr, set()).add(to)
            if k in 'ra':
                self.alt.setdefault(fr, set()).add(to)
            if k == 'p' and fr[0] != RAM and to[0] != RAM:
                self.pushret.setdefault(fr, set()).add(to)
            if k in 'jc' and fr[0] == RAM:
                self.ram_edges.setdefault(fr[2], set()).add(to)
        for a, vs in re.findall(r'^ramcode ([0-9A-F]{4}) (.*)$', rep, re.M):
            have = self.ramcode.setdefault(int(a, 16), set())
            for v in vs.split():
                have.add(v if v.startswith('*') else bytes.fromhex(v))    # '*OP': too many, the opcode OP

    def ram_image(self):
        """The code in RAM where compiled code may be (WRAM, HRAM), as ({address: byte}, {address}): the
        bytes of the instructions that ran with one version of them, the opcodes of those whose versions
        differ in the operands only -- and those operand bytes, which change.  A byte two instructions
        disagree on is left out with both."""
        claims = {}                                   # address -> {byte, or None: an operand that changes}
        insns = []
        for a, vs in self.ramcode.items():
            ops = set()
            for v in vs:
                if isinstance(v, str):
                    ops.add(bytes.fromhex(v[1:]) if len(v) > 1 else None)
                else:
                    ops.add(v[:2] if v[0] == 0xCB else v[:1])
            if len(ops) != 1 or None in ops:
                continue                              # the opcode changes: left to the interpreter
            (opc,) = ops
            n = sm83.LENGTH[opc[0]]
            if len(vs) == 1 and not isinstance(next(iter(vs)), str):
                cl = list(next(iter(vs)))             # one version: every byte
            else:
                cl = list(opc) + [None] * (n - len(opc))
            if a + n > 0xFFFF or not all(ram_ok(a + k) for k in range(n)):
                continue
            insns.append((a, cl))
            for k, x in enumerate(cl):
                claims.setdefault(a + k, set()).add(x)
        img, var = {}, set()
        for a, cl in insns:
            if any(len(claims[a + k]) != 1 for k in range(len(cl))):
                continue
            for k, x in enumerate(cl):
                if x is None:
                    var.add(a + k)
                else:
                    img[a + k] = x
        return img, var

    def flag(self, off):
        return self.cov[off] if off < len(self.cov) else 0


class Program:
    def __init__(self, cart, trace=None, far=True, tables=True, ram=True):
        self.cart, self.trace, self.far, self.tables = cart, trace, far, tables
        self.ram = ram and trace is not None          # the RAM region: from the bytes a trace saw run
        self.origin = {}          # (win, bank, addr) of an entry -> how it was found (reset trace walk table far)
        self.phase = 'reset'
        self.table_spans = []     # the pointer tables completed: (kind, ROM offset, entries, stride)
        self.far_found = self.table_found = 0
        self.regions = {}
        self.data = cart.data
        self.banks = cart.banks

    def off(self, bank, a):
        return ((bank % self.banks) << 14) + (a & 0x3FFF)

    def reader(self, r):
        d = self.data
        if r.win == RAM:
            def read_ram(a):
                if a in r.image:
                    return r.image[a]
                if a in r.var:
                    return 0                          # an operand that changes: read at run time
                raise IndexError(a)
            return read_ram

        def read(a):
            if (a >> 14) != r.win:
                raise IndexError(a)
            o = self.off(r.bank, a)
            return d[o] if o < len(d) else 0xFF
        return read

    def region(self, win, bank):
        k = (win, bank % self.banks if win != RAM else 0)
        if k not in self.regions:
            self.regions[k] = Region(*k) if win != RAM else RamRegion(*self.trace.ram_image())
        return self.regions[k]

    def entry(self, win, bank, a):
        if win is None or (win != RAM and a >= 0x8000) or (win == RAM and not self.ram):
            return
        r = self.region(win, bank)
        if a not in r.entries:
            r.entries.add(a)
            r.work.append(a)
            self.origin.setdefault((r.win, r.bank, a), self.phase)

    def _everywhere(self, a):
        """Entries at a for every bank the tracer ran its window with and ran an instruction at a
        (in RAM: the RAM region's, if it ran there)."""
        if a >= 0x8000:
            if self.trace and a in self.trace.ram_targets:
                self.entry(RAM, 0, a)
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
        if not r.holds(a, i.length):                 # runs over the end of the window (or of the RAM code)
            r.problems.append(a)
            return
        r.code[a] = i
        nxt, t, k, tr = i.next, i.target, i.kind, self.trace
        var = [n for n in range(1, i.length) if (a + n) in getattr(r, 'var', ())]
        if var:
            r.varops[a] = var
        if var and k in ('jump', 'branch', 'call', 'ccall'):
            # the target is an operand that changes: the targets the trace saw, a switch at run time
            seen = tr.ram_edges.get(a, set()) if tr else set()
            r.indirect[a] = sorted(x for w, b, x in seen if w == RAM and r.contains(x))
            for w, b, x in seen:
                if w == RAM and k in ('jump', 'branch'):
                    self._goto(r, x)
                elif w == RAM:
                    r.calls.add(x)
                    self.entry(RAM, 0, x)
                else:
                    self.entry(w, b, x)
            if k == 'branch':
                self._on(r, nxt)
            elif k in ('call', 'ccall'):
                self._goto(r, nxt)
                for w, b, x in (tr.alt.get(self._site(r, nxt), ()) if tr else ()):
                    if (w, b) == r.key:
                        r.alt_ret.setdefault(nxt, set()).add(x)
                        self._goto(r, x)
                    else:
                        self.entry(w, b, x)
            return
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
        self.phase = 'trace'
        if self.trace:
            for w, b, a in self.trace.targets:
                self.entry(w, b, a)
            for a in self.trace.ram_targets:
                self.entry(RAM, 0, a)
            # every traced jump target and return address, in each bank its window ran with
            for o, fl in enumerate(self.trace.cov):
                if fl & (R_JTGT | R_RET) and fl & R_OP:
                    b, lo = o >> 14, o & 0x3FFF
                    for w in (0, 1):
                        if b in self.trace.where.get(w, ()):
                            self.entry(w, b, (w << 14) | lo)
        self.phase = 'walk'
        self._walk()
        if self.tables and self.trace:                # the rest of the pointer tables the trace saw entries of
            from . import tables
            sites = {k: {t for t in v if t[0] != RAM} for k, v in self.trace.ind.items() if k[0] != RAM}
            for k, v in self.trace.pushret.items():
                sites.setdefault(k, set()).update(v)
            new = tables.word_tables(self, sites) | tables.bank_tables(self, self.trace.targets)
            self.phase = 'table'
            for w, b, a in new:
                before = len(self.region(w, b).entries)
                self.entry(w, b, a)
                self.table_found += len(self.region(w, b).entries) - before
            self.phase = 'walk'
            self._walk()
        if self.far:                                  # calls into a bank, by the constants at the call
            from .farcalls import Resolver
            res, done = Resolver(self), set()
            while True:
                new = []
                for r in list(self.regions.values()):
                    if r.win == RAM:
                        continue                      # its constants may be rewritten as it runs
                    for e in res.resolve(r):
                        if e not in done:
                            done.add(e)
                            new.append(e)
                if not new:
                    break
                self.phase = 'far'
                for w, b, a in new:
                    if w != RAM and (b % self.banks or w == 0):
                        before = len(self.region(w, b).entries)
                        self.entry(w, b, a)
                        self.far_found += len(self.region(w, b).entries) - before
                self.phase = 'walk'
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
            if r.win == RAM:
                continue
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
            if r.win == RAM:
                continue
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
