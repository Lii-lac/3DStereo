#!/bin/sh
# Render one of the CIA's svgs to the png the build packs: svg2png.sh <in.svg> <w> <h> [out.png]
# (default out = the svg's name with .png), with headless Chrome at 1:1, no margin.
set -e
in=$1; w=$2; h=$3; out=${4:-${in%.svg}.png}
t=$(mktemp -d)
{ echo '<!doctype html><html><body style="margin:0;background:transparent">'; cat "$in"; echo '</body></html>'; } > "$t/p.html"
"/c/Program Files/Google/Chrome/Application/chrome.exe" --headless=new --disable-gpu --hide-scrollbars \
  --force-device-scale-factor=1 --default-background-color=00000000 --window-size=$w,$h \
  --user-data-dir="$(cygpath -w "$t/prof")" --screenshot="$(cygpath -w "$t/out.png")" "file:///$(cygpath -m "$t/p.html")" 2>/dev/null
cp "$t/out.png" "$out"; rm -rf "$t"; echo "$out (${w}x${h})"
