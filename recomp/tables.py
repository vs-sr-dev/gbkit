"""Pointer tables, completed from the entries a trace saw.

Much of a game's code is reached through tables: jump tables of 2-byte
addresses (a state machine's states, a menu's actions, behind JP HL or a
push and a RET), and tables of 3-byte (bank, address) records for calls into
any bank (pret's Predef).  A trace sees the entries the monkey made the game
take; the rest of each table is in the ROM next to them.

So: for each set of targets a trace saw from one site, the table holding
two or more of them (consecutive entries, in the bank the site ran in or in
bank 0) is found, and grown both ways while each entry is a plausible
pointer -- into the same window as the traced ones, at an address whose
instruction decodes (no undefined opcode) -- up to MAX entries.  The same
for (bank, address) records whose traced targets are cross-bank entries.
What comes out is entries for discovery; one in the wrong place costs code,
never correctness, and pret's disassemblies keep it honest (discover --sym).
"""
from .. import sm83

MAX = 256


def _plausible(prog, w, bank, a):
    if (a >> 14) != w or a >= 0x8000:
        return False
    d = prog.data
    o = prog.off(bank, a)
    if o >= len(d):
        return False

    def read(x):
        if (x >> 14) != w:
            raise IndexError(x)
        oo = prog.off(bank, x)
        return d[oo] if oo < len(d) else 0xFF
    try:
        for _ in range(3):                    # the first instructions decode, and are not padding
            i = sm83.decode(read, a)
            if i.kind == 'bad':
                return False
            a = i.next
    except IndexError:
        return False
    return d[o] != 0xFF                        # RST $38: the padding of an empty ROM


def _grow(prog, base, n0, n1, stride, entry_at):
    """Entries of a table at ROM offset base (n0..n1 already known good), grown both ways."""
    out = []
    k = n0 - 1
    while k >= 0 and n1 - k < MAX:
        e = entry_at(base + k * stride)
        if e is None:
            break
        out.append(e)
        k -= 1
    k = n1 + 1
    while k - n0 < MAX:
        e = entry_at(base + k * stride)
        if e is None:
            break
        out.append(e)
        k += 1
    return out


def word_tables(prog, sites):
    """sites: {site: {(w, bank, addr)}} for JP HL / push+ret sites. Returns new (w, bank, addr) entries."""
    d = prog.data
    found = set()
    for site, targets in sites.items():
        by_wb = {}
        for w, b, a in targets:
            by_wb.setdefault((w, b % prog.banks), set()).add(a)
        sw, sb = site[0], site[1] % prog.banks
        for (w, b), addrs in by_wb.items():
            if len(addrs) < 2:
                continue
            # where the table can be: the site's own bank, or bank 0
            for tb in {sb, 0}:
                lo, hi = tb << 14, min(len(d), (tb + 1) << 14)
                pos = {}
                for o in range(lo, hi - 1):
                    v = d[o] | d[o + 1] << 8
                    if v in addrs:
                        pos.setdefault(o, v)
                for o in sorted(pos):
                    if o + 2 not in pos and o - 2 not in pos:
                        continue                  # two traced entries side by side, at least

                    def entry_at(x, w=w, b=b, lo=lo, hi=hi):
                        if x < lo or x + 1 >= hi:
                            return None
                        v = d[x] | d[x + 1] << 8
                        return (w, b, v) if _plausible(prog, w, b, v) else None
                    if o - 2 in pos:
                        continue                  # grown from the run's first
                    n1 = 0
                    while o + 2 * (n1 + 1) in pos:
                        n1 += 1
                    for e in _grow(prog, o, 0, n1, 2, entry_at):
                        found.add(e)
    return found


def bank_tables(prog, targets):
    """targets: {(w, bank, addr)} reached across banks. Finds runs of (bank, lo, hi) records
    holding two or more of them, and grows them. Returns new entries."""
    d = prog.data
    want = {(b % prog.banks, a) for w, b, a in targets if w == 1}
    if len(want) < 2:
        return set()
    hits = set()
    for o in range(0, len(d) - 2):
        if (d[o], d[o + 1] | d[o + 2] << 8) in want:
            hits.add(o)
    found = set()
    for o in sorted(hits):
        if o + 3 not in hits or o - 3 in hits:
            continue
        n1 = 0
        while o + 3 * (n1 + 1) in hits:
            n1 += 1

        def entry_at(x):
            if x < 0 or x + 2 >= len(d) or (x >> 14) != (o >> 14):
                return None
            b, a = d[x], d[x + 1] | d[x + 2] << 8
            if b == 0 or b >= prog.banks:
                return None
            return (1, b, a) if _plausible(prog, 1, b, a) else None
        for e in _grow(prog, o, 0, n1, 3, entry_at):
            found.add(e)
    return found
