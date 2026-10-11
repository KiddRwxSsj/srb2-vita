// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vita_rthread.c
/// \brief Runs the OpenGL backend on a second core for PS Vita
///
///	Every HWD call made while drawing a frame is recorded with copies of its
///	data. At the end of the frame the list goes to a thread on another core,
///	which replays it through vitaGL and presents, while the game builds the
///	next frame. Calls that return data or change texture state the game reads
///	back (uploads, deletes, readbacks) wait for the thread and run directly.

#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include "SDL.h"

#include "../doomdef.h"
#include "../i_system.h"
#include "../hardware/hw_main.h"
#include "../hardware/hw_drv.h"
#include "../hardware/hw_batching.h"
#include "../sdl/ogl_sdl.h"
#include "../hardware/r_opengl/r_opengl.h"
#include "vita_debug.h"
#include "vita_rthread.h"

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

boolean vita_rt_enabled = true;

static boolean rtactive = false;
static SceUID rtthread = -1, rtwork = -1, rtdone = -1;
static rtbuf_t rtbufs[2];
static INT32 rtcur = 0;
static rtbuf_t *rtsubmitted = NULL;
static struct hwdriver_s rtreal;

// GL names for recorded uploads, reserved in batches while the thread is idle
#define RT_NAMES 64
static GLuint rtnames[RT_NAMES];
static INT32 rtnumnames = 0;

static UINT32 *rtsort = NULL;
static size_t rtsortsize = 0;
static PolygonArrayEntry *rtsortpolys;
static FOutVector *rtverts = NULL;
static UINT32 *rtindices = NULL;
static size_t rtvertsize = 0, rtindexsize = 0;

