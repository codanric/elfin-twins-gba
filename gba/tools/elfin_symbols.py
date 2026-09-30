"""
Symbols for the Elfin Twins GM-021 ROM, collected while reverse engineering
it (see gba/docs). Used by elfin_disasm.py.

RAM / SFR entries: address -> (name, comment)
ROUTINES: address -> (name, description)
COMMENTS: instruction address -> comment
"""

SFR = {
    0x70: ("LCD_CFG", "LCD configuration"),
    0x71: ("PA_DIR", "port A direction: 1 = input, 0 = output"),
    0x72: ("PA_CFG", "port A output config; bits 6-7 = buzzer"),
    0x73: ("PA_DATA", "port A: b0 ESC b1 LEFT b2 RIGHT b3 CLOCK b4 ENTER (0 = pressed), b5 LINK"),
    0x74: ("PXY_DATA", "port X/Y (unused)"),
    0x75: ("PZ_DATA", "port Z (unused)"),
    0x76: ("LCD_BIAS", "LCD bias / contrast"),
    0x77: ("KEYSCAN_L", "key scan output (unused)"),
    0x78: ("KEYSCAN_H", "key scan output (unused)"),
    0x79: ("INT_CFG", "interrupt enable (write) / pending flags (read, clears)"),
    0x7A: ("SYS_CTRL", "system control: b7 stop osc, b5 32k, b4 timer, b3 LCD on, b2 LCD en"),
    0x7B: ("TC_PRESET", "down-counter preset = buzzer pitch"),
    0x7C: ("PRESCALER", "timer prescaler (1 << n)"),
    0x7E: ("KEYSCAN_CTRL", "key scan control"),
    0x7F: ("WDT_CLR", "watchdog clear"),
}

RAM = {
    0x80: ("lifespan", "days the pair will stay: 23-26, chosen at birth (rnd & 3) + 23"),
    0x81: ("rnd", "random number, advanced by rnd_next"),
    0x82: ("clk_halfsec", "clock: half-seconds 0-119"),
    0x83: ("clk_hour", "clock: hour 0-23"),
    0x84: ("clk_min", "clock: minute 0-59"),
    0x85: ("irq_flags", "INT_CFG pending flags latched at wake-up"),
    0x86: ("irq_nmi", "pending flags seen by the NMI handler"),
    0x87: ("int_sleep", "INT_CFG value used while asleep ($8D normally, $8C = CLOCK STOP)"),
    0x88: ("life_10min", "life timer: 10-minute blocks of the current day, 0-143"),
    0x89: ("life_min", "life timer: minutes 0-9"),
    0x8A: ("life_halfsec", "life timer: half-seconds 0-119"),
    0x8B: ("idle_ticks", "2 Hz ticks without input in a sub-screen (240 = back home)"),
    0x8C: ("weight", "weight in kg, 8-99"),
    0x8D: ("age_days", "age in days ('YR' on the meter), starts at 1"),
    0x8E: ("st_mood", "mood (flowers page) 0-33, shown x3"),
    0x8F: ("st_iq", "IQ 0-33, shown x3"),
    0x90: ("st_stamina", "stamina (sneaker page) 0-33, shown x3"),
    0x91: ("st_social", "friendship (people page) 0-33, shown x3"),
    0x92: ("money", "money in $100 units, 0-99 (0 = collapse)"),
    0x93: ("tummy", "meals in the tummy 0-5 (0 = hungry)"),
    0x94: ("drinks", "drinks 0-5 (0 = thirsty)"),
    0x95: ("sickness", "sickness 0-10: >=5 ill, >=10 collapse"),
    0x96: ("dirt", "dirt from digested meals: >=2 needs a bath, >=3 costs money"),
    0x97: ("care_clock", "10-minute counter since wake-up driving hunger/thirst/digestion"),
    0x98: ("ill_clock", "10-minute counter while ill (6 = one hour)"),
    0x9A: ("snd_len", "melody: remaining duration of the current note"),
    0x9B: ("snd_ptr", "melody: pointer to the note list (lo)"),
    0x9C: ("snd_ptr_hi", "melody: pointer to the note list (hi)"),
    0x9D: ("key", "decoded key: 1 UP 2 DOWN 3 LEFT 4 RIGHT 5 ENTER 6 ESC 7 ENTER-long 8 CLOCK 9 ESC-long A ESC+ENTER B CLOCK-long"),
    0x9E: ("key_raw", "PA_DATA & $1F when the key went down"),
    0x9F: ("key_hold", "key debounce / hold counter (4 = pressed, 255 = long)"),
    0xA0: ("gfx_x", "sprite draw: x position"),
    0xA1: ("gfx_y", "sprite draw: y position"),
    0xA2: ("gfx_ptr", "sprite draw: pointer to bitmap (lo)"),
    0xA3: ("gfx_ptr_hi", "sprite draw: pointer to bitmap (hi)"),
    0xAE: ("tmp_count", "temp; pulse count for link_send"),
    0xAF: ("tmp", "temp (also the level written by link_send!)"),
    0xB4: ("link_edges", "link: edges counted by link_recv; also temp"),
    0xB5: ("link_state", "link session state (b7 = we are the caller)"),
    0xB6: ("idle_secs", "seconds since the last key press (stops at $F0)"),
    0xB7: ("flags_b7", "b1 ?! icon on, b2 menu cursor shown, b3 melody playing, b6 sound off, b7 ..."),
    0xB8: ("flags_b8", "b0 locked (password), b1 TEST MODE, b2 night (22-07), b3 screen initialised, ..."),
    0xB9: ("flags_b9", "b0 CLOCK STOP, b2 busy animation, b3 password set, b6 clock setting, b7 clock screen"),
    0xBA: ("sub_sel", "sub-screen choice / animation step"),
    0xBB: ("life_bits", "b0-1 growth stage, b2 choosing destination, b3 room messy, b4-5 destination, b6-7 location"),
    0xBC: ("menu_sel", "menu cursor, one bit: 1 food 2 game 4 clean 8 meter 10 link 20 bag 40 outing 80 lamp"),
    0xBD: ("mode", "game mode (low nibble) + sub-state (high nibble)"),
    0xC4: ("anim_id", "animation / scene id for anim_start"),
    0xCA: ("sel2", "secondary selection (food, location)"),
    0xCB: ("mode_saved", "mode to return to after an interruption"),
    0xCC: ("collapse_10min", "10-minute blocks since collapsing (144 = gone)"),
}

