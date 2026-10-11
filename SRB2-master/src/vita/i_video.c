// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 2014-2023 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file i_video.c
/// \brief PS Vita video and input: vitaGL for display, SDL joystick for pads

#include <stdlib.h>

#include <psp2/display.h>
#include <psp2/power.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/common_dialog.h>
#include <psp2/message_dialog.h>
#include <vitaGL.h>

#include "SDL.h"

#include "../doomdef.h"
#include "../doomstat.h"
#include "../i_system.h"
#include "../v_video.h"
#include "../m_argv.h"
#include "../m_menu.h"
#include "../d_main.h"
#include "../s_sound.h"
#include "../i_joy.h"
#include "../st_stuff.h"
#include "../hu_stuff.h"
#include "../g_game.h"
#include "../i_video.h"
#include "../console.h"
#include "../command.h"
#include "../r_main.h"
#include "../r_fps.h"
#include "../lua_hook.h"
#include "../sdl/sdlmain.h"
#ifdef HWRENDER
#include "../hardware/hw_main.h"
#include "../hardware/hw_drv.h"
#include "../hardware/r_opengl/r_opengl.h"
#include "../sdl/hwsym_sdl.h"
#include "../sdl/ogl_sdl.h"
#include "vita_debug.h"
#include "vita_rthread.h"
#endif

#define DISPLAY_W 960
#define DISPLAY_H 544

rendermode_t rendermode = render_opengl;
rendermode_t chosenrendermode = render_none;

static void VidWaitChanged(void);
consvar_t cv_vidwait = CVAR_INIT ("vid_wait", "On", CV_SAVE|CV_CALL, CV_OnOff, VidWaitChanged);

UINT8 graphics_started = 0;
boolean allow_fullscreen = false;

SDL_Window *window = NULL;

Uint16 realwidth = DISPLAY_W;
Uint16 realheight = DISPLAY_H;

static boolean vgl_started = false;

static const INT32 vitaModes[][2] =
{
	{960, 544},
	{720, 408},
	{640, 368},
	{480, 272},
};
#define NUMVITAMODES ((INT32)(sizeof (vitaModes) / sizeof (vitaModes[0])))
static char vidModeName[NUMVITAMODES][16];

static UINT32 swPalette[256];
static UINT32 *swBuffer = NULL;
static GLuint swTexture = 0;
static GLuint swFBO = 0;
static INT32 swTexWidth = 0, swTexHeight = 0;

static volatile UINT32 flipInterval = 2;
static UINT32 lastFlipVcount = 0;

// Screens without a level run at 60 Hz with "Match refresh rate": their
// animations step at 35 Hz, and 30 Hz drops a step about every sixth frame.
// Flips that come late put them back to 30 Hz for a while.
static volatile UINT32 lateFlips = 0;
static INT32 menuPacingFrames = 0, menuPacingBackoff = 0;
static UINT32 clockCheckFrames = 0;

// The firmware puts its default clocks (333/111) back after a suspend
void VID_ApplyClocks(void)
{
	if (scePowerGetArmClockFrequency() != 444)
		scePowerSetArmClockFrequency(444);
	if (scePowerGetBusClockFrequency() != 222)
		scePowerSetBusClockFrequency(222);
	if (scePowerGetGpuClockFrequency() != 222)
		scePowerSetGpuClockFrequency(222);
	if (scePowerGetGpuXbarClockFrequency() != 166)
		scePowerSetGpuXbarClockFrequency(166);
}

// Runs on the GXM display queue thread right before sceDisplaySetFrameBuf.
// Holding the flip until N vblanks have passed since the previous one
// gives evenly spaced frames at 60/N Hz without blocking the game thread.
static void VID_DisplayCallback(void *framebuf)
{
	UINT32 interval = flipInterval;
	(void)framebuf;

	if (interval)
	{
		INT32 elapsed;
		// a negative count means the vblank counter restarted, don't wait on it
		if (interval == 1 && (INT32)(sceDisplayGetVcount() - lastFlipVcount) > 1)
			lateFlips++;
		while ((elapsed = (INT32)(sceDisplayGetVcount() - lastFlipVcount)) >= 0 && elapsed < (INT32)interval)
			sceDisplayWaitVblankStart();
	}
	lastFlipVcount = sceDisplayGetVcount();
}

