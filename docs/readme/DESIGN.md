# README pictures: design rules

The rules the README's pictures, the app's icon and its HOME banner are drawn and checked against. If a rule or a colour changes,
change it here and in [`build.py`](build.py) together.

## 1. The principle: the cassette

**Everything is drawn the way the icon is.** The icon is a cream cassette tilted on a green tile, and that one object sets the
whole look: flat chunky shapes with generous round corners, cream on green, and depth that is never a blur, only the same shape
again in deep green, offset down and to the right. A few things lean (the cassette, at −8°); nothing glows, fades or casts a soft
shadow. The app's screens don't appear any more: the pictures are the brand, and the app shows itself on the 3DS.

The pieces are few, and each one is a cassette part:

- **The hero** (1280 x 640, so it doubles as the social preview): the wordmark and a five-word promise on the left, the maker's
  chip (GitHub mark, Lii-lac) under them, and the icon big on the right, lifted on its own depth. Over the wordmark, left-aligned
  the way the console's logo wears it, a little **new** in that logo's own silhouette (two brackets of three ring-slice tiles,
  measured off it: see `new_badge`), with only the colours swapped for the cassette's: six of them, as on the logo. Green, cream and the tape's brown
  down the left; their lighter siblings (each its diagonal opposite with the hue turned -30 degrees, as measured on the logo: `#a8506b`, `#e8c9d5`, `#47bd7c`; saturation lifted like the logo's) down the right in the opposite order, so the pair is
  symmetric about the `\` diagonal, not a mirror.
- **Most sections have a piece of music hardware beside their text**, floated (`align="right"` or `"left"` on the `<img>`,
  `<br clear="all">` before the next heading), sides alternating, no words: Install an SD card standing beside the steps (24 by
  32 mm, the cut corner, the lock in its notch on the left edge, grip ridges) whose label is a blank mixtape label, the tape
  window with the tape wound on and lines to write the songs on; Using it a tape deck's piano keys, play pushed down, beside the
  first paragraph; Spotify a phone playing the cassette and sending Wi-Fi out toward the text. A float never sits beside a list
  on its left, since list numbers would slide under it. The sound keeps its chart full width.
- **Building it is the cassette exploded**, the way a service manual draws it (640 x 960, floated right): every part straight
  above where it sits, in one view (30 degrees round, half depth), so pushing them back down the dashed lines and driving the
  screws would put it back together. Top to bottom: five screws, point down, the upper half with its label, a slip sheet, the
  tape on its two hubs with the guide rollers and the felt pad on its leaf spring, a slip sheet, the lower half face down
  showing its screw bosses and roller pins. Measurements are a real cassette's (100.4 by 63.8 mm, the hubs 42.5 apart), the
  holes are real holes (you see the next part through them), and each part's thickness is its shape again in deep green.
- **Two dividers:** a 3.5 mm plug meeting an extension's socket mid-page (its pin tip, ring and sleeve, the tip and the ring
  being the two channels), and a level meter's strip of lights.
- **The nav** is chunky buttons on their depth: green for where the README starts (Install), cream for the rest.
- **The chart** is a dark panel lifted on its depth, with one fat bar per sound setting.
- **The HOME banner** (256 x 128) is the wordmark beside the icon on the app's dark; the 48 x 48 icon is the tile full bleed,
  since the HOME Menu frames it itself.

## 2. The wordmark

**3 | DS | tereo**, on one baseline in IBM Plex Sans: the 3 Bold in green, DS Bold in the ink, tereo Regular in the ink. It isn't
the console's logo and borrows none of its letters; it only sits the way 3DS sits, with the S doing double duty. There's no
little two-screen glyph.

## 3. Rules

1. **Every word has to earn its place.** A word goes in a picture only if the picture is wrong without it: the hero says "new", the name,
   the promise and the maker; the chart its three settings and their numbers; the nav its five destinations. Everything
   else is the README's job.
2. **The cassette's palette only:** green `#1ed760`, deep green `#0a6e39` (depth, and nothing else), cream `#f2e6cf`, the foot's
   darker cream `#d9caa8`, ink `#121212`, oxide brown `#7a5236` (tape, and the new badge's third tile), white for text on the dark panel, grey `#b3b3b3` for its quiet labels, and Stock's
   `#a8997a`. Green means on: Best, Install, the left reel.
3. **One font:** IBM Plex Sans (SIL OFL 1.1), outlined to paths by `build.py`, so a picture looks the same on a machine with no
   fonts.
4. **Two themes**, served with `<picture>` and `prefers-color-scheme`: the light page is `#f4f1ee`, the dark one GitHub's
   `#0d1117`. The objects are the same in both; only the page, the words on it, and the colours of the tape and the headphone cord change.
5. **Contrast passes WCAG AA.** `build.py` checks every text colour against its ground and stops if one fails. The wordmark's
   green 3 is a logotype, which WCAG leaves out, so it isn't checked.
6. **The README makes sense with the pictures off.** Each picture has alt text; dividers have `alt=""`.
7. **Plain SVG, nothing fetched**, no animation.
8. **`###` headings**, so GitHub draws no rule under them.
9. **Honest numbers.** Each chart bar's track is 0 to 24 kHz and its fill is that setting's real Nyquist limit (16.364 and
   23.803 kHz).

The app's name is `NAME` at the top of `build.py`.
