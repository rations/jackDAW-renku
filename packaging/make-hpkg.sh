#!/bin/sh
# make-hpkg.sh — build the `jackdaw` .hpkg on a running Haiku (x86_64).
#
# Prerequisites on the build machine (build these from source first, in order):
#   - jack-port-haiku  (libjack in /boot/system/non-packaged/lib)
#   - VST3-haiku       (SDK static libs in ~/VST3-haiku/build/lib/Release)
# The Makefile's VST3_SDK / VST3_LIB default to ~/VST3-haiku; override if elsewhere.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
VERSION=$(cat "$ROOT/VERSION")   # the only place the version lives
REVISION=4                       # bump for a rebuild of the same VERSION
STAGE="$HERE/stage"

pkgman install -y gcc make pkgconfig haiku_devel glib2_devel libsndfile_devel libsamplerate_devel || true
export PKG_CONFIG_PATH="/boot/system/non-packaged/lib/pkgconfig:/boot/system/lib/pkgconfig:$PKG_CONFIG_PATH"

cd "$ROOT"
make clean
make

rm -rf "$STAGE"
mkdir -p "$STAGE/apps" "$STAGE/data/deskbar/menu/Applications" "$STAGE/add-ons/JackDAW"
cp JackDAW "$STAGE/apps/JackDAW"
cp backends/jack.so "$STAGE/add-ons/JackDAW/jack.so"
ln -sf ../../../../apps/JackDAW "$STAGE/data/deskbar/menu/Applications/JackDAW"

sed -e "s/@VERSION@/$VERSION/g" -e "s/@REVISION@/$REVISION/g" \
	"$HERE/jackdaw.PackageInfo" > "$STAGE/.PackageInfo"
OUT="$HERE/jackdaw-$VERSION-$REVISION-x86_64.hpkg"
package create -C "$STAGE" "$OUT"
echo ">> built $OUT"
