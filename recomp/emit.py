"""SM83 to C++, one region at a time.

Every instruction is written as the interpreter (runtime/gb_core.cpp, step)
runs it: the same accesses through the same helpers (gb_rd, gb_wr, gb_push...)
in the same order, the same charges (gb_tick) where it makes them -- the
opcode's and operands' fetches first, 4 cycles each, then the instruction's
own.  The ROM's bytes being known, a fetch is only its charge.

* a label per instruction, and at each the poll (GB_POLL: one compare
  inline -- the events due and the interrupts, as the interpreter's boundary);
  a `goto` where control goes on to an instruction that is not the next one
  emitted; a switch at the top sends each way in to its label;
* CALL and RST push the return address and call: the callee as a C
  function when it is an entry of this region, else through `gb_call` (the
  native code for the bank mapped then, or the interpreter); then the code
  checks it came back (`c.resume`, SP as it was) -- a return to one of the
  traced places the caller goes on at instead (an RST's inline arguments
  skipped) is a goto, any other return elsewhere unwinds;
* RET and RETI leave the address they popped in `c.resume` and return;
* JP HL is a switch over the targets the tracer saw, `gb_call` for the rest
  (a continuation: the code there returns for us);
* after a write that may reach the mapper (any write not to a fixed address
  of $8000 and up), and after a call, the code checks its bank is still
  mapped; if not, `gb_call` goes on from the next instruction;
* HALT is the runtime's; when it leaves the next opcode already fetched
  (IME clear) the interpreter runs the next instruction.

A region becomes several C++ functions of at most CHUNK_MAX instructions,
cut where the fewest jumps cross (g++ is superlinear in a function's size);
a jump between them is a continuation.
"""
from .. import sm83

R8 = ['c.B', 'c.C', 'c.D', 'c.E', 'c.H', 'c.L', None, 'c.A']
RR_GET = ['GB_BC(c)', 'GB_DE(c)', 'GB_HL(c)', 'c.SP']
RR_SET = ['gb_setbc(c, {})', 'gb_setde(c, {})', 'gb_sethl(c, {})', 'c.SP = {}']
ALU = ['gb_add', 'gb_adc', 'gb_sub', 'gb_sbc', 'gb_and', 'gb_xor', 'gb_or', 'gb_cp']
COND = ['!(c.F & FZ)', '(c.F & FZ)', '!(c.F & FC)', '(c.F & FC)']


def h(v, w=4):
    return f'0x{v:0{w}X}'


def T(n):
    """n fetches: 4 cycles each, one charge each (the timer sees each)."""
    return 'gb_tick(c, 4); ' * n


