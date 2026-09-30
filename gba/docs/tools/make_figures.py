#!/usr/bin/env python3
"""
Generate the screenshots used by the manual and the technical document.

Every picture is produced by running the real ROM on the emulated SPLB20
(gba/host/elfin_sim.py) and rendering LCD RAM with the toy's own artwork
(assets/ElfinTwins.svg), so what you see is exactly what the toy shows.

    python3 gba/docs/tools/make_figures.py [--only name,name]

Writes gba/docs/img/<name>.png (single frames) and <name>_seq.png (strips).
"""
import os
import pickle
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "gba", "host"))
sys.path.insert(0, os.path.join(ROOT, "gba", "tools"))

import elfin_lcd  # noqa: E402
import elfin_sim  # noqa: E402
from PIL import Image  # noqa: E402

OUT = os.path.join(ROOT, "gba", "docs", "img")
CACHE = os.path.join(ROOT, "gba", "host", "build")
MENU = ["food", "game", "clean", "meter", "link", "bag", "picture", "lamp"]
W = 456   # panel render width (2x the GBA screen)


# ---------------------------------------------------------------- helpers
def normal_pet():
    """A freshly hatched pair at home, day 1, 12:03 (cached)."""
    path = os.path.join(CACHE, "fig_normal.snap")
    p = elfin_sim.Pet()
    if os.path.exists(path):
        p.restore(pickle.load(open(path, "rb")))
    else:
        p.run(200)
        os.makedirs(CACHE, exist_ok=True)
        pickle.dump(p.snapshot(), open(path, "wb"))
    return p


def icons(p):
    return [n for n, v in elfin_lcd.icons_from_lcd(p.lcd()).items() if v]


def select(p, item):
    target = 1 << MENU.index(item)
    for _ in range(12):
        if p.peek(0xBC) == target and item in icons(p):
            return
        p.press("right", after=0.3)


def ready(p):
    for _ in range(60):
        if not (p.peek(0xB9) & 0x04) and (p.peek(0xB7) & 0x04):
            return
        p.run(0.1)


def enter_item(p, item):
    select(p, item)
    ready(p)
    p.press("enter", hold=0.3, after=0.5)


def frames(p, secs, step=0.5):
    out = []
    t = 0.0
    while t < secs:
        p.run(step)
        t += step
        out.append(p.lcd())
    return out


def distinct(lcds):
    out = []
    for l in lcds:
        if not out or out[-1] != l:
            out.append(l)
    return out


def save_frame(name, lcd, width=W):
    im = elfin_lcd.render(lcd, width=width)
    im.save(os.path.join(OUT, name + ".png"))
    return im


