#!/usr/bin/env python3
"""
Render the HTML documents in gba/docs to PDF with headless Chromium.

    python3 gba/docs/tools/build_pdfs.py            (needs: pip install playwright)

Set CHROMIUM=/path/to/chrome to use a specific browser binary.
"""
import glob
import os
import sys

from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
DOCS = os.path.abspath(os.path.join(HERE, ".."))

DOCUMENTS = [
    ("manual/manual.html", "Elfin_Twins_GM-021_Manual.pdf"),
    ("tech/technical.html", "Elfin_Twins_GM-021_Technical_Reference.pdf"),
]


def find_chromium():
    if os.environ.get("CHROMIUM"):
        return os.environ["CHROMIUM"]
    for c in sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux/chrome")):
        return c
    return None


def main():
    only = sys.argv[1:]
    with sync_playwright() as pw:
        exe = find_chromium()
        browser = pw.chromium.launch(executable_path=exe) if exe else pw.chromium.launch()
        for src, out in DOCUMENTS:
            if only and not any(o in src for o in only):
                continue
            page = browser.new_page()
            page.goto("file://" + os.path.join(DOCS, src))
            page.wait_for_load_state("networkidle")
            page.evaluate("document.fonts.ready")
            footer = ('<div style="width:100%;font-size:8px;color:#8a86a8;font-family:sans-serif;'
                      'padding:0 16mm;display:flex;justify-content:space-between">'
                      '<span>Elfin Twins GM-021</span><span class="pageNumber"></span></div>')
            page.pdf(path=os.path.join(DOCS, out), format="A4", print_background=True,
                     prefer_css_page_size=True, display_header_footer=True,
                     header_template="<div></div>", footer_template=footer)
            print("wrote", out)
            page.close()
        browser.close()


if __name__ == "__main__":
    main()
