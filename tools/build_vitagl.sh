#!/bin/sh
# vitaGL built from source instead of the vdpm package:
#  - the stock boot splash runs on a second GXM context, which Vita3K can't create
#  - GL error checking is pure overhead in a shipping build
# Pinned to the revision the installed vitashark/SDK was built against.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/tools/vitaGL"
REV=aa32a82
: "${VITASDK:=/usr/local/vitasdk}"
export VITASDK
export PATH="$VITASDK/bin:$PATH"

if [ ! -d "$SRC/.git" ]; then
	git clone https://github.com/Rinnegatamante/vitaGL.git "$SRC"
fi

cd "$SRC"
if [ "$(git rev-parse --short=7 HEAD)" != "$REV" ]; then
	git fetch --shallow-since=2026-03-01 origin master 2>/dev/null || git fetch origin master
	git checkout -q "$REV"
	make clean >/dev/null 2>&1 || true
fi

make NO_SPLASHSCREEN=1 NO_DEBUG=1 -j"$(nproc)"
