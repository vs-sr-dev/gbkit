"""A Game Boy cartridge: its header, its mapper, and what the set says about it.

The SM83 sees 64 KB.  The cartridge answers two windows of it, and takes
the writes to its ROM as the mapper's registers:

    $0000-$3FFF   ROM bank 0 (fixed, but for MBC1 in mode 1 on a large cart)
    $4000-$7FFF   the switchable ROM bank
    $A000-$BFFF   the cartridge's RAM (or the MBC3's clock registers, the
                  MBC7's accelerometer and EEPROM...), when enabled

The header sits at $0100-$014F of bank 0:

    $0100  the entry point (usually NOP; JP $0150)
    $0104  the Nintendo logo (48 bytes; the boot ROM refuses a cart without it)
    $0134  the title (16 bytes on the first carts; 15 or 11 + a maker code later)
    $0143  CGB flag: bit 7 set = the CGB runs it in colour ($80 dual-mode,
           $C0 CGB-only); clear = a DMG game
    $0144  new licensee code (2 ASCII)     $0146  $03 = uses the Super Game Boy
    $0147  cartridge type (the mapper)      $0148  ROM size (32 KB << n)
    $0149  RAM size                         $014A  destination (0 = Japan)
    $014B  old licensee ($33: see $0144)    $014C  version
    $014D  header checksum ($0134-$014C)    $014E  global checksum (16 bits, big-endian)

What a dump is comes from MAME's software lists (hash/gameboy.xml,
gbcolor.xml, CC0) by CRC32: its list, short name, title, slot (the
mapper: rom, rom_mbc1, rom_mbc5...), its RAM (`nvram` when a battery keeps
it), the MBC3's clock, rumble, the SGB enhancement, and whether MAME runs
it.  A dump the lists do not know takes its mapper from the header's type
byte, as MAME's slot does for a loose file.  The key everything else uses
is `<list>_<short name>` (`gameboy_pokered`, `gbcolor_pokecrys`).

The model is the header's, not the list's nor the file's extension: a game
with bit 7 of $0143 set runs as a CGB (dual-mode and CGB-only alike, as a
player with a Game Boy Color sees it), every other one as a DMG.

    python -m gbkit.cart ROM.gb                           # what it is
    python -m gbkit.cart "Game Boy/*/*" --table
    python -m gbkit.cart "Game Boy/*/*" --summary         # the library: models, mappers, sizes
    python -m gbkit.cart --collect SETDIR "Game Boy"      # the listed dumps of a set, one each
"""

import argparse
import glob
import os
import re
import shutil
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
LISTS = ('gameboy', 'gbcolor')
EXTS = ('.gb', '.gbc', '.cgb', '.sgb')

LOGO = bytes.fromhex('CEED6666CC0D000B03730083000C000D0008111F8889000EDCCC6EE6DDDDD999'
                     'BBBB67636E0EECCCDDDC999FBBB9333E')

# the header's type byte -> (MAME slot, has RAM, battery, RTC, rumble), as gbslot.cpp guesses a loose file
TYPES = {
    0x00: ('rom', 0, 0, 0, 0), 0x08: ('rom', 1, 0, 0, 0), 0x09: ('rom', 1, 1, 0, 0),
    0x01: ('rom_mbc1', 0, 0, 0, 0), 0x02: ('rom_mbc1', 1, 0, 0, 0), 0x03: ('rom_mbc1', 1, 1, 0, 0),
    0x05: ('rom_mbc2', 1, 0, 0, 0), 0x06: ('rom_mbc2', 1, 1, 0, 0),
    0x0B: ('rom_mmm01', 0, 0, 0, 0), 0x0C: ('rom_mmm01', 1, 0, 0, 0), 0x0D: ('rom_mmm01', 1, 1, 0, 0),
    0x0F: ('rom_mbc3', 0, 1, 1, 0), 0x10: ('rom_mbc3', 1, 1, 1, 0),
    0x11: ('rom_mbc3', 0, 0, 0, 0), 0x12: ('rom_mbc3', 1, 0, 0, 0), 0x13: ('rom_mbc3', 1, 1, 0, 0),
    0x19: ('rom_mbc5', 0, 0, 0, 0), 0x1A: ('rom_mbc5', 1, 0, 0, 0), 0x1B: ('rom_mbc5', 1, 1, 0, 0),
    0x1C: ('rom_mbc5', 0, 0, 0, 1), 0x1D: ('rom_mbc5', 1, 0, 0, 1), 0x1E: ('rom_mbc5', 1, 1, 0, 1),
    0x20: ('rom_mbc6', 1, 1, 0, 0), 0x22: ('rom_mbc7_2k', 1, 1, 0, 0),
    0xEA: ('rom_yong', 0, 0, 0, 0), 0xFC: ('rom_camera', 1, 1, 0, 0), 0xFD: ('rom_tama5', 1, 1, 1, 0),
    0xFE: ('rom_huc3', 1, 1, 1, 0), 0xFF: ('rom_huc1', 1, 1, 0, 0),
}
RAMSIZE = {0: 0, 1: 0x800, 2: 0x2000, 3: 0x8000, 4: 0x20000, 5: 0x10000}
MAPPER = {'rom': 'ROM', 'rom_mbc1': 'MBC1', 'rom_mbc2': 'MBC2', 'rom_mbc3': 'MBC3', 'rom_mbc30': 'MBC30',
          'rom_mbc5': 'MBC5', 'rom_mbc6': 'MBC6', 'rom_mbc7_2k': 'MBC7', 'rom_mbc7_4k': 'MBC7',
          'rom_mmm01': 'MMM01', 'rom_huc1': 'HuC1', 'rom_huc3': 'HuC3', 'rom_tama5': 'TAMA5',
          'rom_camera': 'Camera'}
