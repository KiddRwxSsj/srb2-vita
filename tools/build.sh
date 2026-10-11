#!/bin/sh
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${VITASDK:=/usr/local/vitasdk}"
export VITASDK
export PATH="$VITASDK/bin:$PATH"

[ -f "$ROOT/sce_sys/icon0.png" ] || "$ROOT/tools/make_sce_sys.sh"
[ -f "$ROOT/tools/vitaGL/libvitaGL.a" ] || "$ROOT/tools/build_vitagl.sh"

cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" -j"$(nproc)"

ls -l "$ROOT/build/srb2.vpk"
