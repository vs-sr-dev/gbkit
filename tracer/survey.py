"""Trace every cartridge and tabulate what a recompiler faces -- and what
Pocketshift (the Game Boy to Game Gear translator that will import gbkit's
discovery) faces.

    python -m gbkit.tracer.survey "gbkit/Game Boy" [--frames 6000] [--jobs 14]
                                  [--only NAME] [--snap 300,1500,3000,5999]

Each ROM runs headless in tracer/gbtrace.exe (the interpreter, with the
monkey pad) to build/trace/KEY: report.txt, cov.bin, PPM frames and a
contact sheet.  What the cartridge is comes from gbkit/cart.py.  One line
per game, and build/survey.csv with every column.

The columns: the model (dmg, dual, cgb) and the mapper; ROM KB; code bytes
run; ROM banks code ran from; indirect jumps (jp hl) seen; returns not to
where their call was; returns with no call (push + ret); undefined opcodes;
bank switches, and of the bank the code runs in; code run from RAM (bytes;
h = HRAM only, the OAM DMA routine's place); self-modifying writes;
interrupts and HALTs a frame; then Pocketshift's: double speed, HDMA (general
/ HBlank), CGB palettes drawn with (BG+OBJ), tiles written (VRAM bank 0 / 1),
frames with the window, raster writes (SCX SCY LCDC WX WY palettes in lines
0-143), STAT interrupts a frame, the most sprites on a line (more than 8:
the Game Gear's limit), frames with 8x16 sprites, VRAM and I/O writes
through a computed address (of all), echo RAM accesses.
"""
import argparse
import concurrent.futures
import csv
import glob
import os
import subprocess
import sys

from .. import cart as cartmod

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACER = os.path.join(ROOT, 'tracer', 'gbtrace.exe')


def roms_in(romdir):
    return sorted(p for p in glob.glob(os.path.join(romdir, '**', '*'), recursive=True)
                  if p.lower().endswith(cartmod.EXTS))


def parse(rep):
    r = {}
    for line in rep.splitlines():
        if line.startswith('edge '):
            k = line.split()[1]
            r['edges_' + k] = r.get('edges_' + k, 0) + 1
            continue
        toks = line.split()
        if not toks or toks[0] == 'rom':
            continue
        if toks[0] == 'where':
            r.setdefault('where', []).append(dict(win=int(toks[2]), bank=int(toks[4], 16), insns=int(toks[6])))
            continue
        if toks[0] in ('ramcode', 'site'):
            continue
        if toks[0] == 'bank':
            r.setdefault('banks', []).append(dict(zip(toks[2::2], toks[3::2])) | {'n': int(toks[1], 16)})
            continue
        if toks[0] in ('irqs', 'rd', 'wr', 'irq_hits', 'ramexec', 'raster', 'undefined_ops'):
            r[toks[0]] = ' '.join(toks[1:])
            continue
        if toks[0] == 'survey':
            toks = toks[1:]
        for k, v in zip(toks[::2], toks[1::2]):
            r[k] = v
    return r


def trace(rom, out, frames, snaps=None):
    c = cartmod.Cart.load(rom)
    os.makedirs(out, exist_ok=True)
    cmd = [TRACER, rom, '--trace', out, '--out', out, '--frames', str(frames), '--quiet', *cartmod.run_flags(c)]
    if snaps:
        cmd += ['--snap', snaps]
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    return c, p


def one(rom, frames, snaps, build):
    k = cartmod.Cart.load(rom).key
    out = os.path.join(build, 'trace', k)
    c, p = trace(rom, out, frames, snaps)
    rp = os.path.join(out, 'report.txt')
    rep = open(rp, encoding='utf8', errors='replace').read() if os.path.exists(rp) else ''
    shots = [os.path.join(out, f'f{int(s):05d}.ppm') for s in snaps.split(',')]
    shots = [s for s in shots if os.path.exists(s)]
    if shots:
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'ppm2png.py'),
                        os.path.join(out, 'sheet.png'), *shots, '--cols', '2'])
    return k, c, parse(rep), p.stderr.strip()


COLS = ['key', 'title', 'model', 'mapper', 'rom_kb', 'code', 'banks_run', 'ind', 'cret', 'pret', 'und', 'bank_change',
        'bank_own', 'ramexec', 'hram_only', 'smc', 'irq_f', 'halt_f', 'double_speed', 'hdma_general', 'hdma_hblank',
        'cgb_pals', 'tiles0', 'tiles1', 'frames_window', 'raster', 'stat_f', 'max_spr', 'lines_over8', 'frames_8x16',
        'vram_ptr', 'vram_w', 'io_ptr', 'io_w', 'echo', 'frames_drawn', 'jammed', 'err']


