# ---------------------------------------------------------------------------
# Shared GBA build rules for the Elfin Twins port.
#
# Works with either toolchain:
#   * devkitARM (devkitPro):  export DEVKITARM=/opt/devkitpro/devkitARM
#       -> uses devkitARM's gcc, gba.specs (crt0 + linker script) and gbafix.
#   * any plain arm-none-eabi GCC with newlib (e.g. Ubuntu gcc-arm-none-eabi)
#       -> uses tools/crt0.s, tools/gba.ld and tools/gbafix.py from this repo.
#
# libtonc is always built from gba/third_party/libtonc so both toolchains
# produce the same code.
#
# A project Makefile sets:
#   TARGET        output name (without extension)
#   SOURCES       thumb C sources (run from ROM)
#   IWRAM_SOURCES ARM C sources placed in IWRAM (fast code)
#   ASM_SOURCES   assembly sources
#   INCLUDES      extra include dirs
#   GAME_TITLE    12 chars max, GAME_CODE 4 chars
# and then: include ../tools/gba.mk
# ---------------------------------------------------------------------------

GBA_ROOT   := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
TONC_DIR   := $(GBA_ROOT)/third_party/libtonc
BUILD      ?= build

ifneq ($(strip $(DEVKITARM)),)
  PREFIX   := $(DEVKITARM)/bin/arm-none-eabi-
  USE_DKA  := 1
else
  PREFIX   ?= arm-none-eabi-
  USE_DKA  := 0
endif

CC         := $(PREFIX)gcc
AS         := $(PREFIX)gcc
AR         := $(PREFIX)gcc-ar
OBJCOPY    := $(PREFIX)objcopy
PYTHON     ?= python3

ARCH       := -mcpu=arm7tdmi -mtune=arm7tdmi -mthumb-interwork
CBASE      := $(ARCH) -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
              -fno-strict-aliasing -ffunction-sections -fdata-sections \
              -D__GBA__ -I$(TONC_DIR)/include $(addprefix -I,$(INCLUDES))
THUMB_CFLAGS := $(CBASE) -mthumb
ARM_CFLAGS   := $(CBASE) -marm -mlong-calls
ASFLAGS    := $(ARCH) -x assembler-with-cpp -I$(TONC_DIR)/include

ifeq ($(USE_DKA),1)
  LDFLAGS  := $(ARCH) -mthumb -specs=gba.specs -Wl,--gc-sections -Wl,-Map,$(BUILD)/$(TARGET).map
  CRT0     :=
  GBAFIX   := $(DEVKITPRO)/tools/bin/gbafix
  ifeq ($(wildcard $(GBAFIX)),)
    GBAFIX := gbafix
  endif
  FIXCMD   = $(GBAFIX) $@ -t"$(GAME_TITLE)" -c"$(GAME_CODE)" -m"$(MAKER_CODE)"
else
  LDFLAGS  := $(ARCH) -mthumb -nostartfiles -T $(GBA_ROOT)/tools/gba.ld \
              -specs=nano.specs -specs=nosys.specs \
              -Wl,--gc-sections -Wl,-Map,$(BUILD)/$(TARGET).map
  CRT0     := $(BUILD)/crt0.o
  FIXCMD   = $(PYTHON) $(GBA_ROOT)/tools/gbafix.py $@ --title "$(GAME_TITLE)" --code "$(GAME_CODE)" --maker "$(MAKER_CODE)"
endif

GAME_TITLE ?= HOMEBREW
GAME_CODE  ?= 0HBE
MAKER_CODE ?= 00

# ------------------------------------------------------------------ libtonc
# tte_iohook.c needs devkitARM's newlib stdio hooks, so it is only built there.
TONC_SRC   := $(wildcard $(TONC_DIR)/src/*.c $(TONC_DIR)/src/font/*.c \
                         $(TONC_DIR)/src/tte/*.c $(TONC_DIR)/src/pre1.3/*.c)
ifeq ($(USE_DKA),0)
  TONC_SRC := $(filter-out %/tte_iohook.c,$(TONC_SRC))
endif
TONC_ASM   := $(wildcard $(TONC_DIR)/asm/*.s $(TONC_DIR)/src/font/*.s $(TONC_DIR)/src/tte/*.s)
TONC_BUILD := $(BUILD)/libtonc
TONC_OBJ   := $(patsubst $(TONC_DIR)/%.c,$(TONC_BUILD)/%.o,$(TONC_SRC)) \
              $(patsubst $(TONC_DIR)/%.s,$(TONC_BUILD)/%.o,$(TONC_ASM))
TONC_LIB   := $(TONC_BUILD)/libtonc.a

$(TONC_BUILD)/%.o: $(TONC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CBASE) -mthumb -Wno-all -Wno-extra -c $< -o $@

$(TONC_BUILD)/%.o: $(TONC_DIR)/%.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

$(TONC_LIB): $(TONC_OBJ)
	@rm -f $@
	$(AR) -crs $@ $^

# ------------------------------------------------------------------ project
OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,$(notdir $(SOURCES))) \
        $(patsubst %.c,$(BUILD)/obj/%.iwram.o,$(notdir $(IWRAM_SOURCES))) \
        $(patsubst %.s,$(BUILD)/obj/%.o,$(notdir $(ASM_SOURCES))) \
        $(EXTRA_OBJS)

vpath %.c $(sort $(dir $(SOURCES) $(IWRAM_SOURCES)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

DEPS := $(OBJS:.o=.d)

$(BUILD)/obj/%.iwram.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ARM_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(THUMB_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj/%.o: %.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD)/crt0.o: $(GBA_ROOT)/tools/crt0.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -marm -c $< -o $@

$(BUILD)/$(TARGET).elf: $(CRT0) $(OBJS) $(TONC_LIB)
	$(CC) $(LDFLAGS) $(CRT0) $(OBJS) -L$(TONC_BUILD) -ltonc -o $@

$(TARGET).gba: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	$(FIXCMD)

.PHONY: all clean
all: $(TARGET).gba

clean:
	rm -rf $(BUILD) $(TARGET).gba

-include $(DEPS)
