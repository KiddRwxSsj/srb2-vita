#!/usr/bin/env python3
# Small vitaGL changes the port needs, applied to the pinned checkout:
#  - keep every file vitaGL writes inside ux0:data/srb2 (old shader cache
#    location is still read so existing installs don't recompile everything)
#  - round data pool reservations up to 8 bytes, an odd sized index buffer
#    otherwise misaligns every later slot and the next VFP store faults
import sys
from pathlib import Path

MARK = "srb2_cache_paths.h"


def patch(root):
	src = root / "source"
	header = Path(__file__).resolve().parent / "vitagl" / MARK
	(src / MARK).write_bytes(header.read_bytes())

	for name, calls in (("ffp.c", ["fname"]), ("custom_shaders.c", ["fname", "vert_fname", "frag_fname"])):
		path = src / name
		text = path.read_text()
		if MARK not in text:
			for call in calls:
				text = text.replace(f"sceIoOpen({call}, SCE_O_RDONLY, 0777)", f"srb2_vgl_read_shader_cache({call})")
			text = text.replace('#include "shared.h"', '#include "shared.h"\n#include "' + MARK + '"', 1)
		path.write_text(fix_paths(text))

	path = src / "vgl.c"
	text = path.read_text()
	entry = "uint8_t *vgl_reserve_data_pool(uint32_t size) {\n"
	if "size = (size + 7) & ~7;" not in text:
		text = text.replace(entry, entry + "\tsize = (size + 7) & ~7;\n", 1)
	path.write_text(fix_paths(text))

	for path in src.glob("*.c"):
		text = path.read_text()
		fixed = fix_paths(text)
		if fixed != text:
			path.write_text(fixed)


def fix_paths(text):
	for old, new in (("ux0:data/shader_cache", "ux0:data/srb2/shader_cache"),
			("ux0:data/vgl_cache", "ux0:data/srb2/vgl_cache"),
			("ux0:/data/vitaGL.log", "ux0:data/srb2/vitaGL.log"),
			("ux0:data/vitagl_gpucrash.sgx", "ux0:data/srb2/vitagl_gpucrash.sgx")):
		# the header's fallback path must stay untouched
		text = text.replace(old, new) if old != "ux0:data/shader_cache" else text.replace('"' + old, '"' + new)
	return text


if __name__ == "__main__":
	if len(sys.argv) != 2:
		sys.exit("usage: patch_vitagl.py VITAGL_DIR")
	patch(Path(sys.argv[1]))
