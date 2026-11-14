#ifndef SRB2_VITA_PATHS_H
#define SRB2_VITA_PATHS_H

#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define VITA_WRITABLE_ROOT "ux0:data/srb2"

static int VITA_PathIsWritable(const char *path)
{
	static const char root[] = VITA_WRITABLE_ROOT;
	const char *part;
	const char *p;

	if (!path || strncmp(path, root, sizeof(root) - 1) != 0)
		return 0;
	p = path + sizeof(root) - 1;
	if (*p && *p != '/')
		return 0;
	while (*p)
	{
		if (*p++ != '/')
			return 0;
		part = p;
		while (*p && *p != '/')
		{
			if (*p == ':' || *p == '\\')
				return 0;
			p++;
		}
		if ((p - part == 1 && part[0] == '.') ||
			(p - part == 2 && part[0] == '.' && part[1] == '.'))
			return 0;
	}
	return 1;
}

static inline int VITA_ResolveOutput(char *dest, size_t size, const char *requested)
{
	int length;
	if (!requested || !*requested)
	{
		errno = EACCES;
		return 0;
	}
	if (requested[0] == '/' || strchr(requested, ':'))
		length = snprintf(dest, size, "%s", requested);
	else
		length = snprintf(dest, size, VITA_WRITABLE_ROOT "/%s", requested);
	if (length < 0 || (size_t)length >= size)
	{
		errno = ENAMETOOLONG;
		return 0;
	}
	if (!VITA_PathIsWritable(dest))
	{
		errno = EACCES;
		return 0;
	}
	return 1;
}

static const char *VITA_OutputBase(const char *requested, const char *fallback, int *use_default)
{
	if (VITA_PathIsWritable(requested))
	{
		*use_default = 0;
		return requested;
	}
	*use_default = 1;
	return fallback;
}

#endif
