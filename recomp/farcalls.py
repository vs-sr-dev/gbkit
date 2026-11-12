"""Calls into a bank, resolved without running the game.

A program reaches the code in $4000-$7FFF of another bank through the
mapper: it writes the bank number to $2000-$3FFF, then calls or jumps.  The
games say it in a few ways, all of which come down to a write of a value
the code holds in a register, and a transfer to an address it holds in
another, or in the bytes after a call:

    ld a, BANK / ld [$2000], a / call Target          (pret's homecall)
    ld b, BANK / ld hl, Target / call Trampoline      (Bankswitch: writes b, jp hl)
    ld a, BANK / call SwitchTo / call Target          (a routine that switches and returns)
    rst $xx / db BANK / dw Target                     (the arguments inline after an RST)

So the routines of bank 0 are summarized by running them symbolically --
straight-line code, from their entry, the registers as their inputs -- for
what they leave behind: the value they last wrote to the mapper's bank
register, how they leave (return, or JP HL to an address made of their
inputs or of the bytes after their call), and what the registers hold then.
Then each region's code is read in order, the registers' constants carried
along each straight run (and forgotten wherever control can come in from
elsewhere), and at every call, jump or RST whose summary or whose target
in $4000-$7FFF needs a bank, the bank and the target come out as numbers:
an entry for discovery.  What cannot be told so (a bank from RAM, a table
indexed at run time) is left to the trace.
"""
from .. import sm83

R8 = ('b', 'c', 'd', 'e', 'h', 'l', None, 'a')
PAIRS = (('b', 'c'), ('d', 'e'), ('h', 'l'))
MAX_STEPS = 80


def is_const(v):
    return isinstance(v, tuple) and v[0] == 'c'


def C(x):
    return ('c', x & 0xFF)


class Summary:
    """What a routine leaves: bank (the value last written to the bank register, or None),
    exit ('ret', 'jphl', or None: not understood), target (h, l) for 'jphl', regs at the exit."""
    __slots__ = ('bank', 'exit', 'target', 'regs', 'wrote')

    def __init__(self):
        self.bank, self.exit, self.target, self.regs, self.wrote = None, None, None, {}, False


def bank_write(cart, addr):
    """Whether a write to addr selects the ROM bank (the low register, which every mapper has)."""
    return 0x2000 <= addr < 0x4000 and cart.mapper not in ('ROM',)


