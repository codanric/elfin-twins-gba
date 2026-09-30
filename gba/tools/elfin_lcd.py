"""
Elfin Twins LCD geometry and renderer, driven by assets/ElfinTwins.svg.

The SVG (from BrickEmuPy) contains one element per LCD segment, named
"<ram byte>_<bit>". 464 of them are the squares of a 29 x 16 dot matrix,
the remaining 16 are the printed icons around it (the "menu" icons and the
four arrows in the corners).

Used by the asset converter for the GBA port and by the analysis/manual
tooling to draw screenshots from LCD RAM contents.
"""
import io
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SVG_PATH = os.path.join(ROOT, "assets", "ElfinTwins.svg")

# Area of the SVG containing the LCD panel (glass + printed icon frame).
PANEL_BOX = (92.0, 95.5, 548.5, 392.0)

MATRIX_W = 29
MATRIX_H = 16

# Names for the 16 icon segments, from the artwork printed next to each.
ICON_NAMES = {
    # corners (yellow panels)
    (47, 0): "arrow_down",     # top-left
    (4, 6): "arrow_up",        # top-right
    (23, 0): "arrow_left",     # bottom-left
    (12, 6): "arrow_right",    # bottom-right
    # top row
    (15, 0): "building",       # office block + speech bubble
    (7, 0): "house",           # cottage with trees
    (36, 6): "exercise",       # figure on a running track
    # left column
    (55, 0): "food",           # fork & knife
    (63, 0): "game",           # little handheld game console
    (39, 0): "clean",          # rake
    # right column
    (44, 6): "lamp",           # table lamp (light)
    (60, 6): "picture",        # figure at the sea (play / outing)
    (52, 6): "bag",            # first-aid bag
    # bottom row
    (31, 0): "meter",          # dial gauge (status)
    (28, 6): "question",       # "?!" (attention)
    (20, 6): "link",           # two figures joined by an arc (connection)
}

_svg_text = None


def svg_text():
    global _svg_text
    if _svg_text is None:
        with open(SVG_PATH, encoding="utf-8") as f:
            _svg_text = f.read()
    return _svg_text


def _segments():
    s = svg_text()
    dots = {}
    for m in re.finditer(r'<path id="(\d+)_(\d+)" d="M([\d.]+) ([\d.]+)H([\d.]+)V([\d.]+)', s):
        b, bit = int(m.group(1)), int(m.group(2))
        x0, y0, x1, y1 = (float(m.group(i)) for i in range(3, 7))
        dots[(b, bit)] = (min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1))
    xs = sorted(set(round(v[0]) for v in dots.values()))
    ys = sorted(set(round(v[1]) for v in dots.values()))
    # merge y clusters that are 1 unit apart (156/157)
    merged = []
    for y in ys:
        if merged and y - merged[-1] <= 2:
            continue
        merged.append(y)
    ys = merged
    assert len(xs) == MATRIX_W and len(ys) == MATRIX_H, (len(xs), len(ys))
    matrix = {}
    for key, (x0, y0, x1, y1) in dots.items():
        cx = min(range(MATRIX_W), key=lambda i: abs(xs[i] - x0))
        cy = min(range(MATRIX_H), key=lambda i: abs(ys[i] - y0))
        matrix[(cx, cy)] = key
    icons = [(int(a), int(b)) for a, b in re.findall(r'id="(\d+)_(\d+)"', s)
             if (int(a), int(b)) not in dots]
    return dots, matrix, icons


DOT_RECTS, MATRIX, ICONS = _segments()
# (x, y) -> (ram byte, bit) for the dot matrix, and the reverse mapping
DOT_OF = {v: k for k, v in MATRIX.items()}


def lcd_bit(lcd, key):
    b, bit = key
    return (lcd[b] >> bit) & 1 if b < len(lcd) else 0


def matrix_from_lcd(lcd):
    """29x16 list of lists (rows) of 0/1."""
    return [[lcd_bit(lcd, MATRIX[(x, y)]) for x in range(MATRIX_W)] for y in range(MATRIX_H)]


def icons_from_lcd(lcd):
    return {ICON_NAMES[k]: lcd_bit(lcd, k) for k in ICONS}


def ascii_frame(lcd):
    rows = matrix_from_lcd(lcd)
    out = ["".join("#" if v else "." for v in r) for r in rows]
    on = [n for n, v in icons_from_lcd(lcd).items() if v]
    out.append("icons: " + (", ".join(on) if on else "-"))
    return "\n".join(out)


def _svg_with_opacity(opacity_of, hide_body=False):
    """Return the SVG text with each segment's opacity set by opacity_of(key)."""
    s = svg_text()

    def repl(m):
        key = (int(m.group(2)), int(m.group(3)))
        return '%s id="%s" opacity="%.3f"' % (m.group(1), m.group(2) + "_" + m.group(3), opacity_of(key))

    return re.sub(r'(<(?:path|g)) id="(\d+)_(\d+)"', repl, s)


def render(lcd, width=None, box=PANEL_BOX, ghost=0.0, full=False):
    """Render LCD RAM to a PIL image using the real artwork.

    lcd   : 64 bytes of LCD RAM (or None for all-off)
    box   : SVG region to render (default: the LCD panel)
    full  : render the whole toy instead of just the panel (RGBA, transparent
            around the shell)
    ghost : opacity of unlit segments (0 = invisible)
    """
    import cairosvg
    from PIL import Image

    lcd = lcd or bytes(64)

    def op(key):
        return 1.0 if lcd_bit(lcd, key) else ghost

    svg = _svg_with_opacity(op)
    if not full:
        x0, y0, x1, y1 = box
        svg = re.sub(r'<svg width="\d+" height="\d+" viewBox="[^"]+"',
                     '<svg width="%g" height="%g" viewBox="%g %g %g %g"'
                     % (x1 - x0, y1 - y0, x0, y0, x1 - x0, y1 - y0), svg, count=1)
    png = cairosvg.svg2png(bytestring=svg.encode("utf-8"), output_width=width)
    return Image.open(io.BytesIO(png)).convert("RGBA" if full else "RGB")
