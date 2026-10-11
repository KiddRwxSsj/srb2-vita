// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vita_debug.c
/// \brief Touch debug panel for PS Vita

#include <stdio.h>
#include <psp2/touch.h>
#include <psp2/power.h>
#include <vitaGL.h>

#include "../doomdef.h"
#include "../command.h"
#include "../screen.h"
#include "../v_video.h"
#include "../r_main.h"
#include "../r_fps.h"
#include "../netcode/d_netcmd.h"
#include "../hardware/hw_main.h"
#include "vita_debug.h"
#include "vita_rthread.h"

UINT32 vita_dbgflags = 0;
boolean vita_dbg_in3d = false;

static boolean dbgopen = false;
static boolean dbgtouching = false;
static SceTouchPanelInfo dbgpanel;

enum
{
	DBG_PERFSTATS, DBG_SAMPLES, DBG_SHOWFPS, DBG_FRAMECAP,
	DBG_WALLS, DBG_PLANES, DBG_SPRITES, DBG_SKYBOX,
	DBG_GLDRAWS, DBG_BATCHING, DBG_CONTRAST, DBG_RTHREAD,
	DBG_FARCLIP, DBG_PGEN, DBG_CULLING, DBG_CLOSE,
	DBG_FOG, DBG_BLANK1, DBG_BLANK2, DBG_BLANK3,
	NUMDBGITEMS
};

#define DBG_COLS 4
#define DBG_CORNER 32
#define DBG_ROWS 5

static const char *dbgLabels[NUMDBGITEMS] =
{
	"Perfstats", "Samples", "Show FPS", "Frame cap",
	"Walls", "Floors", "Sprites", "Skybox",
	"GL draws", "Batching", "Contrast", "Render thread",
	"Far clip", "Geometry thread", "Culling", "Close",
	"Fog", NULL, NULL, NULL
};

void VITA_DebugInit(void)
{
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &dbgpanel) < 0 || dbgpanel.maxDispX <= dbgpanel.minDispX)
	{
		dbgpanel.minDispX = dbgpanel.minDispY = 0;
		dbgpanel.maxDispX = 1919;
		dbgpanel.maxDispY = 1087;
	}
}

static void VITA_DebugLayout(INT32 *top, INT32 *header, INT32 *cellw, INT32 *cellh)
{
	*top = vid.height / 2;
	*header = 12 * vid.dup;
	*cellw = vid.width / DBG_COLS;
	*cellh = (vid.height - *top - *header) / DBG_ROWS;
}

static const char *VITA_DebugFlagValue(UINT32 flag, INT32 *color)
{
	*color = (vita_dbgflags & flag) ? V_REDMAP : V_GREENMAP;
	return (vita_dbgflags & flag) ? "Off" : "On";
}

static const char *VITA_DebugValue(INT32 item, INT32 *color)
{
	static char num[16];

	*color = V_GREENMAP;
	switch (item)
	{
		case DBG_PERFSTATS:
			if (cv_perfstats.value == 1)
				return "Render";
			if (cv_perfstats.value == 2)
				return "Logic";
			*color = V_GRAYMAP;
			return "Off";
		case DBG_SAMPLES:
			snprintf(num, sizeof num, "%d", cv_ps_samplesize.value);
			return num;
		case DBG_SHOWFPS:
			if (!cv_ticrate.value)
				*color = V_GRAYMAP;
			return cv_ticrate.string;
		case DBG_FRAMECAP:
			if (cv_vidwait.value && cv_fpscap.value == 0)
				return "30";
			if (cv_vidwait.value && cv_fpscap.value == 60)
				return "60";
			*color = V_YELLOWMAP;
			return cv_fpscap.string;
		case DBG_WALLS:
			return VITA_DebugFlagValue(VDBG_NOWALLS, color);
		case DBG_PLANES:
			return VITA_DebugFlagValue(VDBG_NOPLANES, color);
		case DBG_SPRITES:
			return VITA_DebugFlagValue(VDBG_NOSPRITES, color);
		case DBG_GLDRAWS:
			return VITA_DebugFlagValue(VDBG_NODRAW, color);
		case DBG_SKYBOX:
			if (!cv_skybox.value)
				*color = V_REDMAP;
			return cv_skybox.string;
		case DBG_BATCHING:
			if (!cv_glbatching.value)
				*color = V_REDMAP;
			return cv_glbatching.string;
		case DBG_CONTRAST:
			return cv_glfakecontrast.string;
		case DBG_RTHREAD:
			if (!vita_rt_enabled)
				*color = V_REDMAP;
			return vita_rt_enabled ? "On" : "Off";
		case DBG_PGEN:
			if (!vita_pgen_enabled)
				*color = V_REDMAP;
			return vita_pgen_enabled ? "On" : "Off";
		case DBG_FOG:
			if (!cv_glfarclip.value)
				*color = V_GRAYMAP;
			else if (!cv_glfarfog.value)
				*color = V_REDMAP;
			return cv_glfarfog.string;
		case DBG_CULLING:
			if (!cv_glculling.value)
				*color = V_REDMAP;
			return cv_glculling.string;
		case DBG_FARCLIP:
			if (!cv_glfarclip.value)
				*color = V_GRAYMAP;
			else
				*color = V_YELLOWMAP;
			return cv_glfarclip.string;
		default:
			return "";
	}
}

