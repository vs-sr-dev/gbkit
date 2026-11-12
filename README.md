# gbkit

Nintendo Game Boy and Game Boy Color (1989-2001) reverse engineering and
static recompilation: the cartridge and its mapper, the Sharp SM83 (as
MAME's lr35902 core runs it), a tracer that measures what a recompiler
faces in every game, an SM83-to-C++ recompiler, and a runtime that replaces
the console -- LCD, sound, timer, DMA, save memory and clock -- under the
recompiled code.

The sibling of [wskit](https://github.com/vs-sr-dev/wskit),
[tg16kit](https://github.com/vs-sr-dev/tg16kit),
[7800kit](https://github.com/vs-sr-dev/7800kit) and
[watarakit](https://github.com/vs-sr-dev/watarakit): the recompiler grew
out of theirs, rebuilt for the Game Boy's banked ROM (code is valid only
under the bank it was found in, tg16kit's rule), for a CPU whose every
memory access is a timed event, and for routines that return past their
inline arguments. One machine for both models: the CGB is a parameter.
It is also the front end of a future translator, *Pocketshift* (Game Boy
to Game Gear), which will import gbkit's discovery; the survey records
from the start the columns that one needs.

Status: the static recompiler runs the whole library. All 663 cartridges
that MAME's software lists know and the set holds (413 Game Boy, 250 Game
Boy Color; in `Game Boy/`, not distributed) are recompiled to C++, built,
and run 6000 frames identical to the interpreter (lockstep: the whole
machine -- CPU, RAM, VRAM, OAM, I/O, the LCD's state and picture, save
memory, the clock, the sound registers -- compared at every frame). The
code games copy to RAM is compiled too -- the OAM DMA routine in HRAM,
the routines in WRAM, those that patch their own operands -- under a
guard on its bytes: 551 of the 663 never touch the interpreter in 6000
frames, and the whole library runs 90 million instructions interpreted
where it ran 609 million. What is left is code games generate as they
run (the opcodes change: Mat Hoffman's Pro BMX, Alone in the Dark - The
New Nightmare, Earthworm Jim, Test Drive Le Mans, the Gex games, at 2-19
million each) and a few routines the monkey's runs never reached
natively. Two cartridges run without what they need from their mapper
(Kirby Tilt 'n' Tumble's MBC7, Robopon's HuC3 clock): identical, but not
playable yet.

## Tools

```sh
python -m gbkit.play "tetris"                                 # find, recompile if needed, play in a window
python -m gbkit.cart "gbkit/Game Boy/*/*" --table             # what each cart is (MAME's lists, by CRC)
python -m gbkit.cart "gbkit/Game Boy/*/*" --summary           # the library: models, mappers, sizes
python -m gbkit.cart --collect SETDIR "gbkit/Game Boy"        # the listed dumps of a set, one each
python -m gbkit.sm83 ROM.gb --entry -n 40                     # a listing from $0100
python -m gbkit.sm83 ROM.gb --at 6:4846 -n 20                 # bank:address

python -m gbkit.tracer.build                                  # tracer/gbtrace.exe (msys64 ucrt64 g++)
tracer/gbtrace.exe ROM.gb --trace OUTDIR --frames 6000 --snap 300,1500
tracer/gbtrace.exe ROM.gb --window                            # just play it, interpreted
python -m gbkit.tracer.survey "gbkit/Game Boy" --jobs 14      # every cart, one line each, and survey.csv
python tools/mosaic.py build/trace f03000.ppm build/mosaic    # one frame of every game
python tools/strings.py build/trace "Game Boy"                # text in the bytes no game reads
python tools/testroms.py testroms                             # the interpreter against the test ROMs
python -m gbkit.recomp.export ROM.gb                          # discovery as JSON (Pocketshift's input)
python -m gbkit.recomp.export --all "gbkit/Game Boy"          # ... every cart, to build/export/KEY.json
```

The set (from the Internet Archive, not distributed) is 1033 files: 545
`.gb` and 488 `.gbc` -- the extension does not say the model, the byte at
`$0143` does -- with translations, hacks, beta and homebrew among them.
`cart.py --collect` keeps what MAME's software lists (`hash/`, CC0) know by
CRC: 732 files, 663 distinct cartridges (413 in the `gameboy` list, 250 in
`gbcolor`; the rest are two names for one dump). Hacks, fan translations,
unlisted betas and homebrew do not count. **The set is not the whole
library**: the lists hold 1123 Game Boy and 1119 Game Boy Color families,
and the set covers 412 and 240 of them -- nearly all western releases
(375 of the 613 Game Boy families with a US or European release, 233 of
597 Color ones), next to nothing only released in Japan. The status above
counts the cartridges MAME's lists know and the set holds; `cart.py` and
the tools take any set.

What the 663 are: 412 DMG games, 86 dual-mode (they run in colour on a
CGB, and so here), 165 CGB-only; 135 with Super Game Boy features. MBC1
369, MBC5 241, plain ROM 28, MBC2 13, MBC3 9 (6 with the clock: the three
second-generation Pokémon, Harvest Moon GBC...), HuC1 1 (Super B-Daman),
HuC3 1 (Robopon), MBC7 1 (Kirby Tilt 'n' Tumble, its accelerometer). From
32 KB to 4 MB. MAME marks three of them as not working (Double Dragon 3,
Dragon Ball Z - Legendary Super Warriors, Pinball Fantasies) and five as
partly.

`gbtrace` is the runtime built with no native code: a Game Boy (MAME's gb
driver as the hardware reference, the Pan Docs as the second) with a
scripted pad. With `--trace` it writes `report.txt` -- coverage by 16 KB
bank, the banks each window ran code with, JP HL targets, calls whose
caller goes on elsewhere (an RST's inline arguments skipped), returns no
call pushed, interrupt entries, code run from RAM and self-modifying writes
to it, bank switches (and of the bank the code runs in), HALT, the ports
read and written, and the survey's columns -- `cov.bin` (a flag per ROM
byte: opcode, operand, read as data, jump target, return address) and PPM
frames.

The boot ROM is not run (it is not distributed, nor needed): the CPU starts
at `$0100` with the registers the Pan Docs give for each model -- A = $01
on a DMG, $11 on a CGB, which is how a game tells the machine -- SP =
$FFFE, the LCD on.

## The recompiler

```sh
python -m gbkit.recomp.discover ROM.gb --trace build/trace/GAME --report    # regions
python -m gbkit.recomp ROM.gb --trace build/trace/GAME --out build/recomp/GAME --build
build/recomp/GAME/gb.exe ROM.gb --lockstep --frames 6000   # native vs interpreter, every frame
build/recomp/GAME/gb.exe ROM.gb --window --scale 3         # play: arrows, X = A, Z = B,
                                                           # Enter = Start, Backspace = Select
build/recomp/GAME/gb.exe ROM.gb --frames 1800 --wav out.wav
build/recomp/GAME/gb.exe ROM.gb --window --record pad.bin  # F12: a screenshot named by its frame
build/recomp/GAME/gb.exe ROM.gb --replay pad.bin --snap N  # the same game again, headless, bit for bit
build/recomp/GAME/gb.exe ROM.gb --state --profile          # the LCD's registers; where the interpreter ran
python -m gbkit.recomp.all "gbkit/Game Boy" --jobs 8       # every cart: trace, recompile, build, lockstep
python -m gbkit.recomp.all "gbkit/Game Boy" --native       # ... again, those not yet free of the interpreter
```

- `recomp/discover.py` -- code by *region*: one of the two ROM windows
  (`$0000-$3FFF`, `$4000-$7FFF`) with one ROM bank mapped in it. The same
  address holds a different program in every bank, so a region is valid
  only under its bank; the runtime looks a region up by the window and the
  bank mapped now. Entries: the reset, and from a trace every jump, call,
  interrupt and computed-return target, under every bank its window ran
  with. A region is also registered at every jump target inside it and
  every address a call returns to, so that the interpreter, carrying on
  after native code has unwound, hands back at once. A walk stops at RET,
  at an undefined opcode, at the end of the window.
- Code in RAM -- the OAM DMA routine nearly every game copies to HRAM, the
  routines some copy to WRAM -- is one more region, made of the bytes the
  trace saw each instruction there run with (the tracer keeps up to four
  versions of each). The instructions that always ran with the same bytes
  are compiled as they are; those whose versions differ only in their
  operands (code that patches its own constants and jump targets) read
  their operands from RAM as they run, and a patched jump or call goes to
  the targets the trace saw, through a switch. Each routine (the
  instructions joined by control flow) is valid while RAM holds the bytes
  it was compiled from: a guard compares them before the runtime enters
  it, at the slow polls, after a call and after a write that may reach
  them or SVBK -- and the interpreter runs it otherwise. Code whose
  opcodes change as it runs (generated code) stays interpreted.
- Beyond the trace, two ways to more code (`recomp/farcalls.py`,
  `recomp/tables.py`). Calls into a bank resolved without running the
  game: the routines of bank 0 are run symbolically for what they leave
  -- the value they wrote to the mapper and how they leave (a return, or
  JP HL to an address made of their inputs or of the bytes after their
  call) -- so `ld b, BANK / ld hl, Target / call Bankswitch`, `ld a, BANK /
  ld [$2000], a / call Target`, a routine that switches and returns, and an
  RST with its arguments inline all come out as an entry, by the constants
  the registers hold at the call. And pointer tables completed from the
  entries the trace saw: the table holding two or more traced targets of
  one JP HL (2-byte addresses) or of the calls into a bank (3-byte bank
  and address records, pret's Predef), grown both ways while each entry
  points at code that decodes.
- Discovery checked against a disassembly: with pret's pokered built
  locally (`make compare`: identical to the set's Pokémon Red) and
  `discover --sym pokered.sym --src DIR`, each label is code or data by the
  file that defines it. Of the 28,789 instructions discovered, none starts
  inside data; they reach 982 of the 2,549 code labels -- 641 from the
  monkey's trace alone, 777 with the calls into a bank resolved, the rest
  from the pointer tables. Static discovery from the reset without a trace
  stays at 15: the game reaches nearly everything through tables indexed
  at run time.
- `recomp/emit.py` -- SM83 to C++ through the interpreter's own helpers
  (`runtime/gb.h`), every access in the interpreter's order and every
  charge where it makes it: the fetches first (4 cycles each, one charge
  each -- the timer and the serial clock see each), then the instruction's
  accesses and internal delays. CALL and RST as a C call with the return
  checked (`c.resume`, SP), a return to one of the traced places the caller
  goes on at instead -- the routines that read their arguments after the
  RST and return past them -- as a goto; RET and RETI return; JP HL a switch
  over the traced targets. After a write that may reach the mapper, and
  after a call, a region checks its bank is still mapped. A region becomes
  several C++ functions of at most 2000 instructions, cut where the fewest
  jumps cross, a jump between them a continuation.
- `runtime/` -- the machine (`gb.h`, `gb_core.cpp`): the bus in 4 KB pages
  (direct where memory has no side effects), the mappers -- MBC1, MBC2,
  MBC3 and its clock (run by the machine's time, not the PC's, so the two
  machines of a lockstep agree, and Pokémon Gold, Silver and Crystal with
  them), MBC5, HuC1, HuC3's banks -- the timer and serial port as MAME's
  driver runs them (a callback at every CPU charge), the joypad, interrupts,
  HALT and STOP, an interpreter in two forms -- flat (the reference) and
  nested (the fallback for whatever is not native, handing back to native
  code at every native entry) -- the tracer (`gb_trace.cpp`), and the
  executable (`gb_main.cpp`: headless, `--interp`, `--lockstep`, `--trace`,
  `--window` with SDL2). `gb_video.cpp`: the LCD's modes (MAME's timing),
  STAT and LY=LYC, the window and its line counter, ten sprites a line and
  their priorities (by X on the DMG, by OAM index on the CGB), the OAM DMA,
  the CGB's VRAM bank, attributes, palettes and HDMA, a line at a time.
  `gb_sound.cpp`: two pulse channels (sweep on the first), the wave
  channel, noise, the frame sequencer, run on demand to the tick.
- Time is ticks of 8.388608 MHz: a dot is 2, a CPU cycle 2 at normal speed
  and 1 in double speed; a line is 456 dots, a frame 154 lines (59.73 Hz).
  The PPU, the DMAs and the sound are brought up to the present when a
  register of theirs is touched, and at the first instruction boundary
  past their next event, by the interpreter and the native code alike.

## Discovery as JSON (for Pocketshift)

`recomp/export.py` writes what discovery and the traces know of a
cartridge to one JSON file (`build/export/KEY.json`; the format's details
in the module's docstring) -- the same discovery the recompiler emits C++
from, so what it calls code is what the lockstep holds identical:

- the regions (a window and a ROM bank, or RAM), their entries and how
  each was found -- `reset`, `trace`, `walk` (a call or jump of code
  already found), `table`, `far` (a call into a bank resolved by its
  constants) -- their other ways in, their code as runs of instructions,
  the JP HL sites with the targets the trace saw, the calls whose caller
  goes on past inline arguments, where a walk stopped;
- the pointer tables completed, by bank, address and entries;
- the code in RAM: the routines compiled (the OAM DMA routine marked), the
  bytes they hold, and the addresses whose bytes changed between runs;
- the *sites*: every instruction that reads or writes VRAM, OAM, the I/O
  registers, the mapper, the cartridge's RAM, the echo RAM or IE, or
  memory through a computed address -- its mode (a fixed address, HL, BC,
  DE, `$FF00+C`, or SP), and what the trace saw it reach, I/O register by
  register. A computed access never traced says so.

Over the library (112 MB for the 663): code in RAM in 658 games, the OAM
DMA routine recognised in 644; 3,888 pointer tables completed in 574; a
median of 795 sites a game, 59 of them reaching VRAM, OAM or I/O through
a computed address (Tecmo Bowl the most: 6,046); 22 games point the stack
at the hardware or the cartridge's RAM (Pokémon Red pops its data from
there).

## The machine, as MAME has it (and so as gbkit does)

| | Game Boy (DMG) | Game Boy Color (CGB) |
|---|---|---|
| CPU | Sharp SM83 @ 4.194304 MHz | the same, and 8.388608 MHz in double speed (KEY1 + STOP) |
| RAM | 8 KB WRAM, 8 KB VRAM, 127 bytes of HRAM | 32 KB WRAM (7 banks at `$D000`), 16 KB VRAM (2 banks) |
| Video | 160x144, 4 shades, 40 sprites (10 a line), window | + 8 BG and 8 OBJ palettes of 4 colours of 15 bits, tile attributes, HDMA |
| Sound | 2 pulse channels, a wave channel, noise | the same |
| Cart | up to 8 MB (MBC5), banks of 16 KB, RAM to 128 KB, clock (MBC3) | the same |
| Interrupts | VBlank, STAT, timer, serial, joypad; vectors `$40-$60` | the same |

MAME's lr35902 core is followed even where it is odd, since the lockstep
holds the recompiled code to the interpreter and the interpreter to MAME:
every memory access costs 4 cycles charged after it, CALL and PUSH spend
their internal cycle before writing, an interrupt spends 12 cycles before
pushing PC (the low byte first); HALT prefetches the next byte and then
burns 2 cycles a time on a DMG, 4 on a CGB, until an interrupt is pending,
with MAME's delays to leave it; STOP reads its second byte and does nothing
but the CGB's speed switch; the undefined opcodes are one-byte no-ops; the
timer is MAME's -- writing DIV can tick TIMA (even with the timer off),
TIMA reloads a step after it overflows, TAC changes tick it as MAME
decides; the joypad register's value is made when it is written. Where
gbkit departs from MAME on purpose (the HALT bug, the speed switch's pause,
HDMA5 after a general DMA) is listed in `THIRD_PARTY.md`.

## The test ROMs

`tools/testroms.py` runs the public test ROMs (not distributed) against the
interpreter, each read the way it reports: blargg's (the serial port,
`$A000`, or the text on the screen -- the BG map holds a tile per ASCII
code), Gekkio's mooneye suite (the registers' Fibonacci signature), Matt
Currie's dmg-acid2 and cgb-acid2 (the picture against the reference).
Now: both acid2 pictures exact; mooneye 85 of the 94 tests for a DMG or a
CGB -- all the CPU timing, timer, OAM DMA, MBC1, MBC2 and MBC5 ones, the
registers and the serial clock as the boot ROM leaves them; not yet
`ei_sequence`, `ie_push`, `halt_ime0_nointr_timing`, the LCD-on, HBlank and
STAT-LYC edge timings (gbkit follows MAME's there), the MBC1 multicart.
Four more are for a CGB running a DMG cartridge in its compatibility mode,
which gbkit does not do (a DMG game runs as a DMG): skipped. blargg 39 of
58 -- `cpu_instrs`, `instr_timing`, `mem_timing` (both), `halt_bug` and
`interrupt_time` pass, the sound tests half (the APU is for the output
only) and the DMG's OAM corruption tests not.

## Survey of the library (6000 frames each, monkey pad)

- Every cartridge boots and draws; the mosaics of frames 300, 1500, 3000
  and 5999 show all but one in play or in a menu (a monkey that presses
  Start early, A and B often, the pad at random). Robopon - Sun Version is
  white throughout: the HuC3's clock and infrared, which it waits on, are
  not covered (MAME runs it partly too). Centipede fades to white after
  its copyright screen and waits there with the link flag set, for keys
  that come over the serial cable (the monkey's doing, not understood
  yet).
- Code actually run: 1.2-70 KB per game (median 12.7 KB) -- Towers: Lord
  Baniff's Deceit the most -- from 5 ROM banks (median; up to 44).
- **No game runs an undefined opcode.** 615 use JP HL (jump tables, often
  behind an RST); 166 have calls whose caller goes on elsewhere (an RST
  with inline arguments, read and skipped by the routine); 414 return
  where no call pushed (push + ret).
- Bank switching in 635 games; of the bank the code runs in, ten: Super
  Chinese Fighter GB, both Bomberman Max, Hamtaro, Mario Golf, both Monster
  Rancher, the Pokémon Trading Card Game, SD Hiryuu no Ken EX, Super Robot
  Pinball.
- Code in RAM in 658 games: in 447 only the OAM DMA routine in HRAM (10-18
  bytes), in 211 more, up to 5.5 KB (Test Drive Le Mans, Alone in the Dark:
  The New Nightmare); written to after it ran in 264 (self-modifying code:
  Mat Hoffman's Pro BMX the most, four million writes in 6000 frames). Five games run nothing from RAM, not
  even the DMA routine: Wild Snake, Hamtaro, Lufia - The Legend Returns,
  Razor Freestyle Scooter, Tomb Raider - Curse of the Sword.
- Interrupts: about two a frame (median); STAT interrupts more than one a
  frame in 193 games (NBA Jam 2001: 144, an HBlank interrupt every line);
  HALT in 396.

What Pocketshift (Game Boy to Game Gear) will face, by the same runs
(`build/survey.csv` has every column per game):

- double speed in 207 of the 251 colour games; HDMA in 130 (general) and
  47 (HBlank); the CGB palettes drawn with, per game;
- more than 8 sprites on a line (the Game Gear's limit) in 408 games, more
  than 10 wanted (the Game Boy drops the rest) in 204; 8x16 sprites in 346;
- the window in 407 games; scroll, LCDC, window or palette registers
  written while lines 0-143 are drawn in 646 (raster effects, and the
  writes in HBlank that make them);
- VRAM written through a computed address in practically every game (the
  median write is through HL or a register pair); I/O registers written
  through a computed address (`ld [c], a`, `(hl)`) in 656 games -- the
  sound drivers above all -- for 19 % of all I/O writes (median);
- the echo RAM (`$E000-$FDFF`) in 40 games.

## Notes from the library

Text in the bytes no run reads (`tools/strings.py`):

- **Debuggers left in.** Dr. Franken II carries a whole `PDS Z80 MONITOR`
  -- the Programmers Development System's on-cartridge monitor -- with its
  command help (`A [s,e[,w]] - Analyze program`, `T [s] - Enter tracer`,
  `U s,l[,p] - Upload memory`, `Z n - Select ba[nk]`). Ultima - Runes of
  Virtue II keeps a `GAMEBOY MONITOR` and the register names it shows
  (`PC SP A B C D E H L`); All-Star Baseball '99 names a `Game Boy
  Emulator/Debugger`.
- **Test modes.** Kamen Rider SD - Hashire! Mighty Riders: `2P FRAME TEST`,
  `2P RANDOM TEST`; Lucle: `VISUAL TEST` next to its option menus; Madden
  '95 and '96: a `SOUND EFFECTS MENU`; Mega Man IV signs a bank `Program
  bank 1F IMZY`.
- **The machines that built them.** Indiana Jones and the Infernal
  Machine keeps a C function's name in an error string, `ERROR :
  SFUN_Pickup()`; Road Rash (the Color one) a Windows registry key,
  `AppEvents\Schemes\Apps\.Default\TRACKEDITORMenuCommand\.Current` -- the
  memory of the PC that ran its track editor, left in the padding.
- **A shared sound driver.** Dexter's Laboratory - Robot Rampage and Sgt.
  Rock - On the Frontline open four banks each with `SOUND DRIVER Ver.2.0
  PROGRAM T.SAKAI ADVICER JAMI CONVERTER K.TAKAHASHI`.
- **Credits nobody sees.** Lemmings: `PLAYTESTED BY ANTHONY BARLOW.....A
  LEMMING MASTER BY FAR!!`; Mole Mania dates its sound data, `1996/4/19 PM
  5:30 ... SOUND PROGRAM & MUSIC & SOUND EFFECTS ALL DATA WERE WRITTEN BY
  TARO BANDO`; Sports Illustrated - Golf Classic: `Programmed by: Wayne A.
  Shirk`; Speedy Gonzales thanks its `GREAT GAME DEBUGGERS`.

What the runs showed:

- **Azure Dreams** (dual-mode, MBC5, 1 MB), a roguelike whose dungeons
  are made anew each visit, so the monkey reaches little of it: 12.5 KB of
  code from 15 banks, 170 calls whose caller goes on past inline
  arguments, 62 returns used as jumps, 263 bytes run from RAM beyond the
  DMA routine (6.7 million instructions in 6000 frames, all compiled now:
  it runs without the interpreter), double speed; identical in lockstep
  all the same. A recorded
  session (`--record`, then `--trace` with `--replay`) gives discovery the
  floors the monkey does not reach.
- **The clock.** Pokémon Gold, Silver and Crystal ask the time of day at
  a new game and keep it in the MBC3's clock; run on the machine's time,
  not the PC's, the clock is the same in both machines of the lockstep.

## Not covered (yet)

- The Super Game Boy: a game sends its packets through the joypad register
  and gets the answer of a Game Boy with no SGB.
- MBC7 (Kirby Tilt 'n' Tumble: the accelerometer and the EEPROM), the
  HuC3's clock and infrared (Robopon), HuC1's infrared, the link cable,
  the Game Boy Printer, the camera, MMM01 and the unlicensed mappers (none
  in the set's listed games).
- The DMG's OAM corruption bug; the LCD draws a line at a time, so changes
  in the middle of a line are not seen; the sound follows MAME's and is
  still to be checked by ear.
- The CGB's colour correction, and the automatic colours its boot ROM
  gives DMG games (a DMG game runs as a DMG, in grey).