def summarize(read, entry, cart):
    """Run the routine at entry (bank 0) symbolically. Inputs are ('in', r); the return address
    pushed by its caller is ('ret', 0); a byte read from ret + k is ('inl', k)."""
    regs = {r: ('in', r) for r in ('a', 'b', 'c', 'd', 'e', 'h', 'l')}
    stack = [('retaddr', 0)]
    mem = {}
    s = Summary()
    a = entry
    seen = set()
    for _ in range(MAX_STEPS):
        if a in seen or a >= 0x4000:
            return s
        seen.add(a)
        try:
            i = sm83.decode(read, a)
        except IndexError:
            return s
        op, raw = i.op, i.raw
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        imm8 = raw[1] if i.length >= 2 else 0
        imm16 = raw[1] | raw[2] << 8 if i.length == 3 else 0

        def hl():
            return regs['h'], regs['l']

        def hl_ret():
            h, l = hl()
            if h == ('hiret', 0) and isinstance(l, tuple) and l[0] == 'loret':
                return l[1]
            return None

        def setpair(p, v16):
            hi, lo = PAIRS[p]
            regs[hi], regs[lo] = v16
        if i.kind == 'bad' or op >= 0x100:
            if op >= 0x100 and (op & 7) != 6 and (op & 0xC0) != 0x40:     # CB on a register: unknown result
                r = R8[op & 7]
                regs[r] = None
            elif op >= 0x100 and (op & 0xC0) == 0x40:
                pass                                                     # BIT: flags only
            elif op >= 0x100:
                pass
            else:
                return s
        elif x == 1 and op != 0x76:
            if y == 6:                                                   # ld [hl], r
                k = hl()
                if is_const(k[0]) and is_const(k[1]):
                    ad = k[0][1] << 8 | k[1][1]
                    if bank_write(cart, ad):
                        s.bank, s.wrote = regs[R8[z]], True
                    mem[ad] = regs[R8[z]]
            elif z == 6:                                                 # ld r, [hl]
                k = hl_ret()
                regs[R8[y]] = ('inl', k) if k is not None else None
            else:
                regs[R8[y]] = regs[R8[z]]
        elif x == 2:
            r = R8[z]
            if y == 7:
                pass                                                     # cp: flags only
            elif y in (5, 6) and z == 7:                                 # xor a / or a
                regs['a'] = C(0) if y == 5 else regs['a']
            else:
                regs['a'] = None
        elif op in (0x06, 0x0E, 0x16, 0x1E, 0x26, 0x2E, 0x3E):
            regs[R8[y]] = C(imm8)
        elif op in (0x01, 0x11, 0x21):
            setpair(op >> 4, (C(imm16 >> 8), C(imm16)))
        elif op == 0xEA:                                                 # ld [nn], a
            if bank_write(cart, imm16):
                s.bank, s.wrote = regs['a'], True
            mem[imm16] = regs['a']
        elif op == 0xFA:
            regs['a'] = mem.get(imm16)
        elif op == 0xE0:
            mem[0xFF00 | imm8] = regs['a']
        elif op == 0xF0:
            regs['a'] = mem.get(0xFF00 | imm8)
        elif op in (0x2A, 0x3A):                                         # ld a, [hl+/-]
            k = hl_ret()
            regs['a'] = ('inl', k) if k is not None else None
            if k is not None and op == 0x2A:
                regs['l'] = ('loret', k + 1)
            else:
                regs['h'] = regs['l'] = None
        elif op in (0x22, 0x32):
            regs['h'] = regs['l'] = None
        elif op in (0x0A, 0x1A):
            regs['a'] = None
        elif op in (0x02, 0x12):
            pass
        elif op in (0x03, 0x13, 0x23, 0x0B, 0x1B, 0x2B):
            p = (op >> 4) & 3
            if op == 0x23 and hl_ret() is not None:
                regs['l'] = ('loret', hl_ret() + 1)
            else:
                hi, lo = PAIRS[p]
                regs[hi] = regs[lo] = None
        elif op in (0x33, 0x3B, 0x39, 0xE8, 0xF8, 0xF9, 0x31):
            if op in (0xF8, 0x39):
                regs['h'] = regs['l'] = None
            if op in (0x31, 0xF9, 0xE8, 0x33, 0x3B):
                return s                                                 # the stack moved: give up
        elif x == 0 and z in (4, 5):
            if y != 6:
                regs[R8[y]] = None
        elif op in (0x09, 0x19, 0x29):
            regs['h'] = regs['l'] = None
        elif op in (0x07, 0x0F, 0x17, 0x1F, 0x27, 0x2F, 0x37, 0x3F, 0x00, 0xF3, 0xFB, 0x34, 0x35, 0x36, 0x08):
            if op in (0x07, 0x0F, 0x17, 0x1F, 0x27, 0x2F):
                regs['a'] = None
        elif op in (0xC5, 0xD5, 0xE5, 0xF5):
            p = (op >> 4) & 3
            stack.append(('pair', (regs['a'], None) if p == 3 else (regs[PAIRS[p][0]], regs[PAIRS[p][1]])))
        elif op in (0xC1, 0xD1, 0xE1, 0xF1):
            if not stack:
                return s
            v = stack.pop()
            p = (op >> 4) & 3
            if v[0] == 'retaddr':
                v16 = (('hiret', 0), ('loret', 0))
            elif v[0] == 'pair':
                v16 = v[1]
            else:
                v16 = (None, None)
            if p == 3:
                regs['a'] = v16[0]
            else:
                setpair(p, v16)
        elif i.kind in ('jump',):
            a = i.target
            continue
        elif i.kind == 'jphl':
            s.exit, s.target, s.regs = 'jphl', hl(), dict(regs)
            return s
        elif i.kind in ('ret', 'reti'):
            if stack and stack[-1][0] == 'retaddr':
                s.exit, s.regs = 'ret', dict(regs)
            elif stack and stack[-1][0] == 'pair':           # push hl / ret: a jump to what was pushed
                s.exit, s.target, s.regs = 'jphl', stack[-1][1], dict(regs)
            return s
        else:                                                # a branch, a call, HALT...: not straight-line
            return s
        a = i.next
    return s


