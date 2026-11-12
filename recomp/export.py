"""Discovery as JSON: what Pocketshift (Game Boy to Game Gear) imports from gbkit.

    python -m gbkit.recomp.export ROM.gb [--trace DIR ...] [--out FILE.json]
    python -m gbkit.recomp.export --all "gbkit/Game Boy" [--jobs N]     # every cart -> build/export/KEY.json

One file per cartridge, made of what discovery (recomp/discover.py) and the
traces (tracer/gbtrace) know -- the same discovery the recompiler emits C++
from, so what the JSON says is code is what the lockstep holds identical:

* `cart`: what the cartridge is (MAME's lists, by CRC) and its header;
* `regions`: code by window and ROM bank (or `ram`), each with its entries
  -- and how each was found: `reset`, `trace` (a target the trace saw),
  `walk` (a call or a jump of code already found), `table` (a pointer
  table completed from traced entries), `far` (a call into a bank resolved
  from the constants at the call) -- its other ways in (jump targets and
  return addresses), its code as runs of consecutive instructions
  ([start, end): decode from start to end), its JP HL sites with their
  traced targets, its calls whose caller goes on elsewhere (an RST's
  inline arguments), and where a walk stopped (into data, off the window);
* `tables`: the pointer tables completed (2-byte addresses, or 3-byte
  bank and address records), by ROM bank, address and entries;
* `ram`: the code run from RAM -- the routines compiled from the bytes
  they ran with (the OAM DMA routine marked `oam_dma`), and the addresses
  whose bytes changed between runs (self-modifying or reloaded code; `op`
  when only the operands changed);
* `sites`: every instruction that reads or writes the hardware -- VRAM,
  OAM, the I/O registers, the mapper, the cartridge's RAM, the echo RAM, IE
  -- or that reads or writes memory through a computed address: its mode
  (`fixed`, or the register pair or C that makes the address; `sp` for a
  push, pop, call or return the trace saw reach the hardware -- Pokémon Red
  pops from the cartridge's RAM), the classes and I/O registers the trace
  saw it reach (`traced`: false when it never ran, so only its mode is
  known; a computed access the trace saw reach plain RAM only is counted in
  the summary, not listed).  Pocketshift's problem number one: a computed
  address that reaches VRAM or I/O.

Addresses are hex strings: "BBB:AAAA" (a ROM bank and an address), "RAM:AAAA",
or "AAAA" inside a region (whose window and bank say the rest).
"""
import argparse
import concurrent.futures
import json
import os
import re
import sys

from .. import cart as cartmod
from .. import sm83
from . import discover
from .discover import RAM

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FORMAT, VERSION = 'gbkit-discovery', 1
CLASSES = [(1, 'vram'), (2, 'oam'), (4, 'io'), (8, 'mbc'), (16, 'sram'), (32, 'echo'), (64, 'ie')]


def x4(a):
    return f'{a:04X}'


def loc(w, bank, a):
    return f'RAM:{a:04X}' if w == RAM else f'{bank:03X}:{a:04X}'


def aclass(a):
    """The class of a fixed address (as the tracer's), or None for plain RAM."""
    if a < 0x8000:
        return 'mbc'
    if a < 0xA000:
        return 'vram'
    if a < 0xC000:
        return 'sram'
    if a < 0xE000:
        return None
    if a < 0xFE00:
        return 'echo'
    if a < 0xFEA0:
        return 'oam'
    if a < 0xFF00:
        return None
    if a < 0xFF80:
        return 'io'
    return 'ie' if a == 0xFFFF else None


