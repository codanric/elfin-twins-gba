TARGET        := elfin-twins
GAME_TITLE    := ELFIN TWINS
GAME_CODE     := 2EFE
MAKER_CODE    := 00

COMMON        := ../common
CORE          := ../core
SOURCES       := source/main.c source/save.c source/assets.c \
                 $(COMMON)/rtc.c $(COMMON)/linkio.c $(CORE)/elfin_catchup.c \
                 $(CORE)/elfin_link.c
IWRAM_SOURCES := source/emu.c source/lcd.c $(CORE)/splb20.c
INCLUDES      := source $(COMMON) $(CORE)

all: $(TARGET).gba

# Regenerate the artwork/ROM tables (needs python3 + cairosvg, pillow, numpy)
assets:
	python3 ../tools/make_assets.py

include ../tools/gba.mk
