# Paper test set: how to print, draw and photograph

Goal: about **30 photos** (10 levels x 3 lights) of crayon drawings on the printed template, so the paper-mode vision can be
tuned on real paper instead of synthetic photos. About 45 minutes with the kids. The kids can draw most of the levels.

## 1. Print the template

File: [`docs/assets/scribble-run-level-template-a4.pdf`](assets/scribble-run-level-template-a4.pdf) (A4 landscape).

1. Print on **plain white A4**, **100 % / "Actual size"**. NOT "Fit to page" or "Scale to fit".
2. **Check the scale:** the line at the bottom left must measure **10.0 cm** (within 1 mm). If it doesn't, change the print setting and reprint.
3. Use matt white paper (not glossy, not coloured). Print **12 copies** (10 levels plus 2 spares).
4. The four black dots in the corners must be fully printed. Don't write or draw on them, and don't let a thumb cover one.

## 2. Draw the levels

Pens: a **fat black marker or crayon**, a **red** one and a **blue** one. Lines must be **at least 3 mm thick** (a fat marker; thin
pencil or a ballpoint will not show up). Draw **inside the dashed box**.

| Material | Colour | What it does |
|---|---|---|
| Ground | black | the ball rolls on it |
| Spikes | red (zigzag) | pops the ball |
| Bouncy | blue (squiggle) | launches the ball |

Always leave a **floor for the ball on the left** and a **place for the flag on the right**. Start and Flag are placed for you.

Suggested set (mix of easy, messy and awkward on purpose):

| # | Level | Why |
|---|---|---|
| 1 | A plain floor, left to right | the easy case |
| 2 | A hill and a valley | curves |
| 3 | A floor with a gap and a bridge | gaps |
| 4 | A floor with a patch of red spikes | red |
| 5 | A floor with a blue bouncy pad | blue |
| 6 | All three colours, stairs up | busy page |
| 7 | **Messy:** a kid's scribbly level, lines overlapping | the real use |
| 8 | **Light pressure:** crayon pressed softly (a bit pale) | faint ink |
| 9 | **Waxy crayon** (shiny, may glare) | glare |
| 10 | A normal level, but **smudge one corner dot** with a finger of black | missing mark |

Let marker ink dry before photographing (smudges look like extra ground).

## 3. Photograph each sheet in 3 lights

For each of the 10 sheets take **3 photos**:

1. **Daylight:** by a window, no direct sun on the paper.
2. **Lamp:** a normal room lamp from the side or above, the way you'd play.
3. **Dim:** the same room with the main light off (only a small lamp or dusk).

And for **about 6 of the 30**, repeat with the sheet or phone **tilted 10 to 20 degrees** (see below).

### How to take each photo (the phone stands in for the device's camera)

The device is stood up facing the paper, so copy that:

1. Put the sheet on a clipboard or stiff card and **lean it upright** against books (or tape it to a wall), **flat, not curled**.
2. Hold the phone **in landscape**, with its camera **40 cm** from the sheet (use the 40 cm string or a ruler: about the length of a school ruler plus a hand span), **centred in front**, the phone **parallel to the sheet** (looking straight at it).
3. Frame it so **all four black dots are inside the picture** with a little margin, and the sheet fills most of it.
4. Phone settings: **flash OFF**, **no zoom**, **no Portrait mode**, **no beauty or filter or HDR "enhance" effects** if you can switch them off (photo mode, "Auto"/standard). Tap the paper once so it focuses.
5. Hold still (or use the timer). **Keep fingers, hands and shadows off the paper.** Don't cast your own shadow across it: stand a little to one side of the light.
6. Take **1 photo per condition** (a second one if you think it blurred).

Tilt shots: aim the phone **10 to 20 degrees off straight-on** (turn the phone left or right a little, like you'd hold the device a bit crooked). More than about 20 degrees is too much: the corners can't be found (that is a known limit, and a useful test too: take one deliberately at 30 degrees and label it).

## 4. Name and send the photos

Keep the **original, uncropped JPEG** (don't edit, crop, filter or screenshot). Name them like:

`L03-daylight.jpg`, `L03-lamp.jpg`, `L03-dim.jpg`, `L03-lamp-tilt15.jpg`, `L10-lamp.jpg` ...
(`L` + level number from the table, then the light, then `-tiltN` if tilted).

Then either:

* put them in **`shared/golden/photos/`** in the repo (create the folder) and push; or
* attach them in the chat with the filenames, and add one line per level saying what you drew (for example "L06: black stairs, one red patch on step 2, blue pad at the end").

I will run each through the vision pipeline (Upload photo in the simulator, and the C++ pipeline on the PC), compare the level it produces with what you drew, and tune the thresholds so at least 80 % give a level you would happily play and the four dots are found in at least 95 % of photos.

## Quick checklist

- [ ] Printed at 100 %, the bottom line measures 10.0 cm
- [ ] Fat black, red and blue, lines at least 3 mm, inside the dashed box
- [ ] Sheet upright and flat, phone in landscape, about 40 cm away, straight on
- [ ] All four dots visible and clean in the photo, no fingers or shadows on the page
- [ ] Flash off, no zoom, no filters
- [ ] 3 lights per sheet, plus about 6 tilted shots
- [ ] Original JPEGs, named `L<number>-<light>[-tiltN].jpg`