# the mappers the runtime implements (the others: "Not covered (yet)")
SUPPORTED_SLOTS = ('rom', 'rom_mbc1', 'rom_mbc2', 'rom_mbc3', 'rom_mbc30', 'rom_mbc5', 'rom_huc1', 'rom_huc3')

_lists = None


def _unxml(s):
    return s.replace('&amp;', '&').replace('&apos;', "'").replace('&quot;', '"').replace('&lt;', '<').replace('&gt;', '>')


def lists():
    """CRC32 -> dict(list, short, title, parent, slot, ram, battery, rtc, rumble, sgb, gbc, supported)
    from MAME's software lists."""
    global _lists
    if _lists is None:
        found = {}                # filled here, published whole (threads read it)
        for name in LISTS:
            x = open(os.path.join(HERE, 'hash', name + '.xml'), encoding='utf8').read()
            for sw in re.finditer(r'<software name="([^"]+)"([^>]*)>(.*?)</software>', x, re.S):
                attrs, body = sw.group(2), sw.group(3)
                feat = dict(re.findall(r'<feature name="([^"]+)" value="([^"]*)"', body))
                shared = dict(re.findall(r'<sharedfeat name="([^"]+)" value="([^"]*)"', body))
                areas = dict((n, int(s, 0)) for n, s in
                             re.findall(r'<dataarea name="([a-z]+)" size="(0x[0-9a-fA-F]+|\d+)"', body))
                parent = re.search(r'cloneof="([^"]+)"', attrs)
                sup = re.search(r'supported="([^"]+)"', attrs)
                rom = re.search(r'<dataarea name="rom".*?</dataarea>', body, re.S)
                info = dict(
                    list=name, short=sw.group(1),
                    title=_unxml(re.search(r'<description>(.*?)</description>', body).group(1)),
                    parent=parent.group(1) if parent else None,
                    slot=feat.get('slot', 'rom'),
                    ram=areas.get('nvram', areas.get('ram', 0)), battery='nvram' in areas,
                    rtc=feat.get('rtc') == 'yes', rumble=feat.get('rumble') == 'yes',
                    sgb=feat.get('enhancement') == 'sgb', gbc=shared.get('compatibility') == 'GBC',
                    supported=sup.group(1) if sup else 'yes')
                for m in re.finditer(r'<rom name="[^"]*" size="(?:0x[0-9a-fA-F]+|\d+)" crc="([0-9a-f]+)"',
                                     rom.group(0) if rom else ''):
                    found[int(m.group(1), 16)] = info
        _lists = found
    return _lists


