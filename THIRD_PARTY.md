# Sources

gbkit is MIT-licensed (`LICENSE`). It contains one piece of third-party
data: MAME's software lists for the Game Boy and Game Boy Color cartridges
(`hash/gameboy.xml`, `hash/gbcolor.xml`, CC0-1.0), which `cart.py` uses to
name each dump and to know its mapper, its RAM, battery and clock.

The hardware the runtime models -- the SM83's instruction set, its cycles
(4 per memory access, charged after it, and the internal delays where MAME
puts them), HALT, EI's delay and the interrupt dispatch; the memory map, the
mappers (MBC1, MBC2, MBC3 and its clock, MBC5, HuC1, HuC3's banks); the
timer and its quirks (DIV writes, TIMA's reload, TAC changes), the serial
port, the joypad; the LCD's modes and their timing, STAT and LY=LYC, the
window, sprites and their priorities, the OAM DMA, the CGB's VRAM and WRAM
banks, palettes, HDMA and double speed; the four sound channels -- follows
the behaviour of MAME's Game Boy driver and devices
(`src/mame/nintendo/gb.cpp`, `src/devices/cpu/lr35902/` by Wilbert Pol and
the authors its header names, `src/devices/video/gb_lcd.cpp`,
`src/devices/sound/gb.cpp`, `src/devices/bus/gameboy/`; BSD-3-Clause), read
as documentation, with the Pan Docs (https://gbdev.io/pandocs/) as the
second reference: the interpreter, the runtime and the synthesizer are
written independently (the LCD draws a line at a time, the sound runs on
demand to the tick).

Where gbkit departs from MAME on purpose:

- the HALT bug is the hardware's: only when an interrupt is already
  pending as HALT runs with IME clear (MAME's DMG core reads the next byte
  twice at every exit from HALT with IME clear);
- the CGB's speed switch pauses the CPU for 2050 M-cycles (Pan Docs; MAME's
  is about 1.4 seconds), both ways;
- a general HDMA leaves HDMA5 at $FF (MAME's decrements it once more);
- the LCD's timing follows MAME's first engine (its state machine's
  durations) in a state machine of gbkit's own; a line is drawn whole as
  mode 3 begins;
- MBC7 (Kirby Tilt 'n' Tumble's accelerometer and EEPROM) and the HuC3's
  clock and infrared are not covered.

The state the boot ROM leaves (the boot ROM is not run): the registers
the Pan Docs give for each model (A = $01 on the DMG, $11 on the CGB -- what
games read to tell the CGB), SP = $FFFE, PC = $0100, the LCD on.

Test ROMs, used locally as the interpreter's regression battery
(`tools/testroms.py`) and not distributed: blargg's Game Boy test ROMs
(cpu_instrs, instr_timing, mem_timing, halt_bug, interrupt_time, dmg_sound,
cgb_sound, oam_bug), Gekkio's mooneye test suite (MIT), Matt Currie's
dmg-acid2 and cgb-acid2 (MIT).

The recompiler and the runtime's structure come from wskit, tg16kit,
7800kit and watarakit (https://github.com/vs-sr-dev/wskit, .../tg16kit,
.../7800kit, .../watarakit, same author, MIT).
