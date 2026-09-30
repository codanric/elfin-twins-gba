#!/usr/bin/env python3
"""Interactive exploration helper for the Elfin Twins analysis."""
import os, pickle, sys
import elfin_sim, elfin_lcd

# Saved machine states ("snapshots"). Override with ELFIN_SNAPS=/some/dir.
SNAP_DIR = os.environ.get('ELFIN_SNAPS',
                          os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build', 'snaps'))
MENU = ['food', 'game', 'clean', 'meter', 'link', 'bag', 'picture', 'lamp']

def load(name='normal'):
    """Restore a saved state. 'normal' (a new pair at home, day 1, 12:03) is
    created on first use by running the ROM for 200 s."""
    p = elfin_sim.Pet()
    path = '%s/%s.snap' % (SNAP_DIR, name)
    if name == 'normal' and not os.path.exists(path):
        p.run(200)
        save(p, name)
        return p
    p.restore(pickle.load(open(path, 'rb')))
    return p

def save(p, name):
    os.makedirs(SNAP_DIR, exist_ok=True)
    pickle.dump(p.snapshot(), open('%s/%s.snap' % (SNAP_DIR, name), 'wb'))

def state(p):
    return 'BD=%02X B5=%02X B6=%02X B7=%02X B8=%02X B9=%02X BA=%02X BB=%02X BC=%02X' % tuple(
        p.peek(a) for a in (0xBD, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC))

def stats(p):
    return ' '.join('%02X=%02X' % (a, p.peek(a)) for a in range(0x8B, 0x9A))

def icons(p):
    return ','.join(n for n, v in elfin_lcd.icons_from_lcd(p.lcd()).items() if v and not n.startswith('arrow'))

def select(p, item):
    """Move the cursor to a menu item (from the idle screen)."""
    target = 1 << MENU.index(item)
    for _ in range(12):
        if p.peek(0xBC) == target and item in icons(p):
            return
        p.press('right'); p.run(0.2)

def watch(p, secs, step=0.25, show=False):
    prev = None
    frames = []
    t = 0
    while t < secs:
        p.run(step); t += step
        a = p.ascii()
        if a != prev:
            frames.append((p.seconds(), p.lcd()))
            if show == 'brief':
                print('--- t=%.2f %s | %s' % (p.seconds(), state(p), icons(p)))
            elif show:
                print('--- t=%.2f %s | %s' % (p.seconds(), state(p), icons(p)))
                print('\n'.join(a.split('\n')[:16]))
            prev = a
    return frames


def strip(frames, path, cols=6, width=228, every=1):
    """Save a contact sheet of LCD frames (rendered with the real artwork)."""
    from PIL import Image
    fr = frames[::every]
    ims = [elfin_lcd.render(lcd, width=width) for _, lcd in fr]
    if not ims:
        return
    w, h = ims[0].size
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * w + (cols - 1) * 4, rows * h + (rows - 1) * 4), (40, 40, 60))
    for i, im in enumerate(ims):
        sheet.paste(im, ((i % cols) * (w + 4), (i // cols) * (h + 4)))
    sheet.save(path)


def enter_item(p, item, hold=0.3):
    """Select a menu item and press ENTER once the twins accept input."""
    select(p, item)
    for _ in range(40):
        if not (p.peek(0xB9) & 0x04) and (p.peek(0xB7) & 0x04):
            break
        p.run(0.1)
    p.press('enter', hold=hold, after=0.5)