class Emitter:
    """One region as C++ (a list of lines)."""

    CHUNK_MIN, CHUNK_MAX = 600, 2000

    def __init__(self, prog, r, comments=True):
        self.p, self.r, self.comments = prog, r, comments
        self.sites = {}
        self._chunk()

    def _chunk(self):
        r = self.r
        order = sorted(r.code)
        idx = {a: n for n, a in enumerate(order)}
        diff = [0] * (len(order) + 1)
        for n, a in enumerate(order):
            i = r.code[a]
            outs = []
            if i.kind in ('jump', 'branch') and i.target is not None:
                outs.append(i.target)
            outs += r.indirect.get(a, [])
            outs += sorted(r.alt_ret.get(i.next, ()))
            if not i.ends:
                outs.append(i.next)
            for x in outs:
                if x in idx:
                    lo, hi = sorted((n, idx[x]))
                    diff[lo] += 1
                    diff[hi] -= 1
        cross, run = [], 0
        for n in range(len(order)):
            run += diff[n]
            cross.append(run)
        cuts, start = [], 0
        while len(order) - start > self.CHUNK_MAX:
            lo, hi = start + self.CHUNK_MIN, min(start + self.CHUNK_MAX, len(order) - 1)
            best = min(range(lo, hi), key=lambda g: (cross[g], -g))
            cuts.append(best + 1)
            start = best + 1
        bounds = [0] + cuts + [len(order)]
        self.chunks = [order[bounds[k]:bounds[k + 1]] for k in range(len(bounds) - 1)]
        self.chunk_of = {}
        for k, ips in enumerate(self.chunks):
            for a in ips:
                self.chunk_of[a] = k
        self.ways = [set() for _ in self.chunks]
        for a in r.ways_in():
            self.ways[self.chunk_of[a]].add(a)
        self.cur = 0

    def fname(self, k):
        return self.r.name if len(self.chunks) == 1 else f'{self.r.name}_{k}'

    def _count(self, k):
        self.sites[k] = self.sites.get(k, 0) + 1

    def _local(self, t):
        return t in self.r.code

    def _jmp(self, t):
        """A jump to t, held by this region: a goto, or a continuation into the function holding it."""
        k = self.chunk_of[t]
        if k == self.cur:
            return f'goto L_{t:04X};'
        self.ways[k].add(t)
        self._count('chunk jump')
        return (f'{{ if (c.depth < 256) {{ c.depth++; c.n_native_calls++; {self.fname(k)}(c, {h(t)}); c.depth--; }} '
                f'else gb_call(c, {h(t)}); return; }}')

    def _goto(self, t):
        if self._local(t):
            return self._jmp(t)
        self._count('dynamic jump')
        return f'gb_call(c, {h(t)}); return;'

    def _bank_ok(self, nxt):
        """Leave (the interpreter going on from nxt) if the bank under the code changed."""
        r = self.r
        return f' if (c.{"bankx" if r.win else "bank0"} != {h(r.bank, 3)}) {{ gb_call(c, {h(nxt)}); return; }}'

    def _callee(self, t):
        r = self.r
        if r.contains(t) and t in r.entries and t in r.code:
            self._count('direct call')
            k = self.chunk_of[t]
            return (f'if (c.depth < 256) {{ c.depth++; c.n_native_calls++; {self.fname(k)}(c, {h(t)}); c.depth--; }} '
                    f'else gb_call(c, {h(t)});')
        self._count('dynamic call')
        return f'gb_call(c, {h(t)});'

    def _call(self, t, nxt):
        """Push nxt, call t, and go on at nxt when it came back there (or at a traced elsewhere)."""
        alts = sorted(x for x in self.r.alt_ret.get(nxt, ()) if self._local(x))
        back = f'if (c.resume != {h(nxt)} || c.SP != s0) {{ '
        if alts:
            self._count('call going on elsewhere')
            back += f'if (c.SP == s0) switch (c.resume) {{ ' + ' '.join(f'case {h(x)}: {self._jmp(x)}' for x in alts) \
                    + ' default: break; } '
        back += 'return; }'
        return f'gb_push(c, {h(nxt)}); {{ const uint16_t s0 = (uint16_t)(c.SP + 2); {self._callee(t)} {back} }}' \
               + self._bank_ok(nxt)

    def _wr_check(self, addr_const, nxt):
        """After a write: the mapper may have switched the bank, unless the address is fixed and past ROM."""
        if addr_const is not None and addr_const >= 0x8000:
            return ''
        return self._bank_ok(nxt)

    def insn(self, a):
        """(C++ text, whether control goes on to the next instruction)."""
        r = self.r
        i = r.code[a]
        op, n, nxt = i.op, i.length, i.next
        raw = i.raw
        i8 = raw[1] if n >= 2 else 0
        i16 = raw[1] | raw[2] << 8 if n == 3 else 0
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        F = T(n) if op < 0x100 else ''

        def rd8(k):
            return R8[k] if k != 6 else 'gb_rd(c, GB_HL(c))'

        if op == 0xCB:
            raise AssertionError
        if op >= 0x100:                                    # CB xx
            cb = op & 0xFF
            cx, cy, cz = cb >> 6, (cb >> 3) & 7, cb & 7
            pre = T(2)
            if cz == 6:
                if cx == 1:
                    return f'{pre}gb_bit(c, {cy}, gb_rd(c, GB_HL(c)));', True
                val = (f'gb_cbrot(c, {cy}, v)' if cx == 0 else f'(uint8_t)(v & {h(~(1 << cy) & 0xFF, 2)})' if cx == 2
                       else f'(uint8_t)(v | {h(1 << cy, 2)})')
                return (f'{pre}{{ const uint16_t ad = GB_HL(c); uint8_t v = gb_rd(c, ad); v = {val}; gb_wr(c, ad, v); }}'
                        + self._bank_ok(nxt)), True
            reg = R8[cz]
            if cx == 1:
                return f'{pre}gb_bit(c, {cy}, {reg});', True
            val = (f'gb_cbrot(c, {cy}, {reg})' if cx == 0 else f'(uint8_t)({reg} & {h(~(1 << cy) & 0xFF, 2)})' if cx == 2
                   else f'(uint8_t)({reg} | {h(1 << cy, 2)})')
            return f'{pre}{reg} = {val};', True
        if x == 1:
            if op == 0x76:
                self._count('halt')
                return f'{F}gb_halt(c, {h(a)}); if (c.hfetch | c.hbug) {{ gb_call(c, {h(nxt)}); return; }}', True
            if y == 6:
                return f'{F}gb_wr(c, GB_HL(c), {R8[z]});' + self._bank_ok(nxt), True
            return f'{F}{R8[y]} = {rd8(z)};', True
        if x == 2:
            return f'{F}{ALU[y]}(c, {rd8(z)});', True
        if op == 0x00:
            return f'{F}', True
        if op in (0x01, 0x11, 0x21, 0x31):
            return f'{F}' + RR_SET[op >> 4].format(h(i16)) + ';', True
        if op in (0x02, 0x12):
            return f'{F}gb_wr(c, {RR_GET[op >> 4]}, c.A);' + self._bank_ok(nxt), True
        if op == 0x22:
            return f'{F}gb_wr(c, GB_HL(c), c.A); gb_sethl(c, (uint16_t)(GB_HL(c) + 1));' + self._bank_ok(nxt), True
        if op == 0x32:
            return f'{F}gb_wr(c, GB_HL(c), c.A); gb_sethl(c, (uint16_t)(GB_HL(c) - 1));' + self._bank_ok(nxt), True
        if op in (0x0A, 0x1A):
            return f'{F}c.A = gb_rd(c, {RR_GET[op >> 4]});', True
        if op == 0x2A:
            return f'{F}c.A = gb_rd(c, GB_HL(c)); gb_sethl(c, (uint16_t)(GB_HL(c) + 1));', True
        if op == 0x3A:
            return f'{F}c.A = gb_rd(c, GB_HL(c)); gb_sethl(c, (uint16_t)(GB_HL(c) - 1));', True
        if op in (0x03, 0x13, 0x23, 0x33):
            return f'{F}' + RR_SET[op >> 4].format(f'(uint16_t)({RR_GET[op >> 4]} + 1)') + '; gb_tick(c, 4);', True
        if op in (0x0B, 0x1B, 0x2B, 0x3B):
            return f'{F}' + RR_SET[op >> 4].format(f'(uint16_t)({RR_GET[op >> 4]} - 1)') + '; gb_tick(c, 4);', True
        if op in (0x09, 0x19, 0x29, 0x39):
            return f'{F}gb_addhl(c, {RR_GET[op >> 4]}); gb_tick(c, 4);', True
        if x == 0 and z == 4 and y != 6:
            return f'{F}{R8[y]} = gb_inc8(c, {R8[y]});', True
        if x == 0 and z == 5 and y != 6:
            return f'{F}{R8[y]} = gb_dec8(c, {R8[y]});', True
        if op == 0x34:
            return (f'{F}{{ const uint16_t ad = GB_HL(c); uint8_t v = gb_rd(c, ad); const uint8_t f = c.F & FC; v++; '
                    f'gb_wr(c, ad, v); c.F = (uint8_t)(f | (v ? 0 : FZ) | ((v & 0xF) ? 0 : FH)); }}'
                    + self._bank_ok(nxt)), True
        if op == 0x35:
            return (f'{F}{{ const uint16_t ad = GB_HL(c); uint8_t v = gb_rd(c, ad); const uint8_t f = (c.F & FC) | FN; v--; '
                    f'gb_wr(c, ad, v); c.F = (uint8_t)(f | (v ? 0 : FZ) | ((v & 0xF) == 0xF ? FH : 0)); }}'
                    + self._bank_ok(nxt)), True
        if x == 0 and z == 6:
            if y == 6:
                return f'{F}gb_wr(c, GB_HL(c), {h(i8, 2)});' + self._bank_ok(nxt), True
            return f'{F}{R8[y]} = {h(i8, 2)};', True
        if op == 0x07:
            return f'{F}c.A = (uint8_t)(c.A << 1 | c.A >> 7); c.F = (c.A & 1) ? FC : 0;', True
        if op == 0x0F:
            return f'{F}c.A = (uint8_t)(c.A >> 1 | c.A << 7); c.F = (c.A & 0x80) ? FC : 0;', True
        if op == 0x17:
            return f'{F}{{ const uint8_t f = (c.A & 0x80) ? FC : 0; c.A = (uint8_t)(c.A << 1 | ((c.F & FC) ? 1 : 0)); c.F = f; }}', True
        if op == 0x1F:
            return f'{F}{{ const uint8_t f = (c.A & 1) ? FC : 0; c.A = (uint8_t)(c.A >> 1 | ((c.F & FC) ? 0x80 : 0)); c.F = f; }}', True
        if op == 0x27:
            return f'{F}gb_daa(c);', True
        if op == 0x2F:
            return f'{F}c.A = (uint8_t)~c.A; c.F |= FN | FH;', True
        if op == 0x37:
            return f'{F}c.F = (uint8_t)((c.F & FZ) | FC);', True
        if op == 0x3F:
            return f'{F}c.F = (uint8_t)((c.F & FZ) | ((c.F & FC) ? 0 : FC));', True
        if op == 0x08:
            return (f'{F}gb_wr(c, {h(i16)}, (uint8_t)c.SP); gb_wr(c, {h((i16 + 1) & 0xFFFF)}, (uint8_t)(c.SP >> 8));'
                    + self._wr_check(i16 if i16 < 0xFFFF else None, nxt)), True
        if op == 0x10:
            self._count('stop')
            return f'{T(2)}gb_stop(c);', True
        if op == 0x18 or op == 0xC3:
            return f'{F}gb_tick(c, 4); {self._goto(i.target)}', False
        if op in (0x20, 0x28, 0x30, 0x38, 0xC2, 0xCA, 0xD2, 0xDA):
            return f'{F}if ({COND[(op >> 3) & 3]}) {{ gb_tick(c, 4); {self._goto(i.target)} }}', True
        if op == 0xE9:
            self._count('jp hl')
            cases = ' '.join(f'case {h(t)}: {self._jmp(t)}' for t in r.indirect.get(a, []) if self._local(t))
            return f'{F}{{ const uint16_t t = GB_HL(c); switch (t) {{ {cases} default: gb_call(c, t); return; }} }}', False
        if op == 0xCD:
            r.calls.add(i.target)
            self._count('call')
            return f'{F}gb_tick(c, 4); {self._call(i.target, nxt)}', True
        if op in (0xC4, 0xCC, 0xD4, 0xDC):
            self._count('call')
            return f'{F}if ({COND[(op >> 3) & 3]}) {{ gb_tick(c, 4); {self._call(i.target, nxt)} }}', True
        if i.kind == 'rst':
            self._count('rst')
            return f'{F}gb_tick(c, 4); {self._call(i.target, nxt)}', True
        if op == 0xC9:
            return f'{F}{{ const uint16_t t = gb_rd16(c, c.SP); c.SP += 2; gb_tick(c, 4); c.resume = t; return; }}', False
        if op == 0xD9:
            return (f'{F}{{ const uint16_t t = gb_rd16(c, c.SP); c.SP += 2; c.ime = 1; gb_tick(c, 4); gb_recalc(c); '
                    f'c.resume = t; return; }}'), False
        if op in (0xC0, 0xC8, 0xD0, 0xD8):
            return (f'{F}gb_tick(c, 4); if ({COND[(op >> 3) & 3]}) {{ const uint16_t t = gb_rd16(c, c.SP); c.SP += 2; '
                    f'gb_tick(c, 4); c.resume = t; return; }}'), True
        if op in (0xC1, 0xD1, 0xE1):
            return (f'{F}{{ const uint8_t lo = gb_rd(c, c.SP++); const uint8_t hi = gb_rd(c, c.SP++); '
                    + RR_SET[(op >> 4) & 3].format('(uint16_t)(hi << 8 | lo)') + '; }'), True
        if op == 0xF1:
            return f'{F}c.F = gb_rd(c, c.SP++); c.A = gb_rd(c, c.SP++); c.F &= 0xF0;', True
        if op in (0xC5, 0xD5, 0xE5):
            return f'{F}gb_tick(c, 4); gb_push(c, {RR_GET[(op >> 4) & 3]});', True
        if op == 0xF5:
            return f'{F}gb_tick(c, 4); c.F &= 0xF0; gb_push(c, (uint16_t)(c.A << 8 | c.F));', True
        if x == 3 and z == 6:
            return f'{F}{ALU[y]}(c, {h(i8, 2)});', True
        if op == 0xE0:
            return f'{F}gb_wr(c, {h(0xFF00 | i8)}, c.A);', True
        if op == 0xF0:
            return f'{F}c.A = gb_rd(c, {h(0xFF00 | i8)});', True
        if op == 0xE2:
            return f'{F}gb_wr(c, (uint16_t)(0xFF00 | c.C), c.A);', True
        if op == 0xF2:
            return f'{F}c.A = gb_rd(c, (uint16_t)(0xFF00 | c.C));', True
        if op == 0xEA:
            if i16 < 0x8000:
                self._count('mapper write')
            return f'{F}gb_wr(c, {h(i16)}, c.A);' + self._wr_check(i16, nxt), True
        if op == 0xFA:
            return f'{F}c.A = gb_rd(c, {h(i16)});', True
        if op == 0xE8:
            return f'{F}c.SP = gb_spe(c, {h(i8, 2)}); gb_tick(c, 8);', True
        if op == 0xF8:
            return f'{F}gb_sethl(c, gb_spe(c, {h(i8, 2)})); gb_tick(c, 4);', True
        if op == 0xF9:
            return f'{F}c.SP = GB_HL(c); gb_tick(c, 4);', True
        if op == 0xF3:
            return f'{F}gb_di(c);', True
        if op == 0xFB:
            return f'{F}gb_ei(c);', True
        raise ValueError(f'{a:04X}: {i.text()}')

    def emit(self):
        r = self.r
        bodies = []
        for k, ips in enumerate(self.chunks):          # the code first: it finds the jumps between chunks
            self.cur = k
            body = []
            for n, a in enumerate(ips):
                text, on = self.insn(a)
                if self.comments:
                    text += f'  // {a:04X} {r.code[a].text()}'
                body.append(f'L_{a:04X}: GB_POLL(c, {h(a)}, {r.win}, {h(r.bank, 3)});')
                body.append('    ' + text)
                nxt = r.code[a].next
                following = ips[n + 1] if n + 1 < len(ips) else None
                if on and nxt != following:
                    body.append('    ' + self._goto(nxt))
            bodies.append(body)
        lines = []
        self.funcs = []
        for k, body in enumerate(bodies):
            ways = sorted(self.ways[k])
            self.funcs.append((self.fname(k), ways))
            lines.append(f'void {self.fname(k)}(GB& c, uint16_t e) {{')
            lines.append('    switch (e) {')
            for e in ways:
                lines.append(f'    case {h(e)}: goto L_{e:04X};')
            lines.append('    default: c.resume = e; return;')
            lines.append('    }')
            lines += body
            lines.append('}')
            lines.append('')
        return lines