static void VITA_DebugActivate(INT32 item)
{
	switch (item)
	{
		case DBG_PERFSTATS:
			CV_SetValue(&cv_perfstats, (cv_perfstats.value + 1) % 3);
			break;
		case DBG_SAMPLES:
			CV_SetValue(&cv_ps_samplesize, cv_ps_samplesize.value == 35 ? 175 : 35);
			break;
		case DBG_SHOWFPS:
			CV_SetValue(&cv_ticrate, (cv_ticrate.value + 1) % 3);
			break;
		case DBG_FRAMECAP:
			CV_SetValue(&cv_fpscap, (cv_vidwait.value && cv_fpscap.value == 0) ? 60 : 0);
			CV_SetValue(&cv_vidwait, 1);
			break;
		case DBG_WALLS:
			vita_dbgflags ^= VDBG_NOWALLS;
			break;
		case DBG_PLANES:
			vita_dbgflags ^= VDBG_NOPLANES;
			break;
		case DBG_SPRITES:
			vita_dbgflags ^= VDBG_NOSPRITES;
			break;
		case DBG_GLDRAWS:
			vita_dbgflags ^= VDBG_NODRAW;
			break;
		case DBG_SKYBOX:
			CV_SetValue(&cv_skybox, !cv_skybox.value);
			break;
		case DBG_BATCHING:
			CV_SetValue(&cv_glbatching, !cv_glbatching.value);
			break;
		case DBG_CONTRAST:
			CV_SetValue(&cv_glfakecontrast, (cv_glfakecontrast.value + 1) % 3);
			break;
		case DBG_RTHREAD:
			vita_rt_enabled = !vita_rt_enabled;
			break;
		case DBG_FARCLIP:
			CV_AddValue(&cv_glfarclip, 1);
			break;
		case DBG_PGEN:
			vita_pgen_enabled = !vita_pgen_enabled;
			break;
		case DBG_CULLING:
			CV_SetValue(&cv_glculling, !cv_glculling.value);
			break;
		case DBG_FOG:
			CV_SetValue(&cv_glfarfog, !cv_glfarfog.value);
			break;
		case DBG_CLOSE:
			dbgopen = false;
			break;
	}
}

void VITA_DebugPollTouch(void)
{
	SceTouchData touch;
	INT32 x, y, top, header, cellw, cellh, col, row;
	boolean down;

	if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) < 1)
		return;

	down = (touch.reportNum > 0);
	if (!down || dbgtouching)
	{
		dbgtouching = down;
		return;
	}
	dbgtouching = true;

	x = (touch.report[0].x - dbgpanel.minDispX) * vid.width / (dbgpanel.maxDispX - dbgpanel.minDispX + 1);
	y = (touch.report[0].y - dbgpanel.minDispY) * vid.height / (dbgpanel.maxDispY - dbgpanel.minDispY + 1);

	// only the top right corner opens it, so stray taps do nothing
	if (!dbgopen)
	{
		if (x >= vid.width - DBG_CORNER * vid.dup && y < DBG_CORNER * vid.dup)
			dbgopen = true;
		return;
	}

	VITA_DebugLayout(&top, &header, &cellw, &cellh);
	if (y < top)
	{
		dbgopen = false;
		return;
	}

	y -= top + header;
	if (x < 0 || y < 0)
		return;
	col = x / cellw;
	row = y / cellh;
	if (col < DBG_COLS && row < DBG_ROWS)
		VITA_DebugActivate(row * DBG_COLS + col);
}

void VITA_DebugDraw(void)
{
	char buf[128];
	INT32 top, header, cellw, cellh, i;
	const INT32 flags = V_NOSCALESTART|V_ALLOWLOWERCASE;

	if (!dbgopen)
		return;

	VITA_DebugLayout(&top, &header, &cellw, &cellh);
	V_DrawFill(0, top, vid.width, vid.height - top, 31|V_NOSCALESTART|V_30TRANS);

	snprintf(buf, sizeof buf, "CPU %d  BUS %d  GPU %d  XBAR %d MHz    VRAM free %u MB    RAM free %u MB",
		scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
		scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency(),
		(unsigned)(vglMemFree(VGL_MEM_VRAM) >> 20), (unsigned)(vglMemFree(VGL_MEM_RAM) >> 20));
	V_DrawThinString(4 * vid.dup, top + 2 * vid.dup, flags|V_YELLOWMAP, buf);

	for (i = 0; i < NUMDBGITEMS; i++)
	{
		INT32 x = (i % DBG_COLS) * cellw;
		INT32 y = top + header + (i / DBG_COLS) * cellh;
		INT32 cx = x + cellw / 2;
		INT32 cy = y + cellh / 2;
		INT32 color;
		const char *value;

		if (!dbgLabels[i])
			continue;
		value = VITA_DebugValue(i, &color);

		V_DrawFill(x + vid.dup, y + vid.dup, cellw - 2 * vid.dup, cellh - 2 * vid.dup, 27|V_NOSCALESTART);
		V_DrawThinString(cx - V_ThinStringWidth(dbgLabels[i], flags) * vid.dup / 2, cy - 9 * vid.dup, flags, dbgLabels[i]);
		V_DrawThinString(cx - V_ThinStringWidth(value, flags) * vid.dup / 2, cy + vid.dup, flags|color, value);
	}
}