class Cart:
    def __init__(self, data, path=''):
        self.path = path
        self.data = bytes(data)
        self.size = len(self.data)
        self.crc = zlib.crc32(self.data)
        h = self.data[0x100:0x150] if self.size >= 0x150 else bytes(0x50)
        self.header = h
        self.logo_ok = h[4:0x34] == LOGO
        self.cgb_flag, self.sgb_flag, self.type = h[0x43], h[0x46], h[0x47]
        self.romsize_code, self.ramsize_code, self.dest = h[0x48], h[0x49], h[0x4A]
        self.old_licensee, self.version = h[0x4B], h[0x4C]
        self.licensee = h[0x44:0x46].decode('latin1') if h[0x4B] == 0x33 else f'{h[0x4B]:02X}'
        self.hsum, self.gsum = h[0x4D], h[0x4E] << 8 | h[0x4F]
        # the title: 16 bytes, cut at the first NUL; the CGB flag ends it when set
        t = h[0x34:0x43] if self.cgb_flag & 0x80 else h[0x34:0x44]
        self.header_title = t.split(b'\0')[0].decode('latin1').rstrip()
        # the model: bit 7 of $0143 is what the CGB's boot ROM looks at
        self.model_class = ('cgb' if self.cgb_flag & 0x40 else 'dual') if self.cgb_flag & 0x80 else 'dmg'
        self.cgb = bool(self.cgb_flag & 0x80)
        info = lists().get(self.crc)
        self.listed = info is not None
        ty = TYPES.get(self.type, ('rom' if self.size <= 0x8000 else 'rom_mbc1', 0, 0, 0, 0))
        if info:
            self.list, self.short, self.title, self.parent = info['list'], info['short'], info['title'], info['parent']
            self.slot, self.rtc, self.rumble = info['slot'], info['rtc'], info['rumble']
            self.ram_size, self.battery = info['ram'], info['battery']
            self.sgb, self.mame_supported = info['sgb'], info['supported']
        else:
            self.list = 'gbcolor' if self.cgb else 'gameboy'
            self.short, self.title, self.parent = None, self.name, None
            self.slot, self.rtc, self.rumble = ty[0], bool(ty[3]), bool(ty[4])
            self.ram_size = 0x200 if ty[0] == 'rom_mbc2' else RAMSIZE.get(self.ramsize_code, 0) if ty[1] else 0
            self.battery = bool(ty[2])
            self.sgb, self.mame_supported = self.sgb_flag == 3, 'yes'
        self.mapper = MAPPER.get(self.slot, self.slot.replace('rom_', ''))

    @classmethod
    def load(cls, path):
        with open(path, 'rb') as f:
            return cls(f.read(), path)

    @property
    def name(self):
        return os.path.splitext(os.path.basename(self.path))[0]

    @property
    def key(self):
        if self.short:
            return f'{self.list}_{self.short}'
        return re.sub(r'[^A-Za-z0-9]+', '_', self.name).strip('_')

    @property
    def banks(self):
        """The number of 16 KB ROM banks in the dump."""
        return max(2, (self.size + 0x3FFF) >> 14)

    def header_sum(self):
        """$014D as the boot ROM checks it: x = x - byte - 1 over $0134-$014C."""
        x = 0
        for b in self.data[0x134:0x14D]:
            x = (x - b - 1) & 0xFF
        return x

    def global_sum(self):
        return (sum(self.data) - self.data[0x14E] - self.data[0x14F]) & 0xFFFF

    def bank(self, n):
        """The 16 KB of ROM bank n (masked by the dump's size, as the mappers wrap)."""
        o = (n % self.banks) << 14
        return self.data[o:o + 0x4000].ljust(0x4000, b'\xff')

    def reader(self, bank=1):
        """read(address) over $0000-$7FFF with ROM bank `bank` at $4000 (elsewhere $FF)."""
        b0, bn = self.bank(0), self.bank(bank)

        def read(a):
            a &= 0xFFFF
            return b0[a] if a < 0x4000 else bn[a - 0x4000] if a < 0x8000 else 0xFF
        return read


def run_flags(c):
    """The runtime's options for cart c: the machine, the mapper and what MAME's list says it carries."""
    f = ['--cgb' if c.cgb else '--dmg', '--mbc', c.slot, '--ram', str(c.ram_size)]
    if c.battery:
        f.append('--battery')
    if c.rtc:
        f.append('--rtc')
    return f


def collect(setdir, outdir, log=print):
    """Copy the dumps of a set that MAME's lists know into OUTDIR/<list>/,
    one per software (preferring names without flags), untouched."""
    seen = {}
    for d, _, fs in os.walk(setdir):
        if os.path.abspath(d).startswith(os.path.abspath(outdir)):
            continue
        for f in sorted(fs):
            if not f.lower().endswith(EXTS):
                continue
            p = os.path.join(d, f)
            c = Cart.load(p)
            if not c.listed:
                continue
            # prefer names without flags ([!], [a1]...) and fewer tags, then the shorter
            score = (f.count('['), f.count('('), len(f), f)
            if c.key not in seen or score < seen[c.key][0]:
                seen[c.key] = (score, p, c)
    n = 0
    for k, (_, p, c) in sorted(seen.items()):
        dst = os.path.join(outdir, c.list)
        os.makedirs(dst, exist_ok=True)
        ext = '.gbc' if c.list == 'gbcolor' else '.gb'
        shutil.copy2(p, os.path.join(dst, os.path.splitext(os.path.basename(p))[0] + ext))
        n += 1
    per = {}
    for _, _, c in seen.values():
        per[c.list] = per.get(c.list, 0) + 1
    log(f'{n} dumps copied to {outdir} (' + ', '.join(f'{k} {v}' for k, v in sorted(per.items())) + ')')
    return n