def access(i):
    """The memory access of instruction i beyond the stack: (mode, fixed address or None, reads, writes),
    or None.  Modes: fixed, hl, hl+, hl-, bc, de, c (= $FF00 + C), sp (the stack)."""
    op = i.op
    if op >= 0x100:                                       # CB: (hl) when the register field is 6
        if (op & 7) != 6:
            return None
        return 'hl', None, True, (op & 0xC0) != 0x40      # BIT only reads
    raw = i.raw
    if op == 0xE0:
        return 'fixed', 0xFF00 | raw[1], False, True
    if op == 0xF0:
        return 'fixed', 0xFF00 | raw[1], True, False
    if op == 0xEA:
        return 'fixed', raw[1] | raw[2] << 8, False, True
    if op == 0xFA:
        return 'fixed', raw[1] | raw[2] << 8, True, False
    if op == 0x08:
        return 'fixed', raw[1] | raw[2] << 8, False, True
    if op == 0xE2:
        return 'c', None, False, True
    if op == 0xF2:
        return 'c', None, True, False
    if op in (0x02, 0x12):
        return ('bc', 'de')[op >> 4], None, False, True
    if op in (0x0A, 0x1A):
        return ('bc', 'de')[op >> 4], None, True, False
    if op in (0x22, 0x32):
        return ('hl+', 'hl-')[op == 0x32], None, False, True
    if op in (0x2A, 0x3A):
        return ('hl+', 'hl-')[op == 0x3A], None, True, False
    if op in (0x34, 0x35):
        return 'hl', None, True, True
    if op == 0x36:
        return 'hl', None, False, True
    if 0x70 <= op <= 0x77 and op != 0x76:
        return 'hl', None, False, True
    if 0x40 <= op <= 0xBF and (op & 7) == 6 and op != 0x76:
        return 'hl', None, True, False
    if op in (0xC1, 0xD1, 0xE1, 0xF1) or i.kind in ('ret', 'cret', 'reti'):
        return 'sp', None, True, False                    # through SP (a game may point it at VRAM or SRAM)
    if op in (0xC5, 0xD5, 0xE5, 0xF5) or i.kind in ('call', 'ccall', 'rst'):
        return 'sp', None, False, True
    return None


def runs(code):
    """Instructions -> [[start, end)]: runs where each instruction starts where the last ended."""
    out = []
    for a in sorted(code):
        i = code[a]
        if out and out[-1][1] == a:
            out[-1][1] = a + i.length
        else:
            out.append([a, a + i.length])
    return [[x4(s), x4(e)] for s, e in out]


def is_oam_dma(r, entry):
    """The routine at entry (in the RAM region) writes $FF46: the OAM DMA routine (it waits for the copy)."""
    a, seen = entry, set()
    while a in r.code and a not in seen and len(seen) < 64:
        seen.add(a)
        i = r.code[a]
        if i.op == 0xE0 and i.raw[1] == 0x46:
            return True
        if i.op == 0xE2 or i.kind in ('ret', 'reti', 'jphl', 'jump'):
            if i.op == 0xE2:                              # ld [c], a: with C = $46 just before
                prev = [b for b in r.code.values() if b.next == a and b.op == 0x0E and b.raw[1] == 0x46]
                if prev:
                    return True
                a = i.next
                continue
            return False
        a = i.next
    return False


def read_sites(trace_dirs):
    """The trace's sites: loc -> {w, r, io_w, io_r, n}, merged over the traces."""
    out = {}
    for d in trace_dirs:
        rep = open(os.path.join(d, 'report.txt'), encoding='utf8', errors='replace').read()
        for at, w, r, iw, ir, n in re.findall(
                r'^site (\S+) w ([0-9A-F]{2}) r ([0-9A-F]{2}) io_w (\S+) io_r (\S+) n (\d+)', rep, re.M):
            s = out.setdefault(at, dict(w=0, r=0, io_w=set(), io_r=set(), n=0))
            s['w'] |= int(w, 16)
            s['r'] |= int(r, 16)
            if iw != '-':
                s['io_w'].update(int(x, 16) for x in iw.split(','))
            if ir != '-':
                s['io_r'].update(int(x, 16) for x in ir.split(','))
            s['n'] += int(n)
    return out


def ran_at(p, r, a):
    """The trace ran the instruction at a of region r."""
    if not p.trace:
        return False
    if r.win == RAM:
        return a in p.trace.ramcode
    return bool(p.trace.flag(p.off(r.bank, a)) & discover.R_OP)


def _export_one(rom, build_dir):
    c = cartmod.Cart.load(rom)
    if not c.listed:
        return None
    doc = export(rom, discover.traces_for(build_dir, c.key))
    write(doc, os.path.join(build_dir, 'export', c.key + '.json'))
    return {'cart': doc['cart'], 'summary': doc['summary']}