def save_strip(name, lcds, cols=4, width=300, gap=8):
    ims = [elfin_lcd.render(l, width=width) for l in lcds]
    if not ims:
        return
    w, h = ims[0].size
    cols = min(cols, len(ims))
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * w + (cols - 1) * gap, rows * h + (rows - 1) * gap), (255, 255, 255))
    for i, im in enumerate(ims):
        sheet.paste(im, ((i % cols) * (w + gap), (i // cols) * (h + gap)))
    sheet.save(os.path.join(OUT, name + ".png"))


# ---------------------------------------------------------------- figures
FIGS = {}


def fig(fn):
    FIGS[fn.__name__] = fn
    return fn


@fig
def toy():
    p = normal_pet()
    elfin_lcd.render(p.lcd(), width=637, full=True).save(os.path.join(OUT, "toy.png"))
    elfin_lcd.render(bytes(64), width=W, ghost=1.0).save(os.path.join(OUT, "panel_all.png"))
    elfin_lcd.render(bytes(64), width=W).save(os.path.join(OUT, "panel_off.png"))


@fig
def intro():
    p = elfin_sim.Pet()
    lcds = distinct(frames(p, 165, step=1.0))
    save_strip("intro_seq", lcds[::max(1, len(lcds) // 12)][:12], cols=4)


@fig
def home():
    p = normal_pet()
    save_frame("home", p.lcd())
    lcds = distinct(frames(p, 20, 1.0))
    save_strip("home_seq", lcds[:8], cols=4)


@fig
def menu():
    p = normal_pet()
    shots = []
    for item in MENU:
        select(p, item)
        shots.append(p.lcd())
    save_strip("menu_ring", shots, cols=4)


@fig
def food():
    p = normal_pet()
    enter_item(p, "food")
    p.run(1.5)
    opts = []
    for i in range(3):
        opts.append(p.lcd())
        p.press("right", after=0.6)
    save_strip("food_choices", opts, cols=3)
    p.press("enter", after=0.2)
    save_strip("food_eat", distinct(frames(p, 8, 0.5))[:8], cols=4)


@fig
def games():
    p = normal_pet()
    enter_item(p, "game")
    p.run(1.5)
    opts = [p.lcd()]
    p.press("right", after=0.6)
    opts.append(p.lcd())
    save_strip("game_choices", opts, cols=2)
    p.press("left", after=0.6)
    p.press("enter", after=0.2)
    save_strip("game_shell", distinct(frames(p, 14, 0.5))[::2][:8], cols=4)
    q = normal_pet()
    enter_item(q, "game")
    q.run(1.5)
    q.press("right", after=0.6)
    q.press("enter", after=0.2)
    save_strip("game_slots", distinct(frames(q, 10, 0.5))[::2][:8], cols=4)


@fig
def meter():
    p = normal_pet()
    enter_item(p, "meter")
    p.run(0.4)
    pages = []
    for i in range(8):
        pages.append(p.lcd())
        p.press("right", after=0.3)
    save_strip("meter_pages", pages, cols=4)
    vals = []
    for page in range(8):
        q = normal_pet()
        enter_item(q, "meter")
        q.run(0.4)
        for _ in range(page):
            q.press("right", after=0.3)
        q.press("enter", after=0.2)
        f = frames(q, 2.5, 0.5)
        vals.append(f[-1])
    save_strip("meter_values", vals, cols=4)


@fig
def picture():
    p = normal_pet()
    enter_item(p, "picture")
    p.run(1.0)
    opts = []
    for i in range(3):
        opts.append(p.lcd())
        p.press("right", after=0.6)
    save_strip("activity_choices", opts, cols=3)
    p.press("right", after=0.6)
    p.press("enter", after=0.2)
    save_strip("activity_music", distinct(frames(p, 12, 0.5))[::2][:8], cols=4)


@fig
def clean():
    p = normal_pet()
    p.poke(0x96, 3)
    lc = distinct(frames(p, 14, 1.0))
    save_strip("alert_dirty", lc[:8], cols=4)
    enter_item(p, "clean")
    save_strip("clean_seq", distinct(frames(p, 12, 0.5))[::2][:8], cols=4)


@fig
def sick():
    p = normal_pet()
    p.poke(0x95, 6)
    lc = distinct(frames(p, 14, 1.0))
    save_strip("alert_sick", lc[:8], cols=4)
    enter_item(p, "bag")
    save_strip("doctor_seq", distinct(frames(p, 14, 0.5))[::2][:8], cols=4)


@fig
def night():
    p = normal_pet()
    p.poke(0x83, 22)
    p.poke(0x84, 5)
    p.run(12)
    lc = distinct(frames(p, 14, 1.0))
    save_strip("alert_night", lc[:8], cols=4)
    enter_item(p, "lamp")
    save_strip("lamp_seq", distinct(frames(p, 12, 0.5))[::2][:8], cols=4)
    # morning
    p.poke(0x83, 6)
    p.poke(0x84, 59)
    save_strip("morning_seq", distinct(frames(p, 90, 2.0))[::2][:8], cols=4)


def idle_ready(p):
    for _ in range(80):
        if not (p.peek(0xB9) & 0x04):
            return
        p.run(0.1)


@fig
def travel():
    p = normal_pet()
    for _ in range(5):
        idle_ready(p)
        p.press("up", after=0.5)
        if p.peek(0xBB) & 0x04:
            break
    sel = [p.lcd()]
    p.press("right", after=0.5)
    sel.append(p.lcd())
    save_strip("travel_select", sel, cols=2)
    idle_ready(p)
    p.press("enter", after=0.2)
    save_strip("travel_seq", distinct(frames(p, 16, 0.5))[::3][:8], cols=4)
    save_frame("at_building", p.lcd())


@fig
def clock():
    p = normal_pet()
    p.press("clock", after=1.0)
    a = p.lcd()
    p.press("left", after=0.8)
    b = p.lcd()
    p.press("right", after=0.5)
    p.down("clock"); p.run(2.5); p.up("clock"); p.run(0.5)
    c = p.lcd()
    p.press("esc", after=0.6)
    for _ in range(3):
        p.press("left", after=0.2)
    p.press("enter", after=0.2)
    lit = lambda l: sum(bin(x).count("1") for x in l)
    seen = [l for l in distinct(frames(p, 8, 0.25)) if lit(l) > 4]
    # the "NEW P.W." frame (most dots lit) and a later frame with the digits
    k = max(range(len(seen)), key=lambda i: lit(seen[i]))
    e = seen[k]
    later = [l for l in seen[k + 1:] if lit(l) < lit(e) * 0.7]
    f = later[-1] if later else seen[-1]
    save_strip("clock_seq", [a, b, c, e, f], cols=5, width=240)


@fig
def stages():
    shots = []
    for stage in range(4):
        p = normal_pet()
        p.poke(0xBB, (p.peek(0xBB) & 0xFC) | stage)
        p.run(20)
        shots.append(p.lcd())
    save_strip("stages", shots, cols=4)


@fig
def collapse():
    p = normal_pet()
    p.poke(0x95, 10)
    p.poke(0x89, 9)
    p.poke(0x8A, 0x76)
    p.run(8)
    save_strip("collapse_seq", distinct(frames(p, 16, 1.0))[:8], cols=4)
    print("collapse mode %02X" % p.peek(0xBD))
    for _ in range(8):   # ESC+ENTER together (retry if an animation swallowed it)
        p.lib.h_port(p.h, 0x11, 0); p.run(0.4); p.lib.h_port(p.h, 0x11, -1); p.run(0.8)
        if (p.peek(0xBD) & 0x0F) != 0x0A:
            break
    save_strip("revive_seq", distinct(frames(p, 16, 1.0))[::2][:8], cols=4)
    print("after revive mode %02X money %d" % (p.peek(0xBD), p.peek(0x92)))


@fig
def ending():
    p = normal_pet()
    p.poke(0x8D, p.peek(0x80) - 1)
    p.poke(0x88, 143)
    p.poke(0x89, 9)
    p.poke(0x8A, 0x70)
    p.run(4)
    save_strip("ending_seq", distinct(frames(p, 60, 0.5))[::4][:8], cols=4)
    print("ending mode %02X" % p.peek(0xBD))


@fig
def birthday():
    p = normal_pet()
    p.poke(0x88, 143)
    p.poke(0x89, 9)
    p.poke(0x8A, 0x70)
    p.run(6)
    save_strip("birthday_seq", distinct(frames(p, 20, 0.5))[::2][:8], cols=4)


@fig
def link():
    from link_sim import LinkC
    from explore import load
    a, b = normal_pet(), normal_pet()
    b.run(0.37)
    L = LinkC(a, b)
    for _ in range(12):
        if a.peek(0xBC) == 0x10:
            break
        L.press(a, "right", hold=0.15, after=0.35)
    for _ in range(40):
        if not (a.peek(0xB9) & 0x04):
            break
        L.step(0.1)
    L.press(a, "enter")
    L.step(5)
    menu_a = []
    for i in range(3):
        menu_a.append(a.lcd())
        L.press(a, "right", after=1.5)
    save_strip("link_menu", menu_a, cols=3)
    L.press(a, "enter")
    fa, fb = [], []
    for i in range(16):
        L.step(1.0)
        fa.append(a.lcd())
        fb.append(b.lcd())
    save_strip("link_game_a", distinct(fa)[:8], cols=4)
    save_strip("link_game_b", distinct(fb)[:8], cols=4)


def main():
    os.makedirs(OUT, exist_ok=True)
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1].split(",")
    for name, fn in FIGS.items():
        if only and name not in only:
            continue
        print("figure", name, flush=True)
        fn()



@fig
def sign():
    p = elfin_sim.Pet()
    while p.peek(0xBD) != 0x8C:
        p.run(0.5)
    shots = [l for l in distinct(frames(p, 3, 0.5))][-1:]
    for _ in range(5):                       # A B C D E
        p.press("right", hold=0.1, after=0.2)
    shots.append(p.lcd())
    p.press("esc", after=0.4)
    for _ in range(12):                      # ... L
        p.press("right", hold=0.1, after=0.2)
    shots.append(p.lcd())
    p.press("esc", after=0.4)
    shots.append(p.lcd())
    save_strip("sign_seq", shots, cols=4, width=240)


# ---------------------------------------------------------------- diagrams
FONT = os.path.join(ROOT, "gba", "docs", "fonts", "Nunito_3.ttf")

# icon bounding boxes in SVG units (measured by rendering each segment)
ICON_BOXES = {
    "arrow_down": (113, 110, 138, 135), "arrow_up": (503, 110, 527, 135),
    "arrow_left": (112, 351, 137, 376), "arrow_right": (502, 349, 527, 373),
    "building": (188, 107, 235, 139), "house": (297, 108, 341, 141),
    "exercise": (393, 104, 457, 139), "food": (107, 163, 135, 194),
    "game": (106, 225, 139, 267), "clean": (102, 279, 136, 327),
    "lamp": (506, 162, 531, 190), "picture": (502, 230, 534, 258),
    "bag": (502, 296, 534, 323), "meter": (189, 347, 234, 378),
    "question": (305, 347, 327, 375), "link": (398, 345, 449, 377),
}
ICON_LABELS = {
    "building": "Office / town", "house": "Home", "exercise": "Running track",
    "food": "Food & drink", "game": "Games", "clean": "Clean",
    "lamp": "Light", "picture": "Outings", "bag": "Doctor",
    "meter": "Status meter", "question": "Attention!", "link": "Link",
}


@fig
def diagrams():
    from PIL import ImageDraw, ImageFont
    font = ImageFont.truetype(FONT, 22)
    # --- labelled LCD panel
    scale = 1.6
    pw = int((elfin_lcd.PANEL_BOX[2] - elfin_lcd.PANEL_BOX[0]) * scale)
    panel = elfin_lcd.render(bytes(64), width=pw, ghost=1.0)
    pad_x, pad_y = 250, 70
    img = Image.new("RGB", (panel.width + 2 * pad_x, panel.height + 2 * pad_y), (255, 255, 255))
    img.paste(panel, (pad_x, pad_y))
    d = ImageDraw.Draw(img)
    x0, y0 = elfin_lcd.PANEL_BOX[0], elfin_lcd.PANEL_BOX[1]
    ink = (60, 40, 120)
    for name, label in ICON_LABELS.items():
        bx0, by0, bx1, by1 = ICON_BOXES[name]
        cx = pad_x + ((bx0 + bx1) / 2 - x0) * scale
        cy = pad_y + ((by0 + by1) / 2 - y0) * scale
        tw = d.textlength(label, font=font)
        if name in ("food", "game", "clean"):
            tx, ty = 20, cy - 12
            d.line([(cx - 30, cy), (tx + tw + 8, cy)], fill=ink, width=2)
        elif name in ("lamp", "picture", "bag"):
            tx, ty = img.width - tw - 20, cy - 12
            d.line([(cx + 30, cy), (tx - 8, cy)], fill=ink, width=2)
        elif name in ("building", "house", "exercise"):
            tx, ty = cx - tw / 2, 12
            d.line([(cx, cy - 30), (cx, ty + 30)], fill=ink, width=2)
        else:
            tx, ty = cx - tw / 2, img.height - 40
            d.line([(cx, cy + 28), (cx, ty - 4)], fill=ink, width=2)
        d.text((tx, ty), label, fill=ink, font=font)
    # dot matrix label
    mx = pad_x + (316 - x0) * scale
    my = pad_y + (240 - y0) * scale
    d.rounded_rectangle([mx - 150, my - 20, mx + 150, my + 18], radius=10, fill=(255, 255, 255))
    d.text((mx - 140, my - 16), "29 x 16 dot-matrix screen", fill=ink, font=font)
    img.save(os.path.join(OUT, "panel_labeled.png"))

    # --- labelled toy
    toy = elfin_lcd.render(normal_pet().lcd(), width=637, full=True)
    font2 = ImageFont.truetype(FONT, 20)
    img = Image.new("RGB", (toy.width + 440, toy.height), (255, 255, 255))
    img.paste(toy, (220, 0), toy)
    d = ImageDraw.Draw(img)
    buttons = [  # (x, y) on the toy, label, side
        ((188, 640), "UP", "L"), ((128, 700), "LEFT", "L"), ((188, 762), "DOWN", "L"),
        ((248, 700), "RIGHT", "L"), ((318, 838), "ESC", "L"),
        ((540, 618), "CLOCK", "R"), ((487, 766), "ENTER", "R"),
    ]
    ys_left = {"UP": 560, "LEFT": 650, "RIGHT": 740, "DOWN": 830, "ESC": 910}
    for (x, y), label, side in buttons:
        x += 220
        if side == "L":
            ty = ys_left[label]
            d.line([(x, y), (200, ty + 10)], fill=ink, width=2)
            d.ellipse([x - 5, y - 5, x + 5, y + 5], fill=ink)
            d.text((200 - d.textlength(label, font=font2) - 8, ty), label, fill=ink, font=font2)
        else:
            ty = y - 10
            d.line([(x, y), (img.width - 200, ty + 10)], fill=ink, width=2)
            d.ellipse([x - 5, y - 5, x + 5, y + 5], fill=ink)
            d.text((img.width - 192, ty), label, fill=ink, font=font2)
    d.line([(220 + 548, 250), (img.width - 200, 250)], fill=ink, width=2)
    d.text((img.width - 192, 240), "LCD screen", fill=ink, font=font2)
    img.save(os.path.join(OUT, "toy_labeled.png"))



# ---------------------------------------------------------------- provenance
MONO = os.path.join(ROOT, "gba", "docs", "fonts", "JetBrainsMono_1.ttf")
CREDIT_ADDR, CREDIT_LEN = 0xFF27, 58
FONT_PTRS, APOLLO_FONT = 0xC50B, 0xFF0B


def _rom(name="ElfinTwins.bin"):
    return open(os.path.join(ROOT, "assets", name), "rb").read()


def _glyph(data, off):
    """5x7 glyph record (width, height, 35 bits, rows bottom-up) -> rows top-down."""
    w, h = data[off], data[off + 1]
    bits = "".join(format(data[off + 2 + i], "08b") for i in range(5))
    return [bits[r * w:(r + 1) * w] for r in range(h)][::-1]


@fig
def credit():
    """The hidden credit at $FF27: stored bytes, then the text after subtracting $20."""
    from PIL import ImageDraw, ImageFont
    rom = _rom()
    raw = rom[CREDIT_ADDR - 0x8000:CREDIT_ADDR - 0x8000 + CREDIT_LEN]
    f = ImageFont.truetype(MONO, 19)
    fb = ImageFont.truetype(MONO, 19)
    fl = ImageFont.truetype(FONT, 18)
    per = 20
    cw, ch = 44, 30
    rows = (len(raw) + per - 1) // per
    img = Image.new("RGB", (80 + per * cw + 20, 36 + rows * (2 * ch + 22)), (255, 255, 255))
    d = ImageDraw.Draw(img)
    ink, muted, hi = (60, 40, 120), (140, 136, 170), (22, 150, 110)
    d.text((10, 6), "stored bytes, and each byte minus $20:", fill=muted, font=fl)
    for r in range(rows):
        y = 36 + r * (2 * ch + 22)
        d.text((10, y + 3), "$%04X" % (CREDIT_ADDR + r * per), fill=muted, font=fl)
        for i, b in enumerate(raw[r * per:(r + 1) * per]):
            x = 80 + i * cw
            d.rounded_rectangle([x, y, x + cw - 4, y + ch], radius=5, fill=(241, 240, 251))
            d.text((x + 6, y + 4), "%02X" % b, fill=ink, font=f)
            c = chr(b - 0x20)
            d.rounded_rectangle([x, y + ch + 3, x + cw - 4, y + 2 * ch + 3], radius=5, fill=(232, 248, 240))
            d.text((x + (cw - 4 - d.textlength(c, font=fb)) / 2, y + ch + 7), c, fill=hi, font=fb)
    img.save(os.path.join(OUT, "credit.png"))


@fig
def font_compare():
    """Elfin Twins' 5x7 font next to Apollo Prince & Princess's; differences marked."""
    from PIL import ImageDraw, ImageFont
    apollo = os.path.join(ROOT, "assets", "ApolloPrince&Princess.bin")
    if not os.path.exists(apollo):
        # Another toy's ROM, not part of this repository: copy it from
        # BrickEmuPy's assets/ to regenerate this figure.
        print("  skipped: needs assets/ApolloPrince&Princess.bin from BrickEmuPy")
        return
    rom, ap = _rom(), _rom("ApolloPrince&Princess.bin")
    ptr = [rom[a - 0x8000] | rom[a - 0x8000 + 1] << 8 for a in range(FONT_PTRS, FONT_PTRS + 52, 2)]
    px, gap, cell = 7, 14, 5 * 7 + 14
    fl = ImageFont.truetype(FONT, 18)
    img = Image.new("RGB", (150 + 13 * cell, 2 * (2 * (7 * px + 34)) + 20), (255, 255, 255))
    d = ImageDraw.Draw(img)
    ink, muted, diff = (60, 40, 120), (140, 136, 170), (214, 96, 40)
    for k in range(26):
        ge = _glyph(rom, ptr[k] - 0x8000)
        ga = _glyph(ap, APOLLO_FONT - 0x8000 + 7 * k)
        same = ge == ga
        half, col = divmod(k, 13)
        for row, (g, label) in enumerate(((ge, "Elfin"), (ga, "Apollo"))):
            y = 10 + half * (2 * (7 * px + 34)) + row * (7 * px + 30)
            x = 150 + col * cell
            if col == 0:
                d.text((10, y + 7 * px / 2 - 10), label + " Twins" if label == "Elfin" else "Apollo P&P",
                       fill=muted, font=fl)
            colour = ink if same else diff
            for r, line in enumerate(g):
                for c, bit in enumerate(line):
                    if bit == "1":
                        d.rectangle([x + c * px, y + r * px, x + c * px + px - 2, y + r * px + px - 2], fill=colour)
            if row == 1:
                d.text((x + 10, y + 7 * px + 4), chr(65 + k), fill=muted, font=fl)
    img.save(os.path.join(OUT, "font_compare.png"))



def factory_test(code, steps):
    """Start the Sunplus factory test program ($8000) with `code` applied to
    port A, as a tester would; returns the machine after `steps` instructions."""
    import ctypes
    p = elfin_sim.Pet()
    L = p.lib
    buf = (ctypes.c_uint8 * L.h_state_size())()
    L.h_state_get(p.h, buf)
    buf[0], buf[1] = 0x00, 0x80          # pc = $8000
    L.h_state_set(p.h, buf)
    L.h_port(p.h, 0xFF, 1)
    L.h_port(p.h, (~code) & 0xFF, 0)
    for _ in range(steps):
        L.h_step(p.h)
    return p


@fig
def factory_lcd():
    ims = [elfin_lcd.render(factory_test(0xFA, 2000).lcd(), width=W),
           elfin_lcd.render(factory_test(0xAA, 2000).lcd(), width=W)]
    img = Image.new("RGB", (2 * W + 24, ims[0].height), (255, 255, 255))
    img.paste(ims[0], (0, 0))
    img.paste(ims[1], (W + 24, 0))
    img.save(os.path.join(OUT, "factory_lcd.png"))


if __name__ == "__main__":
    main()