static boolean VID_MenuPacing(void)
{
	return cv_vidwait.value && cv_fpscap.value == 0 && gamestate != GS_LEVEL && !menuPacingBackoff;
}

// Allows a late flip now and then (loads, wipes), not a steady stream
static void VID_CheckMenuPacing(void)
{
	if (menuPacingBackoff)
	{
		menuPacingBackoff--;
		return;
	}
	if (!VID_MenuPacing())
	{
		menuPacingFrames = 0;
		lateFlips = 0;
		return;
	}
	if (++menuPacingFrames >= 60)
	{
		if (lateFlips > 10)
			menuPacingBackoff = 300;
		menuPacingFrames = 0;
		lateFlips = 0;
	}
}

static void VID_UpdateFlipInterval(void)
{
	UINT32 cap = R_GetFramerateCap();

	if (!cv_vidwait.value)
		flipInterval = 0;
	else if (cap && cap <= 30)
		flipInterval = 2;
	else
		flipInterval = 1;
}

static void VidWaitChanged(void)
{
	VID_UpdateFlipInterval();
}

static void VID_StartupVitaGL(void)
{
	if (vgl_started)
		return;

	vglInitExtended(0, DISPLAY_W, DISPLAY_H, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
	vglWaitVblankStart(GL_FALSE);
	vglSetDisplayCallback(VID_DisplayCallback);
	lastFlipVcount = sceDisplayGetVcount();
	vgl_started = true;

	CONS_Printf("Clocks: CPU %d, bus %d, GPU %d, xbar %d MHz\n",
		scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
		scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
}

//
// Joystick
//

// SDL's Vita driver reports Triangle, Circle, Cross, Square, L, R, Down, Left, Up,
// Right, Select, Start, L2, R2, L3, R3. The game's menus and default binds expect
// an XInput style layout with the d-pad on the hat keys, so translate here.
static const INT32 vitaButtonMap[] =
{
	3,             // Triangle -> Y
	1,             // Circle   -> B
	0,             // Cross    -> A
	2,             // Square   -> X
	4,             // L        -> LB
	5,             // R        -> RB
	-(1 + 1),      // Down     -> hat down
	-(1 + 2),      // Left     -> hat left
	-(1 + 0),      // Up       -> hat up
	-(1 + 3),      // Right    -> hat right
	6,             // Select   -> Back
	7,             // Start    -> Start
	10,            // L2
	11,            // R2
	8,             // L3       -> left stick
	9,             // R3       -> right stick
};
#define NUMVITABUTTONS ((INT32)(sizeof (vitaButtonMap) / sizeof (vitaButtonMap[0])))

static INT32 VID_JoyAxis(Sint16 axis, evtype_t which)
{
	// -32768 to 32767
	INT32 raxis = axis/32;
	if (which == ev_joystick)
	{
		if (Joystick.bGamepadStyle)
		{
			if (raxis < -(JOYAXISRANGE/2))
				raxis = -1;
			else if (raxis > (JOYAXISRANGE/2))
				raxis = 1;
			else
				raxis = 0;
		}
		else
			raxis = JoyInfo.scale!=1?((raxis/JoyInfo.scale)*JoyInfo.scale):raxis;
	}
	else if (which == ev_joystick2)
	{
		if (Joystick2.bGamepadStyle)
		{
			if (raxis < -(JOYAXISRANGE/2))
				raxis = -1;
			else if (raxis > (JOYAXISRANGE/2))
				raxis = 1;
			else
				raxis = 0;
		}
		else
			raxis = JoyInfo2.scale!=1?((raxis/JoyInfo2.scale)*JoyInfo2.scale):raxis;
	}
	return raxis;
}

static void Impl_HandleJoystickAxisEvent(SDL_JoyAxisEvent evt)
{
	event_t event;
	SDL_JoystickID joyid[2];
	Sint32 value = evt.value;

	joyid[0] = SDL_JoystickInstanceID(JoyInfo.dev);
	joyid[1] = SDL_JoystickInstanceID(JoyInfo2.dev);

	// Triggers (PS TV pads) rest at -32768; fold them into a 0..32767 range
	if (evt.axis >= 4)
		value = (value + 32768) / 2;

	evt.axis++;
	event.key = event.x = event.y = INT32_MAX;

	if (evt.which == joyid[0])
		event.type = ev_joystick;
	else if (evt.which == joyid[1])
		event.type = ev_joystick2;
	else
		return;

	if (evt.axis > JOYAXISSET*2)
		return;

	if (evt.axis%2)
	{
		event.key = evt.axis / 2;
		event.x = VID_JoyAxis((Sint16)value, event.type);
	}
	else
	{
		evt.axis--;
		event.key = evt.axis / 2;
		event.y = VID_JoyAxis((Sint16)value, event.type);
	}
	D_PostEvent(&event);
}

static void Impl_HandleJoystickButtonEvent(SDL_JoyButtonEvent evt, Uint32 type)
{
	event_t event;
	SDL_JoystickID joyid[2];
	INT32 mapped;
	boolean second;

	joyid[0] = SDL_JoystickInstanceID(JoyInfo.dev);
	joyid[1] = SDL_JoystickInstanceID(JoyInfo2.dev);

	if (evt.which == joyid[0])
		second = false;
	else if (evt.which == joyid[1])
		second = true;
	else
		return;

	if (type == SDL_JOYBUTTONUP)
		event.type = ev_keyup;
	else if (type == SDL_JOYBUTTONDOWN)
		event.type = ev_keydown;
	else
		return;

	if (evt.button >= NUMVITABUTTONS)
		return;

	mapped = vitaButtonMap[evt.button];
	if (mapped < 0)
		event.key = (second ? KEY_2HAT1 : KEY_HAT1) + (-mapped - 1);
	else
		event.key = (second ? KEY_2JOY1 : KEY_JOY1) + mapped;

	D_PostEvent(&event);
}

static void Impl_HandleJoystickAdded(INT32 which)
{
	SDL_Joystick *newjoy = SDL_JoystickOpen(which);

	CONS_Debug(DBG_GAMELOGIC, "Joystick device index %d added\n", which + 1);

	if (newjoy && (!JoyInfo.dev || !SDL_JoystickGetAttached(JoyInfo.dev))
		&& JoyInfo2.dev != newjoy)
	{
		cv_usejoystick.value = which + 1;

		if (JoyInfo2.dev)
			cv_usejoystick2.value = I_GetJoystickDeviceIndex(JoyInfo2.dev) + 1;
		else if (atoi(cv_usejoystick2.string) != JoyInfo.oldjoy
				&& atoi(cv_usejoystick2.string) != cv_usejoystick.value)
			cv_usejoystick2.value = atoi(cv_usejoystick2.string);
		else if (atoi(cv_usejoystick.string) != JoyInfo.oldjoy
				&& atoi(cv_usejoystick.string) != cv_usejoystick.value)
			cv_usejoystick2.value = atoi(cv_usejoystick.string);
		else
			cv_usejoystick2.value = 0;
	}
	else if (newjoy && (!JoyInfo2.dev || !SDL_JoystickGetAttached(JoyInfo2.dev))
		&& JoyInfo.dev != newjoy)
	{
		cv_usejoystick2.value = which + 1;

		if (JoyInfo.dev)
			cv_usejoystick.value = I_GetJoystickDeviceIndex(JoyInfo.dev) + 1;
		else if (atoi(cv_usejoystick.string) != JoyInfo2.oldjoy
				&& atoi(cv_usejoystick.string) != cv_usejoystick2.value)
			cv_usejoystick.value = atoi(cv_usejoystick.string);
		else if (atoi(cv_usejoystick2.string) != JoyInfo2.oldjoy
				&& atoi(cv_usejoystick2.string) != cv_usejoystick2.value)
			cv_usejoystick.value = atoi(cv_usejoystick2.string);
		else
			cv_usejoystick.value = 0;
	}

	if (!strcmp(cv_usejoystick.string, "0") || !cv_usejoystick.value)
		cv_usejoystick.value = 0;
	else if (atoi(cv_usejoystick.string) <= I_NumJoys() && cv_usejoystick.value)
		CV_SetValue(&cv_usejoystick, cv_usejoystick.value);

	if (!strcmp(cv_usejoystick2.string, "0") || !cv_usejoystick2.value)
		cv_usejoystick2.value = 0;
	else if (atoi(cv_usejoystick2.string) <= I_NumJoys() && cv_usejoystick2.value)
		CV_SetValue(&cv_usejoystick2, cv_usejoystick2.value);

	I_InitJoystick();
	I_InitJoystick2();

	if (currentMenu == &OP_JoystickSetDef)
		M_SetupJoystickMenu(0);

	if (JoyInfo.dev != newjoy && JoyInfo2.dev != newjoy)
		SDL_JoystickClose(newjoy);
}

static void Impl_HandleJoystickRemoved(void)
{
	if (JoyInfo.dev && !SDL_JoystickGetAttached(JoyInfo.dev))
		I_ShutdownJoystick();

	if (JoyInfo2.dev && !SDL_JoystickGetAttached(JoyInfo2.dev))
		I_ShutdownJoystick2();

	if (JoyInfo.dev)
		cv_usejoystick.value = JoyInfo.oldjoy = I_GetJoystickDeviceIndex(JoyInfo.dev) + 1;
	else if (atoi(cv_usejoystick.string) != JoyInfo2.oldjoy)
		cv_usejoystick.value = atoi(cv_usejoystick.string);
	else if (atoi(cv_usejoystick2.string) != JoyInfo2.oldjoy)
		cv_usejoystick.value = atoi(cv_usejoystick2.string);
	else
		cv_usejoystick.value = 0;

	if (JoyInfo2.dev)
		cv_usejoystick2.value = JoyInfo2.oldjoy = I_GetJoystickDeviceIndex(JoyInfo2.dev) + 1;
	else if (atoi(cv_usejoystick2.string) != JoyInfo.oldjoy)
		cv_usejoystick2.value = atoi(cv_usejoystick2.string);
	else if (atoi(cv_usejoystick.string) != JoyInfo.oldjoy)
		cv_usejoystick2.value = atoi(cv_usejoystick.string);
	else
		cv_usejoystick2.value = 0;

	if (!strcmp(cv_usejoystick.string, "0"))
		cv_usejoystick.value = 0;
	else if (atoi(cv_usejoystick.string) <= I_NumJoys() && cv_usejoystick.value)
		CV_SetValue(&cv_usejoystick, cv_usejoystick.value);

	if (!strcmp(cv_usejoystick2.string, "0"))
		cv_usejoystick2.value = 0;
	else if (atoi(cv_usejoystick2.string) <= I_NumJoys() && cv_usejoystick2.value)
		CV_SetValue(&cv_usejoystick2, cv_usejoystick2.value);

	if (currentMenu == &OP_JoystickSetDef)
		M_SetupJoystickMenu(0);
}

void I_GetEvent(void)
{
	SDL_Event evt;

	if (!graphics_started)
		return;

	VITA_DebugPollTouch();

	while (SDL_PollEvent(&evt))
	{
		switch (evt.type)
		{
			case SDL_JOYAXISMOTION:
				Impl_HandleJoystickAxisEvent(evt.jaxis);
				break;
			case SDL_JOYBUTTONUP:
			case SDL_JOYBUTTONDOWN:
				Impl_HandleJoystickButtonEvent(evt.jbutton, evt.type);
				break;
			case SDL_JOYDEVICEADDED:
				Impl_HandleJoystickAdded(evt.jdevice.which);
				break;
			case SDL_JOYDEVICEREMOVED:
				Impl_HandleJoystickRemoved();
				break;
			case SDL_QUIT:
				LUA_HookBool(true, HOOK(GameQuit));
				I_Quit();
				break;
		}
	}
}

void I_StartupMouse(void)
{
}

void I_UpdateMouseGrab(void)
{
}

void I_SetMouseGrab(boolean grab)
{
	(void)grab;
}

void SDLforceUngrabMouse(void)
{
}

void I_GetCursorPosition(INT32 *x, INT32 *y)
{
	*x = *y = 0;
}

void I_OsPolling(void)
{
	if (SDL_WasInit(SDL_INIT_JOYSTICK) == SDL_INIT_JOYSTICK)
	{
		SDL_JoystickUpdate();
		I_GetJoystickEvents();
		I_GetJoystick2Events();
	}

	I_GetEvent();
}

//
// Video modes
//

static void VID_Command_NumModes_f(void)
{
	CONS_Printf(M_GetText("%d video mode(s) available(s)\n"), VID_NumModes());
}

static void VID_Command_Info_f(void)
{
	CONS_Printf("vitaGL %dx%d, rendering at %dx%d\n", DISPLAY_W, DISPLAY_H, vid.width, vid.height);
	CONS_Printf("RAM free: %u KB, VRAM free: %u KB\n",
		(unsigned)(vglMemFree(VGL_MEM_RAM) >> 10), (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 10));
}

static void VID_Command_ModeList_f(void)
{
	INT32 i;
	for (i = 0; i < NUMVITAMODES; i++)
		CONS_Printf("%2d: %dx%d\n", i, vitaModes[i][0], vitaModes[i][1]);
}

static void VID_Command_Mode_f(void)
{
	INT32 modenum;

	if (COM_Argc() != 2)
	{
		CONS_Printf(M_GetText("vid_mode <modenum> : set video mode, current video mode %i\n"), vid.modenum);
		return;
	}

	modenum = atoi(COM_Argv(1));

	if (modenum >= VID_NumModes())
		CONS_Printf(M_GetText("Video mode not present\n"));
	else
		setmodeneeded = modenum+1;
}

INT32 VID_NumModes(void)
{
	return NUMVITAMODES;
}

const char *VID_GetModeName(INT32 modeNum)
{
	if (modeNum < 0 || modeNum >= NUMVITAMODES)
		return NULL;

	sprintf(&vidModeName[modeNum][0], "%dx%d", vitaModes[modeNum][0], vitaModes[modeNum][1]);
	return &vidModeName[modeNum][0];
}

INT32 VID_GetModeForSize(INT32 w, INT32 h)
{
	INT32 i;
	for (i = 0; i < NUMVITAMODES; i++)
	{
		if (vitaModes[i][0] == w && vitaModes[i][1] == h)
			return i;
	}
	return -1;
}

void VID_PrepareModeList(void)
{
	allow_fullscreen = true;
}

//
// Software renderer output
//

static void VID_ReleaseSoftwareBuffers(void)
{
	if (swFBO)
		glDeleteFramebuffers(1, &swFBO);
	if (swTexture)
		glDeleteTextures(1, &swTexture);
	swTexture = swFBO = 0;
	free(swBuffer);
	swBuffer = NULL;
	swTexWidth = swTexHeight = 0;
}

static void VID_SetupSoftwareBuffers(void)
{
	if (swTexture && swTexWidth == vid.width && swTexHeight == vid.height)
		return;

	VID_ReleaseSoftwareBuffers();

	swBuffer = malloc(vid.width * vid.height * sizeof (UINT32));
	if (!swBuffer)
		I_Error("%s", M_GetText("Not enough memory for video buffer\n"));

	glGenTextures(1, &swTexture);
	glBindTexture(GL_TEXTURE_2D, swTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vid.width, vid.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	// Only used as a blit source, which skips the fixed function pipeline entirely
	glGenFramebuffers(1, &swFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, swFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, swTexture, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	swTexWidth = vid.width;
	swTexHeight = vid.height;
}

static void VID_PresentSoftware(void)
{
	const UINT8 *src = screens[0];
	UINT32 *dst;
	INT32 x, y;

	if (!src)
		return;

	VID_SetupSoftwareBuffers();

	// Framebuffer textures are bottom-up, so rows are written in reverse
	for (y = vid.height - 1; y >= 0; y--)
	{
		dst = swBuffer + y * vid.width;
		for (x = 0; x < vid.width; x++)
			*dst++ = swPalette[*src++];
	}

	glBindTexture(GL_TEXTURE_2D, swTexture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vid.width, vid.height, GL_RGBA, GL_UNSIGNED_BYTE, swBuffer);

	glBlitNamedFramebuffer(swFBO, 0,
		0, 0, vid.width, vid.height,
		0, 0, DISPLAY_W, DISPLAY_H,
		GL_COLOR_BUFFER_BIT,
		(vid.width == DISPLAY_W && vid.height == DISPLAY_H) ? GL_NEAREST : GL_LINEAR);
	vglSwapBuffers(GL_FALSE);
}

static void VID_SetupBuffer(void)
{
	vid.rowbytes = vid.width * vid.bpp;
	vid.direct = NULL;
	free(vid.buffer);
	vid.buffer = calloc(NUMSCREENS, vid.rowbytes*vid.height);
	if (!vid.buffer)
		I_Error("%s", M_GetText("Not enough memory for video buffer\n"));
}

static void VID_ApplyMode(void)
{
#ifdef HWRENDER
	VITA_RT_Stop();
	if (rendermode == render_opengl)
	{
		VID_ReleaseSoftwareBuffers();
		OglSdlSurface(vid.width, vid.height);
	}
	else
	{
		VGL_ReleaseSceneTarget();
		VID_SetupSoftwareBuffers();
	}
#else
	VID_SetupSoftwareBuffers();
#endif
}

void VID_CheckGLLoaded(rendermode_t oldrender)
{
	(void)oldrender;
#ifdef HWRENDER
	if (vid.glstate == VID_GL_LIBRARY_ERROR)
	{
		CONS_Alert(CONS_ERROR, "OpenGL never loaded\n");
		rendermode = oldrender;
		if (chosenrendermode == render_opengl)
			rendermode = render_soft;
		if (setrenderneeded)
		{
			CV_StealthSetValue(&cv_renderer, oldrender);
			setrenderneeded = 0;
		}
	}
#endif
}

boolean VID_CheckRenderer(void)
{
	boolean rendererchanged = false;
#ifdef HWRENDER
	rendermode_t oldrenderer = rendermode;
#endif

	if (dedicated)
		return false;

	if (setrenderneeded)
	{
		rendermode = setrenderneeded;
		rendererchanged = true;

#ifdef HWRENDER
		if (rendermode == render_opengl)
		{
			VID_CheckGLLoaded(oldrenderer);

			if (vid.glstate == VID_GL_LIBRARY_NOTLOADED)
				VID_StartupOpenGL();
			else if (vid.glstate == VID_GL_LIBRARY_ERROR)
				rendererchanged = false;
		}
#endif
		setrenderneeded = 0;
	}

	VID_ApplyMode();
	VID_SetupBuffer();

	if (rendermode == render_soft)
		SCR_SetDrawFuncs();
#ifdef HWRENDER
	else if (rendermode == render_opengl && rendererchanged)
	{
		HWR_Switch();
		V_SetPalette(0);
	}
#endif

	return rendererchanged;
}

INT32 VID_SetMode(INT32 modeNum)
{
	vid.recalc = 1;
	vid.bpp = 1;

	if (modeNum < 0)
		modeNum = 0;
	if (modeNum >= NUMVITAMODES)
		modeNum = NUMVITAMODES-1;

	vid.width = vitaModes[modeNum][0];
	vid.height = vitaModes[modeNum][1];
	vid.modenum = modeNum;

	VID_CheckRenderer();
	return SDL_TRUE;
}

void I_StartupGraphics(void)
{
	if (dedicated)
	{
		rendermode = render_none;
		return;
	}
	if (graphics_started)
		return;

	COM_AddCommand("vid_nummodes", VID_Command_NumModes_f, COM_LUA);
	COM_AddCommand("vid_info", VID_Command_Info_f, COM_LUA);
	COM_AddCommand("vid_modelist", VID_Command_ModeList_f, COM_LUA);
	COM_AddCommand("vid_mode", VID_Command_Mode_f, 0);
	CV_RegisterVar(&cv_vidwait);
	VITA_DebugInit();

	keyboard_started = true;

	if (M_CheckParm("-renderer"))
	{
		INT32 i = 0;
		CV_PossibleValue_t *renderer_list = cv_renderer_t;
		const char *modeparm = M_GetNextParm();
		while (renderer_list[i].strvalue)
		{
			if (!stricmp(modeparm, renderer_list[i].strvalue))
			{
				chosenrendermode = renderer_list[i].value;
				break;
			}
			i++;
		}
	}
	else if (M_CheckParm("-software"))
		chosenrendermode = render_soft;
#ifdef HWRENDER
	else if (M_CheckParm("-opengl"))
		chosenrendermode = render_opengl;

	if (M_CheckParm("-nogl"))
	{
		vid.glstate = VID_GL_LIBRARY_ERROR;
		if (chosenrendermode == render_opengl)
			chosenrendermode = render_none;
		rendermode = render_soft;
	}
#else
	rendermode = render_soft;
#endif

	if (chosenrendermode != render_none)
		rendermode = chosenrendermode;

	VID_StartupVitaGL();

#ifdef HWRENDER
	if (rendermode == render_opengl)
		VID_StartupOpenGL();
#endif

	VID_SetMode(0);

	VID_UpdateFlipInterval();

	graphics_started = true;
}

void VID_StartupOpenGL(void)
{
#ifdef HWRENDER
	static boolean glstartup = false;
	if (!glstartup)
	{
		CONS_Printf("VID_StartupOpenGL()...\n");
		HWD.pfnInit             = hwSym("Init",NULL);
		HWD.pfnFinishUpdate     = NULL;
		HWD.pfnDraw2DLine       = hwSym("Draw2DLine",NULL);
		HWD.pfnDrawPolygon      = hwSym("DrawPolygon",NULL);
		HWD.pfnDrawIndexedTriangles = hwSym("DrawIndexedTriangles",NULL);
		HWD.pfnRenderSkyDome    = hwSym("RenderSkyDome",NULL);
		HWD.pfnSetBlend         = hwSym("SetBlend",NULL);
		HWD.pfnClearBuffer      = hwSym("ClearBuffer",NULL);
		HWD.pfnSetTexture       = hwSym("SetTexture",NULL);
		HWD.pfnUpdateTexture    = hwSym("UpdateTexture",NULL);
		HWD.pfnDeleteTexture    = hwSym("DeleteTexture",NULL);
		HWD.pfnReadScreenTexture= hwSym("ReadScreenTexture",NULL);
		HWD.pfnGClipRect        = hwSym("GClipRect",NULL);
		HWD.pfnClearMipMapCache = hwSym("ClearMipMapCache",NULL);
		HWD.pfnSetSpecialState  = hwSym("SetSpecialState",NULL);
		HWD.pfnSetTexturePalette= hwSym("SetTexturePalette",NULL);
		HWD.pfnGetTextureUsed   = hwSym("GetTextureUsed",NULL);
		HWD.pfnDrawModel        = hwSym("DrawModel",NULL);
		HWD.pfnCreateModelVBOs  = hwSym("CreateModelVBOs",NULL);
		HWD.pfnSetTransform     = hwSym("SetTransform",NULL);
		HWD.pfnPostImgRedraw    = hwSym("PostImgRedraw",NULL);
		HWD.pfnFlushScreenTextures=hwSym("FlushScreenTextures",NULL);
		HWD.pfnDoScreenWipe     = hwSym("DoScreenWipe",NULL);
		HWD.pfnDrawScreenTexture= hwSym("DrawScreenTexture",NULL);
		HWD.pfnMakeScreenTexture= hwSym("MakeScreenTexture",NULL);
		HWD.pfnDrawScreenFinalTexture=hwSym("DrawScreenFinalTexture",NULL);

		HWD.pfnInitShaders      = hwSym("InitShaders",NULL);
		HWD.pfnLoadShader       = hwSym("LoadShader",NULL);
		HWD.pfnCompileShader    = hwSym("CompileShader",NULL);
		HWD.pfnSetShader        = hwSym("SetShader",NULL);
		HWD.pfnUnSetShader      = hwSym("UnSetShader",NULL);

		HWD.pfnSetShaderInfo    = hwSym("SetShaderInfo",NULL);

		HWD.pfnSetPaletteLookup = hwSym("SetPaletteLookup",NULL);
		HWD.pfnCreateLightTable = hwSym("CreateLightTable",NULL);
		HWD.pfnUpdateLightTable = hwSym("UpdateLightTable",NULL);
		HWD.pfnClearLightTables = hwSym("ClearLightTables",NULL);
		HWD.pfnSetScreenPalette = hwSym("SetScreenPalette",NULL);

		vid.glstate = HWD.pfnInit() ? VID_GL_LIBRARY_LOADED : VID_GL_LIBRARY_ERROR;
		if (vid.glstate == VID_GL_LIBRARY_LOADED)
			VITA_RT_Init();

		if (vid.glstate == VID_GL_LIBRARY_ERROR)
		{
			rendermode = render_soft;
			setrenderneeded = 0;
		}
		glstartup = true;
	}
#endif
}

void I_ShutdownGraphics(void)
{
	const rendermode_t oldrendermode = rendermode;

#ifdef HWRENDER
	VITA_RT_Stop();
#endif

	rendermode = render_none;

	if (oldrendermode == render_soft)
	{
		free(vid.buffer);
		vid.buffer = NULL;
	}

	I_OutputMsg("I_ShutdownGraphics(): ");

	if (!graphics_started)
	{
		I_OutputMsg("graphics never started\n");
		return;
	}
	graphics_started = false;

	VID_ReleaseSoftwareBuffers();
#ifdef HWRENDER
	if (oldrendermode == render_opengl)
		VGL_ReleaseSceneTarget();
#endif
	I_OutputMsg("shut down\n");
}

//
// Frame output
//

void I_UpdateNoBlit(void)
{
}

void I_FinishUpdate(void)
{
	if (rendermode == render_none)
		return;

	SCR_CalculateFPS();

	if (marathonmode)
		SCR_DisplayMarathonInfo();

	if (cv_closedcaptioning.value)
		SCR_ClosedCaptions();

	if (cv_ticrate.value)
		SCR_DisplayTicRate();

	if (cv_showping.value && netgame && consoleplayer != serverplayer)
		SCR_DisplayLocalPing();

	VID_CheckMenuPacing();
	VID_UpdateFlipInterval();

	if (++clockCheckFrames >= 60)
	{
		clockCheckFrames = 0;
		VID_ApplyClocks();
	}

	if (rendermode == render_soft)
		VID_PresentSoftware();
#ifdef HWRENDER
	else if (rendermode == render_opengl)
		VITA_RT_FinishUpdate(cv_vidwait.value);
#endif
}

void I_UpdateNoVsync(void)
{
	INT32 real_vidwait = cv_vidwait.value;
	cv_vidwait.value = 0;
	I_FinishUpdate();
	cv_vidwait.value = real_vidwait;
}

void I_ReadScreen(UINT8 *scr)
{
	if (rendermode != render_soft)
		I_Error("I_ReadScreen: called while in non-software mode");
	else
		VID_BlitLinearScreen(screens[0], scr,
			vid.width*vid.bpp, vid.height,
			vid.rowbytes, vid.rowbytes);
}

void I_SetPalette(RGBA_t *palette)
{
	size_t i;
	// GL_RGBA bytes in memory order: R, G, B, A
	for (i = 0; i < 256; i++)
		swPalette[i] = palette[i].s.red | (palette[i].s.green << 8) | (palette[i].s.blue << 16) | 0xFF000000u;
}

void VID_ShowErrorDialog(const char *title, const char *text)
{
	SceMsgDialogParam param;
	SceMsgDialogUserMessageParam msg;
	static char buffer[SCE_MSG_DIALOG_USER_MSG_SIZE];

#ifdef HWRENDER
	VITA_RT_Stop();
#endif
	// Errors can fire before I_StartupGraphics, e.g. when srb2.pk3 is missing
	VID_StartupVitaGL();

	snprintf(buffer, sizeof buffer, "%s\n\n%s", title, text);

	sceMsgDialogParamInit(&param);
	memset(&msg, 0, sizeof msg);
	msg.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
	msg.msg = (const SceChar8 *)buffer;
	param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
	param.userMsgParam = &msg;

	if (sceMsgDialogInit(&param) < 0)
		return;

	flipInterval = 1;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, DISPLAY_W, DISPLAY_H);
	while (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING)
	{
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		vglSwapBuffers(GL_TRUE);
	}
	sceMsgDialogTerm();
}

UINT32 I_GetRefreshRate(void)
{
	// The display is paced to every other vblank while vsync is on
	return (cv_vidwait.value && !VID_MenuPacing()) ? 30 : 60;
}
