#!/bin/sh
# build-from-source.sh — build and install JackDAW from source on Haiku.
#
# Build these from source FIRST, in order (each has its own build-from-source.sh):
#   1. jack-port-haiku   (libjack)
#   2. VST3-haiku        (SDK static libs at ~/VST3-haiku/build/lib/Release)
# The Makefile's VST3_SDK / VST3_LIB default to ~/VST3-haiku.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)

pkgman install -y gcc make pkgconfig haiku_devel glib2_devel libsndfile_devel libsamplerate_devel
export PKG_CONFIG_PATH="/boot/system/non-packaged/lib/pkgconfig:/boot/system/lib/pkgconfig:$PKG_CONFIG_PATH"

cd "$HERE"
make clean
make

mkdir -p /boot/system/non-packaged/apps
cp JackDAW /boot/system/non-packaged/apps/JackDAW
mkdir -p /boot/system/non-packaged/add-ons/JackDAW
cp backends/jack.so /boot/system/non-packaged/add-ons/JackDAW/jack.so
echo ">> Installed JackDAW to /boot/system/non-packaged/apps/JackDAW"
echo ">> and its JACK support to /boot/system/non-packaged/add-ons/JackDAW/jack.so"
echo ">> Start jackd, then launch JackDAW from Deskbar (Applications) or run ./JackDAW"