static rtcmd_t *RT_Cmd(UINT32 op)
{
	rtbuf_t *b = &rtbufs[rtcur];

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
static size_t RT_Alloc(size_t len)
{
	rtbuf_t *b = &rtbufs[rtcur];
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

static size_t RT_Copy(const void *src, size_t len)
{
	size_t off;

	if (!src)
		return RT_NULL;
	off = RT_Alloc(len);
	memcpy(rtbufs[rtcur].data + off, src, len);
	return off;
}

static void *RT_Ptr(rtbuf_t *b, size_t off)
{
	return (off == RT_NULL) ? NULL : b->data + off;
}

static void RT_BindTexture(UINT32 name)
{
	GLMipmap_t bind;

	if (!name)
	{
		rtreal.pfnSetTexture(NULL);
		return;
	}
	memset(&bind, 0, sizeof bind);
	bind.downloaded = name;
	rtreal.pfnSetTexture(&bind);
}

static void *RT_Grow(void *p, size_t *size, size_t need, size_t elem)
{
	if (need <= *size)
		return p;
	while (*size < need)
		*size = *size ? *size * 2 : 65536;
	p = realloc(p, *size * elem);
	if (!p)
		I_Error("Out of memory for render batches");
	return p;
}

static int RT_ComparePolys(const void *p1, const void *p2)
{
	return rtsortpolys[*(const UINT32 *)p1].hash - rtsortpolys[*(const UINT32 *)p2].hash;
}

// Untextured polygons batch as texture 0, like a NULL mipmap
#define RT_POLYTEX(p) (((p)->polyFlags & PF_NoTexture) ? 0 : (p)->texname)

// HWR_RenderBatches without the shader paths, which are off on Vita. Texture
// names were stored when the polygons were collected, so the mipmaps the
// game owns are never touched here.
static void RT_DrawBatches(rtbuf_t *b, rtcmd_t *c)
{
	PolygonArrayEntry *polys = RT_Ptr(b, c->u.batch.polys), *p, *next = NULL;
	FOutVector *verts = RT_Ptr(b, c->u.batch.verts);
	INT32 num = c->u.batch.num, i;
	UINT32 n, nverts = 0, nindices = 0, curtex;
	FBITFIELD curflags;
	FSurfaceInfo cursurf;
	INT32 calls = 0, numverts = 0, textures = 1, polyflags = 1, colors = 1;

	rtsort = RT_Grow(rtsort, &rtsortsize, num, sizeof *rtsort);
	for (i = 0; i < num; i++)
		rtsort[i] = i;
	rtsortpolys = polys;
	qsort(rtsort, num, sizeof *rtsort, RT_ComparePolys);

	p = &polys[rtsort[0]];
	curtex = RT_POLYTEX(p);
	curflags = p->polyFlags;
	cursurf = p->surf;
	if (!(curflags & PF_NoTexture))
		RT_BindTexture(curtex);

	for (i = 0; i < num; i++)
	{
		p = &polys[rtsort[i]];
		rtverts = RT_Grow(rtverts, &rtvertsize, nverts + p->numVerts, sizeof *rtverts);
		rtindices = RT_Grow(rtindices, &rtindexsize, nindices + p->numVerts * 3, sizeof *rtindices);
		memcpy(&rtverts[nverts], &verts[p->vertsIndex], p->numVerts * sizeof *verts);
		for (n = 2; n < p->numVerts; n++)
		{
			rtindices[nindices++] = nverts;
			rtindices[nindices++] = nverts + n - 1;
			rtindices[nindices++] = nverts + n;
		}
		nverts += p->numVerts;

		if (i + 1 < num)
		{
			next = &polys[rtsort[i + 1]];
			if (next->hash == p->hash
				|| (RT_POLYTEX(next) == curtex && next->polyFlags == curflags
				&& next->surf.PolyColor.rgba == cursurf.PolyColor.rgba))
				continue;
		}

		rtreal.pfnDrawIndexedTriangles(&cursurf, rtverts, nindices, curflags, rtindices);
		calls++;
		numverts += nindices;
		nverts = nindices = 0;

		if (i + 1 == num)
			break;
		if (RT_POLYTEX(next) != curtex)
		{
			curtex = RT_POLYTEX(next);
			RT_BindTexture(curtex);
			textures++;
		}
		if (next->polyFlags != curflags)
		{
			curflags = next->polyFlags;
			polyflags++;
		}
		if (next->surf.PolyColor.rgba != cursurf.PolyColor.rgba)
		{
			cursurf = next->surf;
			colors++;
		}
	}

	ps_hw_numcalls.value.i = calls;
	ps_hw_numverts.value.i = numverts;
	ps_hw_numshaders.value.i = 1;
	ps_hw_numtextures.value.i = textures;
	ps_hw_numpolyflags.value.i = polyflags;
	ps_hw_numcolors.value.i = colors;
}

static void RT_Replay(rtbuf_t *b)
{
	size_t i;

	for (i = 0; i < b->numcmds; i++)
	{
		rtcmd_t *c = &b->cmds[i];

		switch (c->op)
		{
			case RT_SETTEXTURE:
				RT_BindTexture(c->u.texname);
				break;
			case RT_UPLOAD:
			{
				GLMipmap_t *tex = RT_Ptr(b, c->u.upload.tex);
				tex->data = RT_Ptr(b, c->u.upload.data);
				rtreal.pfnUpdateTexture(tex);
				break;
			}
			case RT_DRAWPOLYGON:
				rtreal.pfnDrawPolygon(RT_Ptr(b, c->u.poly.surf), RT_Ptr(b, c->u.poly.verts), c->u.poly.num, c->u.poly.flags);
				break;
			case RT_DRAWINDEXED:
				rtreal.pfnDrawIndexedTriangles(RT_Ptr(b, c->u.poly.surf), RT_Ptr(b, c->u.poly.verts), c->u.poly.num, c->u.poly.flags, RT_Ptr(b, c->u.poly.indices));
				break;
			case RT_SKYDOME:
				rtreal.pfnRenderSkyDome(c->u.sky);
				break;
			case RT_SETBLEND:
				rtreal.pfnSetBlend(c->u.flags);
				break;
			case RT_CLEARBUFFER:
				rtreal.pfnClearBuffer(c->u.clear.color, c->u.clear.depth, RT_Ptr(b, c->u.clear.rgba));
				break;
			case RT_GCLIPRECT:
				rtreal.pfnGClipRect(c->u.clip.minx, c->u.clip.miny, c->u.clip.maxx, c->u.clip.maxy, c->u.clip.nearclip);
				break;
			case RT_SPECIALSTATE:
				rtreal.pfnSetSpecialState(c->u.state.id, c->u.state.value);
				break;
			case RT_SETTRANSFORM:
				rtreal.pfnSetTransform(RT_Ptr(b, c->u.data));
				break;
			case RT_DRAW2DLINE:
			{
				F2DCoord *v = RT_Ptr(b, c->u.line.coords);
				rtreal.pfnDraw2DLine(&v[0], &v[1], c->u.line.color);
				break;
			}
			case RT_POSTIMGREDRAW:
				rtreal.pfnPostImgRedraw(RT_Ptr(b, c->u.data));
				break;
			case RT_FLUSHSCREENTEX:
				rtreal.pfnFlushScreenTextures();
				break;
			case RT_SCREENWIPE:
				rtreal.pfnDoScreenWipe(c->u.screen.a, c->u.screen.b, RT_Ptr(b, c->u.screen.surf), c->u.screen.flags);
				break;
			case RT_DRAWSCREENTEX:
				rtreal.pfnDrawScreenTexture(c->u.screen.a, RT_Ptr(b, c->u.screen.surf), c->u.screen.flags);
				break;
			case RT_MAKESCREENTEX:
				rtreal.pfnMakeScreenTexture(c->u.value);
				break;
			case RT_DRAWSCREENFINAL:
				rtreal.pfnDrawScreenFinalTexture(c->u.screen.a, c->u.screen.b, c->u.screen.c);
				break;
			case RT_SETSHADER:
				rtreal.pfnSetShader(c->u.value);
				break;
			case RT_UNSETSHADER:
				rtreal.pfnUnSetShader();
				break;
			case RT_SHADERINFO:
				rtreal.pfnSetShaderInfo(c->u.state.id, c->u.state.value);
				break;
			case RT_BATCHES:
				RT_DrawBatches(b, c);
				break;
			case RT_PRESENT:
				OglSdlFinishUpdate(c->u.value);
				break;
		}
	}
}

static int RT_Thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;

	for (;;)
	{
		sceKernelWaitSema(rtwork, 1, NULL);
		RT_Replay(rtsubmitted);
		sceKernelSignalSema(rtdone, 1);
	}
	return 0;
}

// rtdone holds one count while the thread is idle. The game takes it before
// handing over a buffer, and the thread gives it back once that buffer ran.
static void RT_Submit(void)
{
	rtsubmitted = &rtbufs[rtcur];
	rtcur ^= 1;
	rtbufs[rtcur].numcmds = 0;
	rtbufs[rtcur].used = 0;
	sceKernelSignalSema(rtwork, 1);
}

// Runs everything recorded so far and keeps the thread idle until RT_Release
static void RT_Flush(void)
{
	sceKernelWaitSema(rtdone, 1, NULL);
	if (rtbufs[rtcur].numcmds)
	{
		RT_Submit();
		sceKernelWaitSema(rtdone, 1, NULL);
	}
}

static void RT_Release(void)
{
	sceKernelSignalSema(rtdone, 1);
}

#define RT_DIRECT(call) \
	do { \
		boolean wasactive = rtactive; \
		if (wasactive) RT_Flush(); \
		call; \
		if (wasactive) RT_Release(); \
	} while (0)

//
// Recorded calls
//

// Bytes per texel of the formats UpdateTexture takes
static INT32 RT_TexelSize(INT32 format)
{
	switch (format)
	{
		case GL_TEXFMT_P_8:
		case GL_TEXFMT_ALPHA_8:
			return 1;
		case GL_TEXFMT_AP_88:
		case GL_TEXFMT_ALPHA_INTENSITY_88:
			return 2;
		case GL_TEXFMT_RGBA:
			return 4;
		default:
			return 0;
	}
}

// A first upload is recorded with its source texels, so it does not stall
// the game until the thread has finished the previous frame
static boolean RT_RecordUpload(GLMipmap_t *tex)
{
	const INT32 bpp = RT_TexelSize(tex->format);
	const size_t size = (size_t)tex->width * tex->height * bpp;
	GLMipmap_t *copy;
	rtcmd_t *c;

	if (!bpp || !tex->data || !size || size > 4 * 1024 * 1024)
		return false;
	if (!rtnumnames)
	{
		RT_DIRECT(glGenTextures(RT_NAMES, rtnames));
		rtnumnames = RT_NAMES;
	}

	// UpdateTexture flags chromakeyed holes, and the game reads that back
	if ((tex->flags & TF_CHROMAKEYED) && (tex->format == GL_TEXFMT_P_8 || tex->format == GL_TEXFMT_AP_88))
	{
		const UINT8 *p = tex->data;
		size_t i;

		for (i = 0; i < size; i += bpp)
			if (p[i] == HWR_PATCHES_CHROMAKEY_COLORINDEX)
			{
				tex->flags |= TF_TRANSPARENT;
				break;
			}
	}

	tex->downloaded = rtnames[--rtnumnames];
	c = RT_Cmd(RT_UPLOAD);
	c->u.upload.tex = RT_Copy(tex, sizeof *tex);
	c->u.upload.data = RT_Copy(tex->data, size);
	copy = RT_Ptr(&rtbufs[rtcur], c->u.upload.tex);
	copy->flags |= TF_VITAFRESH;
	VGL_TrackTexture(tex);
	RT_Cmd(RT_SETTEXTURE)->u.texname = tex->downloaded;
	return true;
}

static void RT_SetTexture(GLMipmap_t *tex)
{
	if (rtactive && (!tex || tex->downloaded))
	{
		RT_Cmd(RT_SETTEXTURE)->u.texname = tex ? tex->downloaded : 0;
		return;
	}
	if (rtactive && RT_RecordUpload(tex))
		return;
	// An upload writes the new GL name back into the mipmap, and the game
	// reads it right away (batching sorts on it)
	RT_DIRECT(rtreal.pfnSetTexture(tex));
}

static void RT_DrawPolygon(FSurfaceInfo *surf, FOutVector *verts, FUINT num, FBITFIELD flags)
{
	rtcmd_t *c;

	if ((vita_dbgflags & VDBG_NODRAW) && vita_dbg_in3d)
		return;
	if (!rtactive)
	{
		rtreal.pfnDrawPolygon(surf, verts, num, flags);
		return;
	}
	c = RT_Cmd(RT_DRAWPOLYGON);
	c->u.poly.surf = RT_Copy(surf, sizeof *surf);
	c->u.poly.verts = RT_Copy(verts, num * sizeof *verts);
	c->u.poly.num = num;
	c->u.poly.flags = flags;
}

static void RT_DrawIndexedTriangles(FSurfaceInfo *surf, FOutVector *verts, FUINT num, FBITFIELD flags, UINT32 *indices)
{
	rtcmd_t *c;
	UINT32 last = 0;
	FUINT i;

	if ((vita_dbgflags & VDBG_NODRAW) && vita_dbg_in3d)
		return;
	if (!rtactive)
	{
		rtreal.pfnDrawIndexedTriangles(surf, verts, num, flags, indices);
		return;
	}
	// The batcher refills its vertex array from 0 for every batch, so only
	// the part these indices reach needs a copy
	for (i = 0; i < num; i++)
		if (indices[i] > last)
			last = indices[i];
	c = RT_Cmd(RT_DRAWINDEXED);
	c->u.poly.surf = RT_Copy(surf, sizeof *surf);
	c->u.poly.verts = RT_Copy(verts, (last + 1) * sizeof *verts);
	c->u.poly.indices = RT_Copy(indices, num * sizeof *indices);
	c->u.poly.num = num;
	c->u.poly.flags = flags;
}

static void RT_RenderSkyDome(gl_sky_t *sky)
{
	if (!rtactive)
	{
		rtreal.pfnRenderSkyDome(sky);
		return;
	}
	// gl_sky only changes in HWR_BuildSkyDome, which syncs first
	RT_Cmd(RT_SKYDOME)->u.sky = sky;
}

static void RT_SetBlend(FBITFIELD flags)
{
	if (!rtactive)
	{
		rtreal.pfnSetBlend(flags);
		return;
	}
	RT_Cmd(RT_SETBLEND)->u.flags = flags;
}

static void RT_ClearBuffer(FBOOLEAN color, FBOOLEAN depth, FRGBAFloat *rgba)
{
	rtcmd_t *c;

	if (!rtactive)
	{
		rtreal.pfnClearBuffer(color, depth, rgba);
		return;
	}
	c = RT_Cmd(RT_CLEARBUFFER);
	c->u.clear.color = color;
	c->u.clear.depth = depth;
	c->u.clear.rgba = RT_Copy(rgba, sizeof *rgba);
}

static void RT_GClipRect(INT32 minx, INT32 miny, INT32 maxx, INT32 maxy, float nearclip)
{
	rtcmd_t *c;

	if (!rtactive)
	{
		rtreal.pfnGClipRect(minx, miny, maxx, maxy, nearclip);
		return;
	}
	c = RT_Cmd(RT_GCLIPRECT);
	c->u.clip.minx = minx;
	c->u.clip.miny = miny;
	c->u.clip.maxx = maxx;
	c->u.clip.maxy = maxy;
	c->u.clip.nearclip = nearclip;
}

static void RT_SetSpecialState(hwdspecialstate_t id, INT32 value)
{
	rtcmd_t *c;

	// These two flush the texture cache, which clears names the game reads
	if (!rtactive || id == HWD_SET_TEXTUREFILTERMODE || id == HWD_SET_TEXTUREANISOTROPICMODE)
	{
		RT_DIRECT(rtreal.pfnSetSpecialState(id, value));
		return;
	}
	c = RT_Cmd(RT_SPECIALSTATE);
	c->u.state.id = id;
	c->u.state.value = value;
}

static void RT_SetTransform(FTransform *transform)
{
	if (!rtactive)
	{
		rtreal.pfnSetTransform(transform);
		return;
	}
	RT_Cmd(RT_SETTRANSFORM)->u.data = RT_Copy(transform, sizeof *transform);
}

static void RT_Draw2DLine(F2DCoord *v1, F2DCoord *v2, RGBA_t color)
{
	rtcmd_t *c;

	if (!rtactive)
	{
		rtreal.pfnDraw2DLine(v1, v2, color);
		return;
	}
	c = RT_Cmd(RT_DRAW2DLINE);
	c->u.line.coords = RT_Copy(v1, sizeof *v1);
	RT_Copy(v2, sizeof *v2);
	c->u.line.color = color;
}

static void RT_PostImgRedraw(float points[SCREENVERTS][SCREENVERTS][2])
{
	if (!rtactive)
	{
		rtreal.pfnPostImgRedraw(points);
		return;
	}
	RT_Cmd(RT_POSTIMGREDRAW)->u.data = RT_Copy(points, sizeof (float) * SCREENVERTS * SCREENVERTS * 2);
}

static void RT_FlushScreenTextures(void)
{
	if (!rtactive)
	{
		rtreal.pfnFlushScreenTextures();
		return;
	}
	RT_Cmd(RT_FLUSHSCREENTEX);
}

static void RT_DoScreenWipe(int start, int end, FSurfaceInfo *surf, FBITFIELD flags)
{
	rtcmd_t *c;

	if (!rtactive)
	{
		rtreal.pfnDoScreenWipe(start, end, surf, flags);
		return;
	}
	c = RT_Cmd(RT_SCREENWIPE);
	c->u.screen.a = start;
	c->u.screen.b = end;
	c->u.screen.surf = RT_Copy(surf, sizeof *surf);
	c->u.screen.flags = flags;
}

static void RT_DrawScreenTexture(int tex, FSurfaceInfo *surf, FBITFIELD flags)
{
	rtcmd_t *c;

	if (!rtactive)
	{
		rtreal.pfnDrawScreenTexture(tex, surf, flags);
		return;
	}
	c = RT_Cmd(RT_DRAWSCREENTEX);
	c->u.screen.a = tex;
	c->u.screen.surf = RT_Copy(surf, sizeof *surf);
	c->u.screen.flags = flags;
}

static void RT_MakeScreenTexture(int tex)
{
	if (!rtactive)
	{
		rtreal.pfnMakeScreenTexture(tex);
		return;
	}
	RT_Cmd(RT_MAKESCREENTEX)->u.value = tex;
}

static void RT_DrawScreenFinalTexture(int tex, int width, int height)
{
	rtcmd_t *c;

	if (!rtactive)
	{
		rtreal.pfnDrawScreenFinalTexture(tex, width, height);
		return;
	}
	c = RT_Cmd(RT_DRAWSCREENFINAL);
	c->u.screen.a = tex;
	c->u.screen.b = width;
	c->u.screen.c = height;
}

static void RT_SetShader(int slot)
{
	if (!rtactive)
	{
		rtreal.pfnSetShader(slot);
		return;
	}
	RT_Cmd(RT_SETSHADER)->u.value = slot;
}

static void RT_UnSetShader(void)
{
	if (!rtactive)
	{
		rtreal.pfnUnSetShader();
		return;
	}
	RT_Cmd(RT_UNSETSHADER);
}

static void RT_SetShaderInfo(hwdshaderinfo_t info, INT32 value)
{
	rtcmd_t *c;

	if (!rtactive)
	{
		rtreal.pfnSetShaderInfo(info, value);
		return;
	}
	c = RT_Cmd(RT_SHADERINFO);
	c->u.state.id = info;
	c->u.state.value = value;
}

// Takes over HWR_RenderBatches: the sort and the draw list are built on the
// render thread from a copy of the collected polygons
boolean VITA_RT_RenderBatches(PolygonArrayEntry *polys, INT32 numpolys, FOutVector *verts, INT32 numverts)
{
	rtcmd_t *c;

	if (!rtactive || (cv_glshaders.value && gl_shadersavailable))
		return false;
	if ((vita_dbgflags & VDBG_NODRAW) && vita_dbg_in3d)
		return true;

	c = RT_Cmd(RT_BATCHES);
	c->u.batch.num = numpolys;
	c->u.batch.polys = RT_Copy(polys, numpolys * sizeof *polys);
	c->u.batch.verts = RT_Copy(verts, numverts * sizeof *verts);
	return true;
}

//
// Calls that wait for the thread and run directly
//

static void RT_SetTexturePalette(RGBA_t *pal)
{
	RT_DIRECT(rtreal.pfnSetTexturePalette(pal));
}

static void RT_UpdateTexture(GLMipmap_t *tex)
{
	RT_DIRECT(rtreal.pfnUpdateTexture(tex));
}

static void RT_DeleteTexture(GLMipmap_t *tex)
{
	RT_DIRECT(rtreal.pfnDeleteTexture(tex));
}

static void RT_ReadScreenTexture(int tex, UINT8 *dst)
{
	RT_DIRECT(rtreal.pfnReadScreenTexture(tex, dst));
}

static void RT_ClearMipMapCache(void)
{
	RT_DIRECT(rtreal.pfnClearMipMapCache());
}

static void RT_DrawModel(model_t *model, INT32 frame, float duration, float tics, INT32 nextframe, FTransform *pos, float hscale, float vscale, UINT8 flipped, UINT8 hflipped, FSurfaceInfo *surf)
{
	RT_DIRECT(rtreal.pfnDrawModel(model, frame, duration, tics, nextframe, pos, hscale, vscale, flipped, hflipped, surf));
}

static void RT_CreateModelVBOs(model_t *model)
{
	RT_DIRECT(rtreal.pfnCreateModelVBOs(model));
}

static INT32 RT_GetTextureUsed(void)
{
	INT32 used;
	RT_DIRECT(used = rtreal.pfnGetTextureUsed());
	return used;
}

static boolean RT_InitShaders(void)
{
	boolean ok;
	RT_DIRECT(ok = rtreal.pfnInitShaders());
	return ok;
}

static void RT_LoadShader(int slot, char *code, hwdshaderstage_t stage)
{
	RT_DIRECT(rtreal.pfnLoadShader(slot, code, stage));
}

static boolean RT_CompileShader(int slot)
{
	boolean ok;
	RT_DIRECT(ok = rtreal.pfnCompileShader(slot));
	return ok;
}

static void RT_SetPaletteLookup(UINT8 *lut)
{
	RT_DIRECT(rtreal.pfnSetPaletteLookup(lut));
}

static UINT32 RT_CreateLightTable(RGBA_t *table)
{
	UINT32 id;
	RT_DIRECT(id = rtreal.pfnCreateLightTable(table));
	return id;
}

static void RT_UpdateLightTable(UINT32 id, RGBA_t *table)
{
	RT_DIRECT(rtreal.pfnUpdateLightTable(id, table));
}

static void RT_ClearLightTables(void)
{
	RT_DIRECT(rtreal.pfnClearLightTables());
}

static void RT_SetScreenPalette(RGBA_t *pal)
{
	RT_DIRECT(rtreal.pfnSetScreenPalette(pal));
}

// Zone memory and the lump caches, shared by the game thread and the level
// geometry worker. Created on the first Z_ call, before any thread starts.
static SceKernelLwMutexWork zonelock;
static boolean zonelockready = false;

void VITA_ZoneLock(void)
{
	if (!zonelockready)
	{
		sceKernelCreateLwMutex(&zonelock, "srb2_zone", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
		zonelockready = true;
	}
	sceKernelLockLwMutex(&zonelock, 1, NULL);
}

void VITA_ZoneUnlock(void)
{
	sceKernelUnlockLwMutex(&zonelock, 1);
}

void VITA_RT_Init(void)
{
	if (rtreal.pfnInit)
		return;

	rtreal = hwdriver;
	HWD.pfnSetTexturePalette = RT_SetTexturePalette;
	HWD.pfnDraw2DLine = RT_Draw2DLine;
	HWD.pfnDrawPolygon = RT_DrawPolygon;
	HWD.pfnDrawIndexedTriangles = RT_DrawIndexedTriangles;
	HWD.pfnRenderSkyDome = RT_RenderSkyDome;
	HWD.pfnSetBlend = RT_SetBlend;
	HWD.pfnClearBuffer = RT_ClearBuffer;
	HWD.pfnSetTexture = RT_SetTexture;
	HWD.pfnUpdateTexture = RT_UpdateTexture;
	HWD.pfnDeleteTexture = RT_DeleteTexture;
	HWD.pfnReadScreenTexture = RT_ReadScreenTexture;
	HWD.pfnGClipRect = RT_GClipRect;
	HWD.pfnClearMipMapCache = RT_ClearMipMapCache;
	HWD.pfnSetSpecialState = RT_SetSpecialState;
	HWD.pfnDrawModel = RT_DrawModel;
	HWD.pfnCreateModelVBOs = RT_CreateModelVBOs;
	HWD.pfnSetTransform = RT_SetTransform;
	HWD.pfnGetTextureUsed = RT_GetTextureUsed;
	HWD.pfnPostImgRedraw = RT_PostImgRedraw;
	HWD.pfnFlushScreenTextures = RT_FlushScreenTextures;
	HWD.pfnDoScreenWipe = RT_DoScreenWipe;
	HWD.pfnDrawScreenTexture = RT_DrawScreenTexture;
	HWD.pfnMakeScreenTexture = RT_MakeScreenTexture;
	HWD.pfnDrawScreenFinalTexture = RT_DrawScreenFinalTexture;
	HWD.pfnInitShaders = RT_InitShaders;
	HWD.pfnLoadShader = RT_LoadShader;
	HWD.pfnCompileShader = RT_CompileShader;
	HWD.pfnSetShader = RT_SetShader;
	HWD.pfnUnSetShader = RT_UnSetShader;
	HWD.pfnSetShaderInfo = RT_SetShaderInfo;
	HWD.pfnSetPaletteLookup = RT_SetPaletteLookup;
	HWD.pfnCreateLightTable = RT_CreateLightTable;
	HWD.pfnUpdateLightTable = RT_UpdateLightTable;
	HWD.pfnClearLightTables = RT_ClearLightTables;
	HWD.pfnSetScreenPalette = RT_SetScreenPalette;

	rtwork = sceKernelCreateSema("srb2_rtwork", 0, 0, 1, NULL);
	rtdone = sceKernelCreateSema("srb2_rtdone", 0, 1, 1, NULL);
	rtthread = sceKernelCreateThread("srb2_render", RT_Thread, 0x10000100, 512 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
	if (rtwork < 0 || rtdone < 0 || rtthread < 0 || sceKernelStartThread(rtthread, 0, NULL) < 0)
	{
		CONS_Alert(CONS_WARNING, "Could not start the render thread\n");
		rtthread = -1;
		vita_rt_enabled = false;
		return;
	}
	// Keep the game on its own core so the two never share one
	sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
}

void VITA_RT_FinishUpdate(INT32 waitvbl)
{
	if (!rtactive)
	{
		OglSdlFinishUpdate(waitvbl);
		// Only switch on between frames, with nothing recorded yet
		if (vita_rt_enabled && rtthread >= 0)
			rtactive = true;
		return;
	}

	RT_Cmd(RT_PRESENT)->u.value = waitvbl;
	sceKernelWaitSema(rtdone, 1, NULL);
	RT_Submit();

	if (!vita_rt_enabled)
		VITA_RT_Stop();
}

void VITA_RT_Sync(void)
{
	if (!rtactive)
		return;
	RT_Flush();
	RT_Release();
}

void VITA_RT_Stop(void)
{
	if (!rtactive)
		return;
	if (sceKernelGetThreadId() == rtthread)
	{
		// I_Error from inside a replayed call; the game thread is blocked or
		// recording, so just let this thread finish on its own
		rtactive = false;
		return;
	}
	RT_Flush();
	rtactive = false;
	RT_Release();
}
