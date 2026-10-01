TARGET        := elfin-linktest
GAME_TITLE    := ELFINLINKTST
GAME_CODE     := 2ELT
MAKER_CODE    := 00

COMMON        := ../common
SOURCES       := source/main.c $(COMMON)/rtc.c $(COMMON)/gba_link.c
IWRAM_SOURCES :=
INCLUDES      := source $(COMMON)

all: $(TARGET).gba

include ../tools/gba.mk