def classes(mask):
    return [name for bit, name in CLASSES if mask & bit]


def export(rom, trace_dirs):
    c = cartmod.Cart.load(rom)
    tr = discover.Trace(trace_dirs, c) if trace_dirs else None
    p = discover.Program(c, tr).discover()
    traced = read_sites(trace_dirs) if trace_dirs else {}

    regions, sites, used_sites, ptr_ram = [], [], set(), 0
    for k in sorted(p.regions):
        r = p.regions[k]
        regions.append({
            'name': r.name,
            'window': 'ram' if r.win == RAM else r.win,
            'bank': None if r.win == RAM else r.bank,
            'entries': {x4(a): p.origin.get((r.win, r.bank, a), 'walk') for a in sorted(r.entries)},
            'resumes': [x4(a) for a in sorted(r.resumes - r.entries)],
            'code': runs(r.code),
            'instructions': len(r.code),
            'jp_hl': {x4(a): [x4(t) for t in ts] for a, ts in sorted(r.indirect.items())},
            'calls_elsewhere': {x4(a): [x4(t) for t in sorted(ts)] for a, ts in sorted(r.alt_ret.items())},
            'stops': [x4(a) for a in sorted(set(r.problems))],
        })
        for a in sorted(r.code):
            i = r.code[a]
            acc = access(i)
            if not acc:
                continue
            mode, fixed, rd, wr = acc
            at = loc(r.win, r.bank, a)
            t = traced.get(at)
            if mode == 'fixed' and aclass(fixed) is None:
                continue                                  # a fixed address in plain RAM: nothing to say
            ran = ran_at(p, r, a)
            if mode == 'sp' and not t:
                continue                                  # the stack, in RAM as far as anyone saw
            if not t and ran and mode != 'fixed':
                ptr_ram += 1                              # ran, through a pointer, reaching plain RAM only
                continue
            s = {'at': at, 'insn': i.text(), 'mode': mode}
            if fixed is not None:
                s['addr'] = x4(fixed)
            s['read'], s['write'] = rd, wr
            if t:
                used_sites.add(at)
                s['traced'] = True
                s['w'], s['r'] = classes(t['w']), classes(t['r'])
                if t['io_w']:
                    s['io_w'] = [x4(0xFF00 + x) for x in sorted(t['io_w'])]
                if t['io_r']:
                    s['io_r'] = [x4(0xFF00 + x) for x in sorted(t['io_r'])]
                s['n'] = t['n']
            else:
                s['traced'] = ran
                if fixed is not None:
                    cl = aclass(fixed)
                    s['w'], s['r'] = ([cl] if wr else []), ([cl] if rd else [])
            sites.append(s)
    # sites the trace saw in code discovery does not hold (RAM code left to the interpreter): as the trace has them
    for at, t in sorted(traced.items()):
        if at in used_sites:
            continue
        s = {'at': at, 'insn': None, 'mode': None, 'traced': True, 'w': classes(t['w']), 'r': classes(t['r'])}
        if t['io_w']:
            s['io_w'] = [x4(0xFF00 + x) for x in sorted(t['io_w'])]
        if t['io_r']:
            s['io_r'] = [x4(0xFF00 + x) for x in sorted(t['io_r'])]
        s['n'] = t['n']
        sites.append(s)

    ram = {'routines': [], 'image': [], 'unstable': []}
    rr = p.regions.get((RAM, 0))
    if rr:
        ram['routines'] = [{'entry': x4(a), 'found': p.origin.get((RAM, 0, a), 'walk'), 'oam_dma': is_oam_dma(rr, a)}
                           for a in sorted(rr.entries)]
        ram['image'] = [[x4(a), b.hex().upper()] for a, b in rr.runs()]
    if tr:
        for a, vs in sorted(tr.ramcode.items()):
            if len(vs) == 1 and not any(isinstance(v, str) for v in vs):
                continue
            u = {'addr': x4(a)}
            star = [v for v in vs if isinstance(v, str)]
            if star:
                u['versions'] = 'many'
                if star[0][1:]:
                    u['op'] = star[0][1:]                 # only the operands change
            else:
                u['versions'] = sorted(v.hex().upper() for v in vs)
            ram['unstable'].append(u)

    tables = []
    for kind, off, n, stride in sorted(set(p.table_spans), key=lambda t: t[1]):
        b, a = off >> 14, off & 0x3FFF
        tables.append({'kind': kind, 'at': loc(0, b, a | (0x4000 if b else 0)), 'entries': n, 'stride': stride})

    origins = {}
    for r in p.regions.values():
        for a in r.entries:
            o = p.origin.get((r.win, r.bank, a), 'walk')
            origins[o] = origins.get(o, 0) + 1
    cov = p.coverage()
    ptr_hw = sum(1 for s in sites if s['mode'] not in ('fixed', None) and s['traced']
                 and set(s['w'] + s['r']) & {'vram', 'oam', 'io'})
    doc = {
        'format': FORMAT, 'version': VERSION,
        'cart': {
            'key': c.key, 'title': c.title, 'file': os.path.basename(rom), 'crc32': f'{c.crc:08X}',
            'list': c.list, 'listed': c.listed, 'model': c.model_class, 'mapper': c.mapper, 'slot': c.slot,
            'rom_size': c.size, 'rom_banks': c.banks, 'ram_size': c.ram_size, 'battery': c.battery, 'rtc': c.rtc,
            'sgb': bool(c.sgb), 'header_title': c.header_title,
        },
        'traces': [os.path.basename(d) for d in trace_dirs or []],
        'summary': {
            'regions': len(p.regions), 'instructions': sum(len(r.code) for r in p.regions.values()),
            'entries': origins, 'traced_held': list(cov) if cov else None,
            'ram_code_bytes': sum(len(b) // 2 for _, b in ram['image']), 'ram_unstable': len(ram['unstable']),
            'tables': len(tables), 'sites': len(sites),
            'sites_ptr_to_hw': ptr_hw, 'sites_ptr_ram_only': ptr_ram,
        },
        'regions': regions,
        'tables': tables,
        'ram': ram,
        'sites': sites,
    }
    return doc


def write(doc, path):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, 'w', encoding='utf8', newline='\n') as f:
        json.dump(doc, f, separators=(',', ':'), ensure_ascii=False)
        f.write('\n')


