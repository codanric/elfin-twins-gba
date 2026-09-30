# cores/: BrickEmuPy's SPLB20 emulator (vendored)

These files are copied unchanged from [BrickEmuPy](https://github.com/azya52/BrickEmuPy)
by **azya52**, released under **CC0 1.0 Universal** (see `LICENSE-BrickEmuPy-CC0.txt`).

`gba/core/splb20.c` is a line-by-line C port of `SPLB20.py`.
`gba/host/verify_core.py` runs both side by side and compares them after every instruction.
`gba/docs/tools/gen_tech.py` reads the cycle counts from `SPLB20.py`.
Only `__init__.py` is new: it imports just the SPLB20 core instead of all of BrickEmuPy's cores.
