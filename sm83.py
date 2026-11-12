"""The SM83 (the Game Boy's CPU, MAME's lr35902) instruction set: lengths,
operands and control flow, as MAME's core runs it.

256 opcodes plus 256 behind the CB prefix.  What MAME makes of the odd
ones, which is what the interpreter and the recompiler must agree on:

    10 xx      STOP reads the byte after it (a 2-byte instruction); it only
               does anything when a CGB speed switch is armed (KEY1)
    76         HALT
    D3 DB DD E3 E4 EB EC ED F4 FC FD
               undefined: MAME logs them and goes on (a 1-byte, 4-cycle
               no-op; the hardware locks up).  Refused here, as a walk into
               data.

An instruction is an `Insn`: address, length, raw bytes, the opcode (with
CB: $100 + the second byte), a mnemonic and its operands as text (RGBDS's
syntax, as pret's disassemblies write it), and its control flow:

    kind   None (falls through), 'jump', 'branch' (conditional jump),
           'call', 'ccall' (conditional call), 'rst', 'ret', 'cret'
           (conditional return), 'reti', 'jphl', 'halt', 'stop', 'bad'
    target the jump's, call's or RST's destination (None for the others)

    python -m gbkit.sm83 ROM.gb --entry -n 40           # from $0100
    python -m gbkit.sm83 ROM.gb --at 4:6A3B -n 20       # bank:address
    python -m gbkit.sm83 ROM.gb --at 0150 --bank 3      # $4000-$7FFF as bank 3
"""
import argparse
import sys

R8 = ['b', 'c', 'd', 'e', 'h', 'l', '[hl]', 'a']
R16 = ['bc', 'de', 'hl', 'sp']
R16S = ['bc', 'de', 'hl', 'af']
CC = ['nz', 'z', 'nc', 'c']
ALU = ['add', 'adc', 'sub', 'sbc', 'and', 'xor', 'or', 'cp']
CBOPS = ['rlc', 'rrc', 'rl', 'rr', 'sla', 'sra', 'swap', 'srl']
UNDEFINED = frozenset((0xD3, 0xDB, 0xDD, 0xE3, 0xE4, 0xEB, 0xEC, 0xED, 0xF4, 0xFC, 0xFD))
LENGTH = [1] * 256
for _op in (0x06, 0x0E, 0x16, 0x1E, 0x26, 0x2E, 0x36, 0x3E, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38,
            0xC6, 0xCE, 0xD6, 0xDE, 0xE6, 0xEE, 0xF6, 0xFE, 0xE0, 0xF0, 0xE8, 0xF8, 0xCB):
    LENGTH[_op] = 2
for _op in (0x01, 0x11, 0x21, 0x31, 0x08, 0xC2, 0xC3, 0xCA, 0xD2, 0xDA, 0xC4, 0xCC, 0xCD, 0xD4, 0xDC,
            0xEA, 0xFA):
    LENGTH[_op] = 3


class Insn:
    __slots__ = ('addr', 'length', 'raw', 'op', 'mnem', 'args', 'kind', 'target', 'cond')

    def __init__(self, addr, raw, op, mnem, args='', kind=None, target=None, cond=None):
        self.addr, self.raw, self.length, self.op = addr, raw, len(raw), op
        self.mnem, self.args, self.kind, self.target, self.cond = mnem, args, kind, target, cond

    @property
    def next(self):
        return (self.addr + self.length) & 0xFFFF

    @property
    def ends(self):
        """True when nothing falls through to the next instruction."""
        return self.kind in ('jump', 'ret', 'reti', 'jphl', 'bad')

    def text(self):
        return f'{self.mnem} {self.args}'.rstrip()

    def __repr__(self):
        return f'{self.addr:04X}  {self.raw.hex(" "):9} {self.text()}'