ROUTINES = {
    0x970C: ("reset", "Reset / wake-up vector. After STOP the chip restarts here;\n"
                      "pending interrupt flags decide between cold start and wake-up."),
    0x9827: ("new_pair", "Cold start: clock 12:00, lifespan, stats, intro"),
    0x8C01: ("nmi", "NMI handler (interrupts while the CPU is running)"),
    0x8C00: ("irq", "IRQ vector (unused: rti)"),
    0x8C6D: ("tick_2hz", "2 Hz tick: clock, life timer, 10-minute and daily events"),
    0x8DE7: ("night_flag", "Set/clear the night flag (22:00 / 07:00)"),
    0x8E09: ("growth_stage", "Stage from age: <=3 baby, <=8 child, <=15 teen, else adult"),
    0x8E26: ("attention", "Decide whether the ?! icon is shown"),
    0x8E74: ("sleep_main", "Common exit: update ?!, wait for the melody, then sleep"),
    0x8E7A: ("sleep", "Stop the oscillator until the next interrupt (32 kHz keeps running)"),
    0x8E83: ("snd_play", "Start melody X (also the instruction right after the STOP)"),
    0x8EB2: ("wake", "Wake-up handler: link line, keys, tick, animation"),
    0x8FB8: ("key_decode", "Debounce and decode the five contacts into a key code"),
    0x9061: ("gfx_draw", "Draw a bitmap from ROM ($A2) at ($A0,$A1) into LCD RAM"),
    0x9338: ("rnd_next", "Advance the random number generator"),
    0x9520: ("tick_dispatch", "Per-tick handler: jump table T_957F by mode"),
    0x93AD: ("key_dispatch", "Per-key handler: jump table T_959D by mode"),
    0x98E8: ("home_tick", "Mode 0 (home) tick: alerts, idle animations, bedtime"),
    0x9AF6: ("home_key", "Mode 0 (home) keys: menu, destination, test mode"),
    0x9CCA: ("test_add_10min", "TEST MODE: ESC adds 10 minutes"),
    0x9CF1: ("test_stage", "TEST MODE: long ESC cycles the growth stage"),
    0x9D51: ("cursor_off", "Hide the menu cursor icon"),
    0x9D83: ("cursor_on", "Show the menu cursor icon"),
    0x9DBA: ("attention_on", "Light ?! (with beep)"),
    0x9DC8: ("attention_off", "Clear ?!"),
    0x9E46: ("drinks_dec", "drinks - 1"), 0x9E4D: ("tummy_dec", "tummy - 1"),
    0x9E54: ("stamina_dec", "stamina - 1"), 0x9E5B: ("iq_dec", "IQ - 1"),
    0x9E62: ("mood_dec", "mood - 1"), 0x9E69: ("social_dec", "friendship - 1"),
    0x9E70: ("money_dec", "money - $100; at $0 the pair collapses"),
    0x9E7E: ("weight_dec", "weight - 1 kg (not below 8)"),
    0x9E87: ("drinks_inc", "drinks + 1 (max 5)"), 0x9E90: ("tummy_inc", "tummy + 1 (max 5)"),
    0x9E99: ("stamina_inc", "stamina + 1 (max 33)"), 0x9EA2: ("iq_inc", "IQ + 1 (max 33)"),
    0x9EAB: ("mood_inc", "mood + 1 (max 33)"), 0x9EB4: ("social_inc", "friendship + 1 (max 33)"),
    0x9EBD: ("money_inc", "money + $100 (max $9900)"), 0x9EC6: ("weight_inc", "weight + 1 kg (max 99)"),
    0xA007: ("food_tick", "Mode 1 (food) tick"), 0xA07C: ("food_key", "Mode 1 (food) keys"),
    0xA22B: ("food_effect", "Apply the effect of the served food"),
    0xA287: ("lamp_tick", "Mode 2 (light) tick"),
    0xA386: ("game_tick", "Mode 3 (games) tick"), 0xA40A: ("game_key", "Mode 3 (games) keys"),
    0xAA8E: ("doctor_tick", "Mode 4 (doctor) tick"),
    0xAAED: ("clean_tick", "Mode 5 (clean) tick"),
    0xAB3A: ("meter_tick", "Mode 6 (status meter) tick"), 0xAB84: ("meter_key", "Mode 6 keys"),
    0xAFAD: ("outing_tick", "Mode 7 (outings) tick"), 0xB0B9: ("outing_key", "Mode 7 keys"),
    0xB67D: ("outing_effect", "Apply the effect of outing code 1-12"),
    0xBF12: ("sleep_tick", "Mode 8 (asleep) tick: alarm 07:00, up 08:00"),
    0xBFED: ("birthday_tick", "Mode 9 (birthday) tick"),
    0xC015: ("collapse_tick", "Mode A (collapsed) tick: 24 h to rescue"),
    0xC09B: ("collapse_key", "Mode A keys: ESC+ENTER rescue / new pair"),
    0xC103: ("ending_tick", "Mode B (farewell) tick"),
    0xC154: ("wipe_ram", "Clear RAM $85-$FF and start a new pair"),
    0xC2CB: ("intro_tick", "Mode C (opening story) tick"),
    0x95BB: ("password_tick", "Mode D (password) tick"), 0x9631: ("password_key", "Mode D keys"),
    0xB76C: ("link_tick", "Mode E (link) tick: session state machine"),
    0xB7ED: ("link_key", "Mode E keys"),
    0xC72A: ("clock_screen", "Show the clock screen"),
    0xBE0A: ("link_send", "Send tmp_count level changes on PA5 (link cable)"),
    0xBE41: ("link_recv", "Count PA5 edges until the line is quiet"),
    0xBEA1: ("delay_1ms", "~1.26 ms delay (88 x 8 cycles)"),
    0xBEAD: ("delay_short", "~0.3 ms delay (20 x 8 cycles)"),
    0xBE8F: ("delay_8ms", "6 x delay_1ms"),
    0xC479: ("delay_183ms", "delay_n with A=$32 (clobbers $AF)"),
    0xC47D: ("delay_n", "delay $AF x 256 x 8 cycles"),
    0xC61B: ("wait_tick", "Busy-wait until the next 2 Hz tick"),
    0xC626: ("wait_melody", "Busy-wait until the melody has finished"),
    0xC637: ("lcd_fill", "Fill LCD RAM with A"),
    0xC641: ("anim_start", "Start animation / scene $C4"),

    # Sunplus factory test program ($8000-$83D4). Identical in every SPLB20
    # ROM in BrickEmuPy's assets/ (Apollo 2 in 1, Apollo Prince & Princess, Big Cat 9 in 1,
    # Gyaoppi 9 in 1), so it belongs to the chip, not to the game. Entered
    # through the test vectors at $FFF2-$FFF7 (presumably selected by a test
    # pin); the game never jumps here. Behaviour checked by running each mode
    # on the emulated core.
    0x8000: ("ft_reset", "FACTORY TEST (Sunplus): entry from test vector $FFF4.\n"
             "Port A as inputs, then read the test code the tester applies:\n"
             "  $F0 standby   $FA LCD all on   $F5 ROM dump   $AF misc/instr/IRQ\n"
             "  $0A checksum + RAM test   $0F RAM pattern echo   $AA LCD pattern + buzzer\n"
             "Anything else: clear the watchdog and read again."),
    0x804B: ("ft_standby", "Test $F0: interrupts off, stop the oscillator (standby current)"),
    0x8063: ("ft_lcd_all", "Test $FA: every LCD segment on, wait, then 32 kHz / LCD-only mode"),
    0x8094: ("ft_rom_dump", "Test $F5: port A as outputs, stream ROM $8000-$FFFF to it forever"),
    0x80BB: ("ft_misc", "Test $AF: port/LCD config pulses, three buzzer tones, execute one of\n"
             "each instruction form, then wait for an interrupt (result on port A: $09 = none,\n"
             "or the pending flags << 4 | $09 from ft_nmi)"),
    0x8171: ("ft_checksum", "Test $0A: 16-bit sum of all ROM bytes, sent as four nibbles $1x $2x $3x $4x;\n"
             "then checkerboard $AA/$55 over LCD RAM + CPU RAM (port A $F5 = pass, $05 = fail)\n"
             "and over data RAM $1000-$17FF ($F8 = pass, $08 = fail).\n"
             "Emulated: 11 21 33 41 F5 F8 for sum $F311 (the 4th nibble repeats the low\n"
             "byte's high nibble instead of the high byte's - a bug in Sunplus's code)."),
    0x82A8: ("ft_ram_echo", "Test $0F: same RAM patterns, every byte read back is output on port A"),
    0x8344: ("ft_lcd_buzzer", "Test $AA: copy the 64-byte pattern ft_lcd_pattern to LCD RAM, sweep the buzzer"),
    0x83BE: ("ft_nmi", "Test-mode NMI (vector $FFF2): pending flags << 4 | $09 to port A, mark done"),
    0x83CE: ("ft_irq", "Test-mode IRQ (vector $FFF6): return"),
    0x83CF: ("ft_delay", "~0.5 ms delay loop"),
}

