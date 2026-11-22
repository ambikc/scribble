#!/usr/bin/env python3
"""Scribble Run - printable A4 level template (landscape). MIT licensed.

Run:  python3 tools/template/make_template.py  [output.pdf]
Needs: pip install reportlab

All geometry is in millimetres and mirrors shared/constants.json / docs/system-design.md
section 5.2. If you change a number here, change it there (the vision code finds the page
from the four corner marks and maps them onto the 320 x 192 level).
"""
import sys
from reportlab.pdfgen import canvas
from reportlab.lib.units import mm

# ---- geometry (mm, measured from the TOP-LEFT of the landscape A4 sheet) --------------
PAGE_W, PAGE_H = 297.0, 210.0
DOT_DIAMETER   = 10.0          # corner marks
RING_HOLE      = 3.5           # top-left mark is a ring: it tells the camera which corner is "top-left"
DOT_X          = (13.5, 283.5) # centres -> 270 mm apart
DOT_Y          = (24.0, 186.0) # centres -> 162 mm apart  (270 : 162 = 5 : 3 = the 320 x 192 level)
DOT_RECT_W     = DOT_X[1] - DOT_X[0]
DOT_RECT_H     = DOT_Y[1] - DOT_Y[0]
AREA_SCALE     = 0.92          # drawing area = dot rectangle shrunk to 92 % about its centre
AREA_W         = DOT_RECT_W * AREA_SCALE      # 248.4 mm  -> 320 level pixels (0.776 mm per pixel)
AREA_H         = DOT_RECT_H * AREA_SCALE      # 149.0 mm  -> 192 level pixels
CX, CY         = (DOT_X[0] + DOT_X[1]) / 2, (DOT_Y[0] + DOT_Y[1]) / 2
AREA_X0, AREA_Y0 = CX - AREA_W / 2, CY - AREA_H / 2

BLACK = (0.08, 0.08, 0.10)
RED   = (0.90, 0.22, 0.18)
BLUE  = (0.18, 0.43, 0.88)
GUIDE = (0.80, 0.80, 0.80)     # very light grey: visible to a child, ignored by the vision step
TEXT  = (0.30, 0.30, 0.30)


def y(mm_from_top):            # reportlab's origin is bottom-left
    return (PAGE_H - mm_from_top) * mm


def make(path):
    c = canvas.Canvas(path, pagesize=(PAGE_W * mm, PAGE_H * mm))
    c.setTitle("Scribble Run - level template (A4 landscape)")
    c.setAuthor("Scribble Run contributors (MIT)")
    c.setSubject("Draw your level inside the dashed box. Print at 100 percent.")

    # 1. drawing area: dashed light-grey box
    c.setStrokeColorRGB(*GUIDE); c.setLineWidth(0.8 * mm); c.setDash(4 * mm, 3 * mm)
    c.rect(AREA_X0 * mm, y(AREA_Y0 + AREA_H), AREA_W * mm, AREA_H * mm, stroke=1, fill=0)
    c.setDash()

    # 2. corner marks: three solid dots and one RING at the top-left
    c.setFillColorRGB(*BLACK)
    r = DOT_DIAMETER / 2
    for i, (dx, dy) in enumerate([(DOT_X[0], DOT_Y[0]), (DOT_X[1], DOT_Y[0]),
                                  (DOT_X[1], DOT_Y[1]), (DOT_X[0], DOT_Y[1])]):
        c.circle(dx * mm, y(dy), r * mm, stroke=0, fill=1)
        if i == 0:                                   # ring: punch a white hole
            c.setFillColorRGB(1, 1, 1)
            c.circle(dx * mm, y(dy), (RING_HOLE / 2) * mm, stroke=0, fill=1)
            c.setFillColorRGB(*BLACK)

    # 3. legend in the top margin (outside the dot rectangle, so the camera ignores it)
    ly = 11.0
    # ground
    c.setFillColorRGB(*BLACK);  c.roundRect(68 * mm, y(ly + 2.5), 22 * mm, 5 * mm, 1.2 * mm, stroke=0, fill=1)
    # spike (red zig-zag)
    c.setStrokeColorRGB(*RED); c.setLineWidth(1.6 * mm); c.setLineJoin(1); c.setLineCap(1)
    p = c.beginPath(); xs = 128
    p.moveTo(xs * mm, y(ly + 3)); 
    for k in range(1, 6):
        p.lineTo((xs + 4 * k) * mm, y(ly - 3 if k % 2 else ly + 3))
    c.drawPath(p, stroke=1, fill=0)
    # bouncy (blue spring)
    c.setStrokeColorRGB(*BLUE)
    p = c.beginPath(); xb = 190
    p.moveTo(xb * mm, y(ly + 3)); 
    for k in range(1, 6):
        p.lineTo((xb + 4 * k) * mm, y(ly - 3 if k % 2 else ly + 3))
    c.drawPath(p, stroke=1, fill=0)
    c.setFont("Helvetica-Bold", 8); c.setFillColorRGB(*TEXT)
    c.drawString(68 * mm, y(ly + 8.5), "GROUND  black")
    c.drawString(128 * mm, y(ly + 8.5), "SPIKES  red")
    c.drawString(190 * mm, y(ly + 8.5), "BOUNCY  blue")

    # 4. footer: how to use + print-scale check (outside the dot rectangle)
    c.setFillColorRGB(*TEXT); c.setFont("Helvetica", 8.5)
    c.drawString(24 * mm, y(195.0), "Draw big with a fat marker or crayon (lines at least 3 mm thick). Draw a floor for your ball on the left")
    c.drawString(24 * mm, y(198.8), "and a place for the flag on the right. Stand the device up, hold this sheet 40 cm from it, and tap GO.")
    c.setStrokeColorRGB(*TEXT); c.setLineWidth(0.5 * mm); c.setDash()
    c.line(24 * mm, y(202.2), 124 * mm, y(202.2))
    c.line(24 * mm, y(200.8), 24 * mm, y(203.6)); c.line(124 * mm, y(200.8), 124 * mm, y(203.6))
    c.setFont("Helvetica", 7)
    c.drawString(127 * mm, y(202.6), "This line must measure 10 cm. Print at 100% / Actual size (not Fit to page).")
    c.setFont("Helvetica", 6.5)
    c.drawRightString(272 * mm, y(202.6), "Scribble Run - MIT license")

    c.showPage(); c.save()


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "docs/assets/scribble-run-level-template-a4.pdf"
    make(out)
    print(f"wrote {out}")
    print(f"dot rectangle {DOT_RECT_W:.1f} x {DOT_RECT_H:.1f} mm, drawing area {AREA_W:.1f} x {AREA_H:.1f} mm "
          f"at ({AREA_X0:.2f}, {AREA_Y0:.2f}), {AREA_W/320:.4f} mm per level pixel")
