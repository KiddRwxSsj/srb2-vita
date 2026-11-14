#!/bin/sh
# vitaGL built from source instead of the vdpm package:
#  - the stock boot splash runs on a second GXM context, which Vita3K can't create
#  - GL error checking is pure overhead in a shipping build
#  - shader cache, logs and crash dumps go to ux0:data/srb2 instead of ux0:data
# Pinned to the revision the installed vitashark/SDK was built against.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/tools/vitaGL"
REV=aa32a8215a9f7f0fc08f8e22d14a55aa90a5872e
: "${VITASDK:=/usr/local/vitasdk}"
export VITASDK
export PATH="$VITASDK/bin:$PATH"

if [ ! -d "$SRC/.git" ]; then
	git clone https://github.com/Rinnegatamante/vitaGL.git "$SRC"
fi

cd "$SRC"
if [ "$(git rev-parse HEAD)" != "$REV" ]; then
	git fetch --shallow-since=2026-03-01 origin master 2>/dev/null || git fetch origin master
	git checkout -q -f "$REV"
	make clean >/dev/null 2>&1 || true
fi

python3 "$ROOT/tools/patch_vitagl.py" "$SRC"
make NO_SPLASHSCREEN=1 NO_DEBUG=1 -j"$(nproc)"