def main(argv=None):
    ap = argparse.ArgumentParser(prog='gbkit.recomp.export')
    ap.add_argument('rom', nargs='?')
    ap.add_argument('--trace', action='append', help='a trace directory (more than one: merged)')
    ap.add_argument('--out')
    ap.add_argument('--all', metavar='ROMDIR', help='every cart in ROMDIR, with its traces in build/trace')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--build-dir', default=os.path.join(ROOT, 'build'))
    a = ap.parse_args(argv)
    if a.all:
        from ..tracer.survey import roms_in
        n = 0
        roms = roms_in(a.all)
        with concurrent.futures.ProcessPoolExecutor(max_workers=a.jobs) as ex:
            for doc in ex.map(_export_one, roms, [a.build_dir] * len(roms)):
                if doc:
                    n += 1
                    s = doc['summary']
                    print(f"{doc['cart']['key']:24} {s['instructions']:6} insns {s['regions']:4} regions "
                          f"{s['ram_code_bytes']:5} B RAM code {s['sites']:5} sites ({s['sites_ptr_to_hw']} by pointer to hw)",
                          flush=True)
        print(f'{n} carts -> {os.path.join(a.build_dir, "export")}')
        return 0
    if not a.rom:
        ap.error('a ROM or --all')
    c = cartmod.Cart.load(a.rom)
    tds = a.trace if a.trace is not None else discover.traces_for(a.build_dir, c.key)
    doc = export(a.rom, tds)
    out = a.out or os.path.join(a.build_dir, 'export', c.key + '.json')
    write(doc, out)
    s = doc['summary']
    print(f"{c.key}: {s['regions']} regions, {s['instructions']} instructions, entries {s['entries']}, "
          f"{s['tables']} tables, {s['ram_code_bytes']} B of RAM code ({s['ram_unstable']} addresses unstable), "
          f"{s['sites']} sites ({s['sites_ptr_to_hw']} reach VRAM/OAM/I-O by pointer) -> {out}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
