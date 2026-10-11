// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 2014-2023 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file ogl_vita.c
/// \brief vitaGL specific part of the OpenGL API for SRB2

#include "SDL.h"

#include "../doomdef.h"

#ifdef HWRENDER
#include "../hardware/r_opengl/r_opengl.h"
#include "../hardware/hw_main.h"
#include "../sdl/ogl_sdl.h"
#include "../i_system.h"
#include "../m_argv.h"

PFNglClear pglClear;
PFNglGetIntegerv pglGetIntegerv;
PFNglGetString pglGetString;

INT32 oglflags = 0;

void *GetGLFunc(const char *proc)
{
	return vglGetProcAddress(proc);
}

boolean LoadGL(void)
{
	return SetupGLfunc();
}

boolean OglSdlSurface(INT32 w, INT32 h)
{
	static boolean first_init = false;

	oglflags = 0;

	if (!first_init)
	{
		gl_version = pglGetString(GL_VERSION);
		gl_renderer = pglGetString(GL_RENDERER);
		gl_extensions = pglGetString(GL_EXTENSIONS);

		CONS_Printf("OpenGL %s\n", gl_version);
		CONS_Printf("GPU: %s\n", gl_renderer);
	}
	first_init = true;

	maximumAnisotropy = 1;
	supportMipMap = true;

	SetupGLFunc4();

	glanisotropicmode_cons_t[1].value = maximumAnisotropy;

	VGL_SetupSceneTarget(w, h);

	SetModelView(w, h);
	SetStates();
	pglClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

	HWR_Startup();
	// RGB5_A1 forces vitaGL to repack every upload on the CPU
	textureformatGL = GL_RGBA;

	return true;
}

void OglSdlFinishUpdate(boolean waitvbl)
{
	(void)waitvbl;

	VGL_PresentScene(realwidth, realheight);

	GClipRect(0, 0, screen_width, screen_height, NZCLIP_PLANE);
}

EXPORT void HWRAPI(OglSdlSetPalette) (RGBA_t *palette)
{
	size_t palsize = (sizeof(RGBA_t) * 256);
	// on a palette change, you have to reload all of the textures
	if (memcmp(&myPaletteData, palette, palsize))
	{
		memcpy(&myPaletteData, palette, palsize);
		Flush();
	}
}

#endif //HWRENDER
