// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2014-2023 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file vita_io.c
/// \brief Block cached read-only FILE streams for WAD/PK3 access

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <psp2/io/fcntl.h>

// Every sceIoRead/sceIoLseek is a round trip to the memory card, and
// parsing a pk3 does one seek and one small read per lump (local headers,
// then patch/flat/sprite headers): tens of thousands of them at boot.
// Serving those from a few large cached blocks keeps it to sequential reads.
#define BLOCK_SHIFT 16
#define BLOCK_SIZE (1 << BLOCK_SHIFT)
#define NUM_BLOCKS 8

typedef struct
{
	SceUID fd;
	SceOff size;
	SceOff pos;
	SceOff tag[NUM_BLOCKS];
	unsigned int used[NUM_BLOCKS];
	unsigned int tick;
	unsigned char *data;
} vitafile_t;

static int PreadAll(SceUID fd, void *buf, size_t n, SceOff offset)
{
	size_t done = 0;

	while (done < n)
	{
		int got = sceIoPread(fd, (char *)buf + done, n - done, offset + done);
		if (got <= 0)
			break;
		done += got;
	}
	return (int)done;
}

static unsigned char *GetBlock(vitafile_t *f, SceOff block)
{
	int i, slot = 0;

	for (i = 0; i < NUM_BLOCKS; i++)
	{
		if (f->tag[i] == block)
		{
			f->used[i] = ++f->tick;
			return f->data + i * BLOCK_SIZE;
		}
		if (f->used[i] < f->used[slot])
			slot = i;
	}

	f->tag[slot] = -1;
	if (PreadAll(f->fd, f->data + slot * BLOCK_SIZE, BLOCK_SIZE, block << BLOCK_SHIFT) <= 0)
		return NULL;
	f->tag[slot] = block;
	f->used[slot] = ++f->tick;
	return f->data + slot * BLOCK_SIZE;
}

static ssize_t VIO_Read(void *cookie, char *buf, size_t n)
{
	vitafile_t *f = cookie;
	size_t done = 0;

	if (f->pos >= f->size)
		return 0;
	if ((SceOff)n > f->size - f->pos)
		n = (size_t)(f->size - f->pos);

	// whole lumps (music, maps, big textures) skip the cache
	if (n >= 2 * BLOCK_SIZE)
	{
		int got = PreadAll(f->fd, buf, n, f->pos);
		f->pos += got;
		return got;
	}

	while (done < n)
	{
		SceOff block = f->pos >> BLOCK_SHIFT;
		size_t ofs = (size_t)(f->pos & (BLOCK_SIZE - 1));
		size_t len = BLOCK_SIZE - ofs;
		unsigned char *data = GetBlock(f, block);

		if (!data)
			break;
		if (len > n - done)
			len = n - done;
		memcpy(buf + done, data + ofs, len);
		done += len;
		f->pos += len;
	}
	return (ssize_t)done;
}

#ifdef __LARGE64_FILES
static int VIO_Seek(void *cookie, _off64_t *offset, int whence)
#else
static int VIO_Seek(void *cookie, off_t *offset, int whence)
#endif
{
	vitafile_t *f = cookie;
	SceOff pos;

	switch (whence)
	{
		case SEEK_SET: pos = *offset; break;
		case SEEK_CUR: pos = f->pos + *offset; break;
		case SEEK_END: pos = f->size + *offset; break;
		default: return -1;
	}
	if (pos < 0)
		return -1;

	f->pos = pos;
	*offset = pos;
	return 0;
}

static int VIO_Close(void *cookie)
{
	vitafile_t *f = cookie;

	sceIoClose(f->fd);
	free(f->data);
	free(f);
	return 0;
}

FILE *VITA_FOpenRead(const char *path)
{
	cookie_io_functions_t io = { VIO_Read, NULL, VIO_Seek, VIO_Close };
	vitafile_t *f;
	FILE *handle;
	SceUID fd;
	int i;

	fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return NULL;

	f = calloc(1, sizeof (*f));
	if (f)
		f->data = malloc(NUM_BLOCKS * BLOCK_SIZE);
	if (!f || !f->data)
	{
		free(f);
		sceIoClose(fd);
		return NULL;
	}

	f->fd = fd;
	f->size = sceIoLseek(fd, 0, SCE_SEEK_END);
	for (i = 0; i < NUM_BLOCKS; i++)
		f->tag[i] = -1;

	handle = fopencookie(f, "rb", io);
	if (!handle)
	{
		VIO_Close(f);
		return NULL;
	}

	// newlib flushes every open stream before each read from an unbuffered
	// one, so keep a small stdio buffer on top of the block cache
	setvbuf(handle, NULL, _IOFBF, 4096);
	return handle;
}
