# Credits and third-party material

## BrickEmuPy, by azya52
<https://github.com/azya52/BrickEmuPy>, released under **CC0 1.0 Universal** (public domain dedication).

This project would not exist without BrickEmuPy. azya52 reverse-engineered the Sunplus SPLB20
microcontroller, wrote its emulator, and dumped and described the Elfin Twins GM-021. From BrickEmuPy
this repository uses:

| What | Where here | Use |
|---|---|---|
| SPLB20 CPU / SFR / timer / sound emulation (`SPLB20.py`, `SPLB20Sound.py`, `SPLB20dasm.py`, `rom.py`) | `cores/` (unchanged) | reference implementation. `gba/core/splb20.c` is a line-by-line C port of it, checked instruction by instruction against it |
| The Elfin Twins ROM dump | `assets/ElfinTwins.bin` | the game itself; embedded in the GBA ROM |
| The toy's artwork with one element per LCD segment | `assets/ElfinTwins.svg` | GBA screen, figures in the documents |
| The device description | `assets/ElfinTwins.brick` | button wiring; the DOWN mask is corrected here (12 → 24, CLOCK+ENTER) |
| The idea of the LCD "motion blur" | `gba/elfin/source/lcd.c` | optional *LCD fade* setting |

CC0 asks for nothing in return. The credit is given because it's deserved.

The Elfin Twins unit that BrickEmuPy's dump came from was provided for reverse engineering by
**t.me/oksiniah** (credited in BrickEmuPy's `ElfinTwins.brick`).

## Other components
- **libtonc** by J. Vijn: MIT licence, vendored in `gba/third_party/libtonc` (see its `license.txt`).
- **Fonts** in `gba/docs/fonts` (Nunito, Fredoka, Inter, JetBrains Mono, Source Serif 4) from Google Fonts:
  SIL Open Font License 1.1 (`gba/docs/fonts/OFL.txt`).
- **mGBA** (MPL 2.0) is used through `libmgba` by the headless test runner `gba/host/mgba_run.c`. It is
  not distributed here.

## The game
*Elfin Twins GM-021*, its program (ROM), graphics, melodies and name belong to their original rights holders.
A credit hidden in the ROM names **Sparkle Crystal Development Co., Ltd.**, programmer **WangLaXian**,
16 November 1996 (see the technical reference, §8). They are not covered by this repository's licences.
This is an unofficial, non-commercial preservation and documentation project.
