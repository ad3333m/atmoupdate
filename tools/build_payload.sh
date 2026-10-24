#!/bin/sh
# Builds romfs/swap_payload.bin: AIO Switch Updater's TegraExplorer fork (GPL-2.0,
# https://github.com/HamletDuFromage/TegraExplorer). At boot it renames
# atmosphere/{package3,stratosphere.romfs}.aio over the originals (which Atmosphère
# keeps open while running), then chainloads bootloader/update.bin (hekate).
set -e
REV=26f992d378697a406610004d3079a4b3463ed68f
DIR=${1:-/tmp/te}
OUT=${2:-romfs/swap_payload.bin}

rm -rf "$DIR"
git clone --quiet https://github.com/HamletDuFromage/TegraExplorer "$DIR"
git -C "$DIR" checkout --quiet "$REV"

# Lines 294-308 at this revision are AIO's ASCII-art banner; show our name instead.
sed -i '294,308c WPRINTF("AtmoUpdate: finishing the Atmosphere update...");' "$DIR/source/main.c"
grep -q 'AtmoUpdate: finishing' "$DIR/source/main.c"
if grep -q '"     /_/' "$DIR/source/main.c"; then echo "banner patch missed"; exit 1; fi

make -C "$DIR" -j"$(nproc)"

size=$(wc -c < "$DIR/output/TegraExplorer.bin")
echo "swap payload: $size bytes"
[ "$size" -le 126296 ] || { echo "payload exceeds IRAM limit"; exit 1; }
mkdir -p "$(dirname "$OUT")"
cp "$DIR/output/TegraExplorer.bin" "$OUT"