def decode(read, addr):
    """The instruction at `addr`, read(a) giving the byte at a (16-bit, wraps)."""
    a = addr & 0xFFFF
    op = read(a)
    n = LENGTH[op]
    raw = bytes([op] + [read((a + i) & 0xFFFF) for i in range(1, n)])
    nxt = (a + n) & 0xFFFF
    i8 = raw[1] if n >= 2 else 0
    i16 = raw[1] | raw[2] << 8 if n == 3 else 0
    rel = (nxt + (i8 - 256 if i8 & 0x80 else i8)) & 0xFFFF

    def I(m, args='', **kw):
        return Insn(a, raw, op, m, args, **kw)

    if op in UNDEFINED:
        return I('db', f'${op:02X}', kind='bad')
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    if op == 0xCB:
        cb = raw[1]
        cx, cy, cz = cb >> 6, (cb >> 3) & 7, cb & 7
        o = 0x100 | cb
        if cx == 0:
            return Insn(a, raw, o, CBOPS[cy], R8[cz])
        return Insn(a, raw, o, ('bit', 'res', 'set')[cx - 1], f'{cy}, {R8[cz]}')
    if x == 1:
        if op == 0x76:
            return I('halt', kind='halt')
        return I('ld', f'{R8[y]}, {R8[z]}')
    if x == 2:
        return I(ALU[y], f'a, {R8[z]}')
    if x == 0:
        if z == 0:
            if y == 0:
                return I('nop')
            if y == 1:
                return I('ld', f'[${i16:04X}], sp')
            if y == 2:
                return I('stop', kind='stop')
            if y == 3:
                return I('jr', f'${rel:04X}', kind='jump', target=rel)
            return I('jr', f'{CC[y - 4]}, ${rel:04X}', kind='branch', target=rel, cond=y - 4)
        if z == 1:
            if y & 1:
                return I('add', f'hl, {R16[y >> 1]}')
            return I('ld', f'{R16[y >> 1]}, ${i16:04X}')
        if z == 2:
            m = ['[bc]', '[de]', '[hli]', '[hld]'][y >> 1]
            return I('ld', f'a, {m}' if y & 1 else f'{m}, a')
        if z == 3:
            return I('dec' if y & 1 else 'inc', R16[y >> 1])
        if z == 4:
            return I('inc', R8[y])
        if z == 5:
            return I('dec', R8[y])
        if z == 6:
            return I('ld', f'{R8[y]}, ${i8:02X}')
        return I(['rlca', 'rrca', 'rla', 'rra', 'daa', 'cpl', 'scf', 'ccf'][y])
    # x == 3
    if z == 0:
        if y < 4:
            return I('ret', CC[y], kind='cret', cond=y)
        if y == 4:
            return I('ldh', f'[${0xFF00 | i8:04X}], a')
        if y == 5:
            return I('add', f'sp, {i8 - 256 if i8 & 0x80 else i8}')
        if y == 6:
            return I('ldh', f'a, [${0xFF00 | i8:04X}]')
        d = i8 - 256 if i8 & 0x80 else i8
        return I('ld', f'hl, sp{d:+d}')
    if z == 1:
        if y & 1 == 0:
            return I('pop', R16S[y >> 1])
        return [I('ret', kind='ret'), I('reti', kind='reti'), I('jp', 'hl', kind='jphl'), I('ld', 'sp, hl')][y >> 1]
    if z == 2:
        if y < 4:
            return I('jp', f'{CC[y]}, ${i16:04X}', kind='branch', target=i16, cond=y)
        return I('ld', ['[$ff00+c], a', f'[${i16:04X}], a', 'a, [$ff00+c]', f'a, [${i16:04X}]'][y - 4])
    if z == 3:
        if y == 0:
            return I('jp', f'${i16:04X}', kind='jump', target=i16)
        return I('di' if y == 6 else 'ei')       # y == 7; CB handled above, the rest undefined
    if z == 4:
        return I('call', f'{CC[y]}, ${i16:04X}', kind='ccall', target=i16, cond=y)
    if z == 5:
        if y & 1 == 0:
            return I('push', R16S[y >> 1])
        return I('call', f'${i16:04X}', kind='call', target=i16)
    if z == 6:
        return I(ALU[y], f'a, ${i8:02X}')
    return I('rst', f'${y * 8:02X}', kind='rst', target=y * 8)


def main(argv=None):
    from .cart import Cart
    ap = argparse.ArgumentParser(prog='gbkit.sm83')
    ap.add_argument('rom')
    ap.add_argument('--entry', action='store_true', help='from $0100')
    ap.add_argument('--at', help='[bank:]address (hex)')
    ap.add_argument('--bank', type=lambda s: int(s, 16), default=1, help='ROM bank at $4000-$7FFF (hex)')
    ap.add_argument('-n', type=int, default=32)
    a = ap.parse_args(argv)
    c = Cart.load(a.rom)
    bank, addr = a.bank, 0x100
    if a.at:
        if ':' in a.at:
            b, s = a.at.split(':')
            bank, addr = int(b, 16), int(s, 16)
        else:
            addr = int(a.at, 16)
    read = c.reader(bank)
    for _ in range(a.n):
        i = decode(read, addr)
        where = f'{0 if i.addr < 0x4000 else bank:02X}:' if i.addr < 0x8000 else '--:'
        print(f'{where}{i!r}')
        addr = i.next


if __name__ == '__main__':
    sys.exit(main())