def summary(carts):
    from collections import Counter
    listed = [c for c in carts if c.listed]
    print(f'{len(carts)} dumps, {len(listed)} in the lists '
          f'({", ".join(f"{k} {v}" for k, v in sorted(Counter(c.list for c in listed).items()))})')
    for title, f in (('model', lambda c: c.model_class), ('mapper', lambda c: c.mapper + ('+RTC' if c.rtc else '')),
                     ('ROM', lambda c: f'{c.size // 1024}K'),
                     ('RAM', lambda c: f'{c.ram_size // 1024}K' + ('+batt' if c.battery else '') if c.ram_size else '-'),
                     ('MAME', lambda c: c.mame_supported)):
        cnt = Counter(f(c) for c in listed)
        print(f'  {title:7}' + '  '.join(f'{k} {v}' for k, v in sorted(cnt.items(), key=lambda kv: -kv[1])))
    print(f'  SGB    {sum(c.sgb for c in listed)}   rumble {sum(c.rumble for c in listed)}   '
          f'logo bad {sum(not c.logo_ok for c in listed)}   header sum bad {sum(c.header_sum() != c.hsum for c in listed)}')
    odd = [c for c in listed if c.mame_supported != 'yes' or c.slot not in SUPPORTED_SLOTS]
    for c in odd:
        print(f'    {c.key:24} {c.mapper:8} MAME {c.mame_supported:8} {c.title}')


def main(argv=None):
    ap = argparse.ArgumentParser(prog='gbkit.cart')
    ap.add_argument('roms', nargs='*')
    ap.add_argument('--table', action='store_true')
    ap.add_argument('--summary', action='store_true')
    ap.add_argument('--unlisted', action='store_true', help='with --table: only the dumps the lists do not know')
    ap.add_argument('--collect', nargs=2, metavar=('SETDIR', 'OUTDIR'))
    a = ap.parse_args(argv)
    if a.collect:
        collect(*a.collect)
        return 0
    paths = [p for g in a.roms for p in (sorted(glob.glob(g)) or [g])]
    paths = [p for p in paths if os.path.isfile(p) and p.lower().endswith(EXTS)]
    if a.summary:
        summary([Cart.load(p) for p in paths])
        return 0
    for p in paths:
        c = Cart.load(p)
        if a.table:
            if a.unlisted and c.listed:
                continue
            ram = f'{c.ram_size // 1024}K' if c.ram_size >= 1024 else f'{c.ram_size}B' if c.ram_size else '-'
            flags = ('B' if c.battery else '-') + ('R' if c.rtc else '-') + ('S' if c.sgb else '-') + \
                    ('U' if c.rumble else '-') + ('!' if c.header_sum() == c.hsum else '?') + \
                    ('' if c.mame_supported == 'yes' else c.mame_supported[0])
            print(f'{c.key[:24]:24} {c.model_class:4} {c.mapper:6} {c.size // 1024:5d}K {ram:>5} {flags:6} '
                  f'{c.crc:08X}  {c.title[:60]}')
            continue
        print(c.name)
        print(f'  {"listed" if c.listed else "NOT in the lists"}: {c.list}/{c.short}  {c.title!r}  crc {c.crc:08X}'
              + (f'  clone of {c.parent}' if c.parent else '') +
              ('' if c.mame_supported == 'yes' else f'  MAME: supported="{c.mame_supported}"'))
        print(f'  size {c.size} ({c.banks} banks of 16 KB)  slot {c.slot} ({c.mapper})  '
              f'RAM {c.ram_size}{" battery" if c.battery else ""}  rtc {c.rtc}  rumble {c.rumble}  sgb {c.sgb}')
        print(f'  header: title {c.header_title!r}  cgb {c.cgb_flag:02X} ({c.model_class})  sgb {c.sgb_flag:02X}  '
              f'type {c.type:02X}  rom {c.romsize_code:02X}  ram {c.ramsize_code:02X}  dest {c.dest:02X}  '
              f'licensee {c.licensee}  version {c.version:02X}')
        print(f'    entry {c.header[:4].hex(" ")}  logo {"good" if c.logo_ok else "BAD"}  '
              f'header sum {c.hsum:02X} ({"good" if c.header_sum() == c.hsum else f"computed {c.header_sum():02X}"})  '
              f'global sum {c.gsum:04X} ({"good" if c.global_sum() == c.gsum else f"computed {c.global_sum():04X}"})')


if __name__ == '__main__':
    sys.exit(main())