class Resolver:
    """Summaries of the routines of bank 0, and the constants at each call site of a region."""

    def __init__(self, prog):
        self.p = prog
        self.cart = prog.cart
        self.data = prog.data
        self.sums = {}

    def read0(self, a):
        if a >= 0x4000:
            raise IndexError(a)
        return self.data[a] if a < len(self.data) else 0xFF

    def summary(self, t):
        if t not in self.sums:
            self.sums[t] = summarize(self.read0, t, self.cart) if t < 0x4000 else None
        return self.sums[t]

    def resolve(self, r):
        """[(window, bank, address)] the code of region r transfers to, by the constants it holds."""
        out = []
        ways = set(r.ways_in())
        regs, bank = {}, None
        prev_next = None
        for a in sorted(r.code):
            i = r.code[a]
            if a in ways or a != prev_next:
                regs, bank = {}, None
            prev_next = i.next
            op, raw = i.op, i.raw
            x, y, z = op >> 6, (op >> 3) & 7, op & 7
            imm8 = raw[1] if i.length >= 2 else 0
            imm16 = raw[1] | raw[2] << 8 if i.length == 3 else 0
            # the transfers first, with the state before them
            if i.kind in ('call', 'ccall', 'jump', 'branch', 'rst') and i.target is not None:
                t = i.target
                if t >= 0x4000 and t < 0x8000 and r.win == 0 and bank is not None:
                    out.append((1, bank, t))
                if t < 0x4000:
                    s = self.summary(t)
                    if s and s.exit == 'jphl' and s.wrote:
                        b = self._ev(r, s.bank, regs, a, i)
                        th, tl = (self._ev(r, v, regs, a, i) for v in s.target)
                        if b is not None and th is not None and tl is not None:
                            tt = th << 8 | tl
                            if 0x4000 <= tt < 0x8000:
                                out.append((1, b, tt))
                            elif tt < 0x4000:
                                out.append((0, 0, tt))
                    if s and s.exit == 'ret' and s.wrote:
                        bank = self._ev(r, s.bank, regs, a, i)
                    if i.kind in ('call', 'ccall', 'rst'):
                        if s and s.exit == 'ret':
                            regs = {k: v for k, v in ((k, self._ev(r, v, regs, a, i)) for k, v in s.regs.items())
                                    if v is not None}
                        else:
                            regs = {}
                        continue
                elif i.kind in ('call', 'ccall'):
                    regs = {}
                    continue
            # then what the instruction does to the constants
            if x == 1 and op != 0x76:
                if y == 6:
                    if 'h' in regs and 'l' in regs and z != 6 and R8[z] in regs:
                        if bank_write(self.cart, regs['h'] << 8 | regs['l']):
                            bank = regs[R8[z]]
                elif z == 6:
                    regs.pop(R8[y], None)
                elif R8[z] in regs:
                    regs[R8[y]] = regs[R8[z]]
                else:
                    regs.pop(R8[y], None)
            elif x == 2:
                if y == 5 and z == 7:
                    regs['a'] = 0
                elif y != 7:
                    regs.pop('a', None)
            elif op in (0x06, 0x0E, 0x16, 0x1E, 0x26, 0x2E, 0x3E):
                regs[R8[y]] = imm8
            elif op in (0x01, 0x11, 0x21):
                hi, lo = PAIRS[op >> 4]
                regs[hi], regs[lo] = imm16 >> 8, imm16 & 0xFF
            elif op == 0xEA:
                if bank_write(self.cart, imm16) and 'a' in regs:
                    bank = regs['a']
                elif bank_write(self.cart, imm16):
                    bank = None
            elif op in (0x02, 0x12, 0x22, 0x32, 0x36, 0x34, 0x35):
                pass
            elif op == 0xC1 or op == 0xD1 or op == 0xE1 or op == 0xF1 or op >= 0x100 or (x == 0 and z in (3, 4, 5)) \
                    or op in (0xF0, 0xFA, 0x0A, 0x1A, 0x2A, 0x3A, 0x09, 0x19, 0x29, 0x39, 0xF8, 0x07, 0x0F, 0x17,
                              0x1F, 0x27, 0x2F, 0xC6, 0xCE, 0xD6, 0xDE, 0xE6, 0xEE, 0xF6):
                for k in self._clobbers(op):
                    regs.pop(k, None)
            if i.ends:
                regs, bank = {}, None
        return out

    @staticmethod
    def _clobbers(op):
        if op >= 0x100:
            return (R8[op & 7],) if (op & 0xC0) != 0x40 and (op & 7) != 6 else ()
        if op in (0xC1, 0xD1, 0xE1):
            return PAIRS[(op >> 4) & 3]
        if op == 0xF1 or op in (0xF0, 0xFA, 0x0A, 0x1A, 0x07, 0x0F, 0x17, 0x1F, 0x27, 0x2F, 0xC6, 0xCE, 0xD6, 0xDE,
                                0xE6, 0xEE, 0xF6):
            return ('a',)
        if op in (0x2A, 0x3A):
            return ('a', 'h', 'l')
        if op in (0x09, 0x19, 0x29, 0x39, 0xF8):
            return ('h', 'l')
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        if x == 0 and z in (4, 5) and y != 6:
            return (R8[y],)
        if x == 0 and z == 3:
            return PAIRS[(op >> 4) & 3] if (op >> 4) < 3 else ()
        return ()

    def _ev(self, r, v, regs, at, insn):
        """A summary's symbolic value at a call site of region r: a number, or None."""
        if v is None:
            return None
        if isinstance(v, int):
            return v
        k = v[0]
        if k == 'c':
            return v[1]
        if k == 'in':
            return regs.get(v[1])
        if k == 'inl':                                   # the k-th byte after the call
            ad = insn.next + v[1]
            if ad >= 0x8000 or (ad >> 14) != r.win:
                return None
            o = self.p.off(r.bank, ad)
            return self.data[o] if o < len(self.data) else None
        return None