def row(k, c, r, err):
    fr = max(1, int(r.get('frames', 1)))
    code = sum(int(x['code_bytes']) for x in r.get('banks', []))
    rb = len({x['n'] for x in r.get('banks', []) if int(x['code_bytes'])})
    ram = int(r.get('ramexec_bytes', 0))
    return dict(
        key=k, title=c.title, model=c.model_class, mapper=c.mapper + ('+RTC' if c.rtc else ''), rom_kb=c.size // 1024,
        code=code, banks_run=rb, ind=r.get('edges_i', 0), cret=r.get('edges_r', 0) + r.get('edges_a', 0),
        pret=r.get('edges_p', 0), und=r.get('undefined', '?'), bank_change=r.get('bank_change', '?'),
        bank_own=r.get('bank_own', '?'), ramexec=ram, hram_only=int(ram > 0 and ram == int(r.get('hram_bytes', 0))),
        smc=r.get('smc_writes', '?'), irq_f=round(int(r.get('irq', 0)) / fr, 2), halt_f=round(int(r.get('halt', 0)) / fr, 2),
        double_speed=r.get('double_speed', '?'), hdma_general=r.get('hdma_general', '?'),
        hdma_hblank=r.get('hdma_hblank', '?'),
        cgb_pals=f'{r.get("cgb_bpals", 0)}+{r.get("cgb_opals", 0)}', tiles0=r.get('tiles_bank0', '?'),
        tiles1=r.get('tiles_bank1', '?'), frames_window=r.get('frames_window', '?'), raster=r.get('raster_writes', '?'),
        stat_f=r.get('stat_irq_per_frame', '?'), max_spr=r.get('max_sprites_line', '?'),
        lines_over8=r.get('lines_over8', '?'), frames_8x16=r.get('frames_8x16', '?'),
        vram_ptr=r.get('vram_w_ptr', '?'), vram_w=int(r.get('vram_w_ptr', 0)) + int(r.get('vram_w_fixed', 0)),
        io_ptr=r.get('io_w_ptr', '?'), io_w=int(r.get('io_w_ptr', 0)) + int(r.get('io_w_fixed', 0)),
        echo=r.get('echo', '?'), frames_drawn=r.get('frames_drawn', '?'), jammed=r.get('jammed', '?'), err=err[:120])


def main(argv=None):
    ap = argparse.ArgumentParser(prog='gbkit.tracer.survey')
    ap.add_argument('romdir')
    ap.add_argument('--frames', type=int, default=6000)
    ap.add_argument('--jobs', type=int, default=14)
    ap.add_argument('--only')
    ap.add_argument('--snap', default='300,1500,3000,5999')
    ap.add_argument('--build-dir', default=os.path.join(ROOT, 'build'))
    ap.add_argument('--csv', default=None)
    a = ap.parse_args(argv)
    roms = roms_in(a.romdir)
    if a.only:
        roms = [r for r in roms if a.only.lower() in r.lower()]
    print(f'{"game":22} {"mdl":4} {"mapper":8} {"KB":>4} {"code":>6} {"bk":>3} {"ind":>3} {"cret":>4} {"pret":>4} '
          f'{"und":>3} {"bank":>6} {"own":>3} {"ram":>5} {"smc":>4} {"irq/f":>5} {"hlt/f":>5} | {"ds":>2} {"hdma":>9} '
          f'{"pals":>5} {"tiles":>7} {"win":>4} {"rast":>6} {"st/f":>5} {"spr":>3} {"8x16":>4} {"vptr%":>5} {"ioptr%":>6} '
          f'{"echo":>5} jam')
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for k, c, r, err in ex.map(lambda p: one(p, a.frames, a.snap, a.build_dir), roms):
            if not r:
                print(f'{k[:22]:22} FAILED {err[:100]}', flush=True)
                rows.append(dict(key=k, title=c.title, model=c.model_class, mapper=c.mapper, err=err[:120] or 'no report'))
                continue
            d = row(k, c, r, err)
            rows.append(d)
            pct = lambda x, n: f'{100 * int(x) / n:.0f}' if str(x).isdigit() and n else '-'
            print(f'{k[:22]:22} {d["model"]:4} {d["mapper"][:8]:8} {d["rom_kb"]:4d} {d["code"]:6d} {d["banks_run"]:3d} '
                  f'{d["ind"]:3d} {d["cret"]:4d} {d["pret"]:4d} {d["und"]:>3} {d["bank_change"]:>6} {d["bank_own"]:>3} '
                  f'{d["ramexec"]:5d}{"h" if d["hram_only"] else " "}{d["smc"]:>4} {d["irq_f"]:5.1f} {d["halt_f"]:5.1f} | '
                  f'{d["double_speed"]:>2} {d["hdma_general"]:>4}/{d["hdma_hblank"]:<4} {d["cgb_pals"]:>5} '
                  f'{d["tiles0"]:>3}/{d["tiles1"]:<3} {d["frames_window"]:>4} {d["raster"]:>6} {d["stat_f"]:>5} '
                  f'{d["max_spr"]:>3} {d["frames_8x16"]:>4} {pct(d["vram_ptr"], d["vram_w"]):>5} '
                  f'{pct(d["io_ptr"], d["io_w"]):>6} {d["echo"]:>5} {d["jammed"]}' + (f'  {err[:80]}' if err else ''),
                  flush=True)
    path = a.csv or os.path.join(a.build_dir, 'survey.csv' if not a.only else 'survey_only.csv')
    with open(path, 'w', newline='', encoding='utf8') as f:
        w = csv.DictWriter(f, COLS)
        w.writeheader()
        for d in rows:
            w.writerow({k: d.get(k, '') for k in COLS})
    print(f'{len(rows)} games -> {path}')


if __name__ == '__main__':
    sys.exit(main())
