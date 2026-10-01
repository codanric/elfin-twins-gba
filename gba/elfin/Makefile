TARGET        := elfin-twins
GAME_TITLE    := ELFIN TWINS
GAME_CODE     := 2EFE
MAKER_CODE    := 00

COMMON        := ../common
CORE          := ../core
SOURCES       := source/main.c source/save.c source/assets.c source/emu.c \
                 $(COMMON)/rtc.c $(COMMON)/gba_link.c $(CORE)/elfin_catchup.c
IWRAM_SOURCES := source/lcd.c $(CORE)/splb20.c
INCLUDES      := source $(COMMON) $(CORE)

all: $(TARGET).gba

# Regenerate the artwork/ROM tables (needs python3 + cairosvg, pillow, numpy)
assets:
	python3 ../tools/make_assets.py

include ../tools/gba.mk