# Entry points that the vectors / jump tables do not reach.
ENTRIES = (0x8000, 0x83BE, 0x83CE)

# Address ranges where CPU RAM holds something else than the game's
# variables (no RAM names in the listing there).
FOREIGN_CODE = ((0x8000, 0x83D5),)

# Labelled data blocks: address -> (name, description, length, kind)
#   kind "db"      plain bytes
#   kind "text20"  text stored as ASCII + $20 (decoded in the comment)
#   kind "vectors" 16-bit vector table
DATA = {
    0x837E: ("ft_lcd_pattern", "Factory test: LCD RAM pattern shown by test $AA", 0x40, "db"),
    0xC50B: ("font_ptrs", "Font: pointers to the 28 glyphs (A-Z, blank, '.'); letter code n -> entry n-1", 0x38, "db"),
    0xC543: ("font_glyphs", "Font: 28 glyphs of 7 bytes: width (5), height (7), then 35 pixel bits,\n"
             "rows stored bottom-up. Same format as Apollo Prince & Princess's font ($FF0B\n"
             "there); 16 of the 28 glyphs are identical, 10 letters are drawn differently.", 28 * 7, "db"),
    0xFF27: ("credit", "Hidden developer credit, never read by the game. Stored as ASCII + $20 so it\n"
             "does not show in a plain strings dump:", 58, "text20"),
    0xFFF0: ("vectors", "Vector table. $FFF2-$FFF7: test-mode vectors (factory test program);\n"
             "$FFFA-$FFFF: the normal NMI / RESET / IRQ vectors.", 16, "vectors"),
}
VECTOR_NAMES = {0xFFF0: "(unused)", 0xFFF2: "test NMI", 0xFFF4: "test RESET", 0xFFF6: "test IRQ",
                0xFFF8: "(unused)", 0xFFFA: "NMI", 0xFFFC: "RESET", 0xFFFE: "IRQ"}

COMMENTS = {}
