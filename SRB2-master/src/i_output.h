#ifndef SRB2_I_OUTPUT_H
#define SRB2_I_OUTPUT_H

#include <stdio.h>
#ifdef __vita__
#include "vita/vita_paths.h"
#include <unistd.h>
#include <stdlib.h>
#endif

static inline FILE *I_OpenFile(const char *path, const char *mode)
{
#ifdef __vita__
	char output[1024];
	if (strpbrk(mode, "wa+"))
	{
		if (!VITA_ResolveOutput(output, sizeof output, path))
			return NULL;
		return fopen(output, mode);
	}
#endif
	return fopen(path, mode);
}

static inline int I_RemoveFile(const char *path)
{
#ifdef __vita__
	char output[1024];
	if (!VITA_ResolveOutput(output, sizeof output, path))
		return -1;
	return remove(output);
#else
	return remove(path);
#endif
}

#ifdef __vita__
static inline int I_UnlinkFile(const char *path)
{
	char output[1024];
	if (!VITA_ResolveOutput(output, sizeof output, path))
		return -1;
	return unlink(output);
}
#else
#define I_UnlinkFile unlink
#endif

static inline FILE *I_TemporaryFile(void)
{
#ifdef __vita__
	char path[] = VITA_WRITABLE_ROOT "/srb2tmpXXXXXX";
	int descriptor = mkstemp(path);
	FILE *file;
	int saved_errno;

	if (descriptor < 0)
		return NULL;
	file = fdopen(descriptor, "wb+");
	saved_errno = errno;
	if (file == NULL)
		close(descriptor);
	I_RemoveFile(path);
	errno = saved_errno;
	return file;
#else
	return tmpfile();
#endif
}

#endif
