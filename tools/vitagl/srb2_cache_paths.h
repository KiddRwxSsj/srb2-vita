#ifndef SRB2_VGL_CACHE_PATHS_H
#define SRB2_VGL_CACHE_PATHS_H

#include <stdio.h>
#include <string.h>
#include <psp2/io/fcntl.h>

static inline SceUID srb2_vgl_read_shader_cache(const char *path)
{
	static const char root[] = "ux0:data/srb2/shader_cache";
	SceUID file = sceIoOpen(path, SCE_O_RDONLY, 0777);
	char legacy[256];
	int length;

	if (file >= 0 || strncmp(path, root, sizeof root - 1) || path[sizeof root - 1] != '/')
		return file;
	length = snprintf(legacy, sizeof legacy, "ux0:data/shader_cache%s", path + sizeof root - 1);
	if (length < 0 || (size_t)length >= sizeof legacy)
		return file;
	return sceIoOpen(legacy, SCE_O_RDONLY, 0777);
}

#endif
