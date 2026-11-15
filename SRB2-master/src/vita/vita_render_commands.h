// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.

#ifndef SRB2_VITA_RENDER_COMMANDS_H
#define SRB2_VITA_RENDER_COMMANDS_H

#include <stdlib.h>
#include <string.h>
#include "../i_system.h"
#include "../hardware/hw_defs.h"

#define RT_NULL ((size_t)-1)

enum
{
	RT_SETTEXTURE,
	RT_DRAWPOLYGON,
	RT_DRAWINDEXED,
	RT_SKYDOME,
	RT_SETBLEND,
	RT_CLEARBUFFER,
	RT_GCLIPRECT,
	RT_SPECIALSTATE,
	RT_SETTRANSFORM,
	RT_DRAW2DLINE,
	RT_POSTIMGREDRAW,
	RT_FLUSHSCREENTEX,
	RT_SCREENWIPE,
	RT_DRAWSCREENTEX,
	RT_MAKESCREENTEX,
	RT_DRAWSCREENFINAL,
	RT_SETSHADER,
	RT_UNSETSHADER,
	RT_SHADERINFO,
	RT_BATCHES,
	RT_UPLOAD,
	RT_PRESENT
};

typedef struct
{
	UINT32 op;
	union
	{
		UINT32 texname;
		INT32 value;
		FBITFIELD flags;
		gl_sky_t *sky;
		size_t data;
		struct { size_t surf, verts, indices; FUINT num; FBITFIELD flags; } poly;
		struct { FBOOLEAN color, depth; size_t rgba; } clear;
		struct { INT32 minx, miny, maxx, maxy; float nearclip; } clip;
		struct { INT32 id, value; } state;
		struct { size_t coords; RGBA_t color; } line;
		struct { INT32 a, b, c; size_t surf; FBITFIELD flags; } screen;
		struct { size_t polys, verts; INT32 num; } batch;
		struct { size_t tex, data; } upload;
	} u;
} rtcmd_t;

typedef struct
{
	rtcmd_t *cmds;
	size_t numcmds, maxcmds;
	UINT8 *data;
	size_t used, size;
} rtbuf_t;

static rtcmd_t *RT_BufferCommand(rtbuf_t *b, UINT32 op)
{
	if (b->numcmds == b->maxcmds)
	{
		b->maxcmds = b->maxcmds ? b->maxcmds * 2 : 4096;
		b->cmds = realloc(b->cmds, b->maxcmds * sizeof (rtcmd_t));
		if (!b->cmds)
			I_Error("Out of memory for render commands");
	}
	b->cmds[b->numcmds].op = op;
	return &b->cmds[b->numcmds++];
}

// Data is referenced by offset, so the arena can grow while a frame is recorded
static size_t RT_BufferAlloc(rtbuf_t *b, size_t len)
{
	size_t off;

	off = (b->used + 7) & ~(size_t)7;
	if (off + len > b->size)
	{
		size_t newsize = b->size ? b->size : 512 * 1024;
		while (off + len > newsize)
			newsize *= 2;
		b->data = realloc(b->data, newsize);
		if (!b->data)
			I_Error("Out of memory for render data");
		b->size = newsize;
	}
	b->used = off + len;
	return off;
}

static size_t RT_BufferCopy(rtbuf_t *b, const void *src, size_t len)
{
	size_t off;

	if (!src)
		return RT_NULL;
	off = RT_BufferAlloc(b, len);
	memcpy(b->data + off, src, len);
	return off;
}

static void *RT_Ptr(rtbuf_t *b, size_t off)
{
	return (off == RT_NULL) ? NULL : b->data + off;
}

#endif
