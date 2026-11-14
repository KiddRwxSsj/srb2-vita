// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vita_bench.c
/// \brief Automatic map tour benchmark for PS Vita, armed by <srb2home>/benchmark.txt

#include "../p_dormant.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../doomdef.h"
#include "../doomstat.h"
#include "../d_main.h"
#include "../g_game.h"
#include "../g_demo.h"
#include "../g_state.h"
#include "../command.h"
#include "../i_system.h"
#include "../m_misc.h"
#include "../p_local.h"
#include "../tables.h"
#include "vita_framelog.h"
#include "vita_bench.h"
#include "vita_prof.h"
#include "../perf_count.h"

#define BENCH_RUN_US     10000000ULL
#define BENCH_TIMEOUT_US 45000000ULL
#define BENCH_MAXITEMS   64

typedef enum
{
	BS_OFF,
	BS_WAITTITLE,
	BS_LOADING,
	BS_RUNNING,
} benchstate_t;

typedef struct
{
	INT16 map;
	char demo[128];
} benchitem_t;

static const INT16 defaultmaps[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 22, 23, 25, 26, 27};

static benchitem_t items[BENCH_MAXITEMS];
static INT32 numitems, itemidx;
static char addons[4][128];
static INT32 numaddons;
static boolean profile;
static boolean sample;
static boolean counter;
static UINT32 stabilityseconds;
static UINT64 stabilitystart;
static benchstate_t state = BS_OFF;
static UINT64 statestart, benchstart;
static angle_t startangle;
static FILE *out;

static char *Bench_Trim(char *s)
{
	char *e;

	while (*s && isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

static INT16 Bench_ParseMap(const char *s)
{
	if (!strnicmp(s, "MAP", 3) && s[3] && s[4])
		return (INT16)M_MapNumber(toupper((unsigned char)s[3]), toupper((unsigned char)s[4]));
	if (isdigit((unsigned char)*s))
		return (INT16)atoi(s);
	return 0;
}

static void Bench_ParseLine(char *line)
{
	char *s = Bench_Trim(line);

	if (!*s || *s == '#')
		return;
	if (!stricmp(s, "profile"))
		profile = true;
	else if (!stricmp(s, "sample"))
		sample = true;
	else if (!stricmp(s, "count"))
		counter = true;
	else if (!strnicmp(s, "stability ", 10))
	{
		char *end;
		unsigned long seconds = strtoul(s + 10, &end, 10);
		stabilityseconds = (!*end && seconds >= 1800 && seconds <= 3600) ? (UINT32)seconds : 1;
	}
	else if (!strnicmp(s, "addon ", 6) && numaddons < 4)
		strlcpy(addons[numaddons++], Bench_Trim(s + 6), sizeof addons[0]);
	else if (!strnicmp(s, "demo ", 5) && numitems < BENCH_MAXITEMS)
	{
		items[numitems].map = 0;
		strlcpy(items[numitems++].demo, Bench_Trim(s + 5), sizeof items[0].demo);
	}
	else if (numitems < BENCH_MAXITEMS)
	{
		INT16 map = Bench_ParseMap(s);

		if (map > 0 && map <= NUMMAPS)
		{
			items[numitems].map = map;
			items[numitems++].demo[0] = '\0';
		}
	}
}

void VITA_BenchInit(void)
{
	char path[512], line[160];
	FILE *f;
	INT32 i;

	snprintf(path, sizeof path, "%s" PATHSEP "benchmark.txt", srb2home);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof line, f))
		Bench_ParseLine(line);
	fclose(f);
	remove(path);
	if (sample && profile)
	{
		I_OutputMsg("Benchmark: sample disables clock-heavy profile mode\n");
		profile = false;
	}
	if (counter && (sample || profile || stabilityseconds))
	{
		I_OutputMsg("Benchmark FAILED: count must run separately from sampling/profile/stability\n");
		COM_BufAddText("quit\n");
		return;
	}
#ifndef SRB2_PERF_COUNT
	if (counter)
	{
		I_OutputMsg("Benchmark FAILED: this build has no count diagnostics\n");
		COM_BufAddText("quit\n");
		return;
	}
#endif

	if (!numitems)
	{
		for (; numitems < (INT32)(sizeof defaultmaps / sizeof *defaultmaps); numitems++)
			items[numitems].map = defaultmaps[numitems];
	}
	if (stabilityseconds && (stabilityseconds < 1800 || numitems != 1 || (items[0].map && items[0].map != 14) || sample || profile))
	{
		I_OutputMsg("Benchmark: stability requires 1800..3600 seconds and exactly one MAP14 or demo, no profiling\n");
		COM_BufAddText("quit\n");
		return;
	}

	snprintf(path, sizeof path, "%s" PATHSEP "benchmark-result.txt", srb2home);
	out = fopen(path, "w");
	if (out)
	{
		fprintf(out, "SRB2 Vita benchmark, %s, %d items, %u s per map, profile %s\n",
			compdate, (int)numitems, (unsigned)(BENCH_RUN_US / 1000000), profile ? "on" : "off");
		fprintf(out, "Sampling %s\n", sample ? "on" : "off");
		if (counter) fprintf(out, "Counting on: 60 seconds per map; every original query executes\n");
		if (sample) fprintf(out, "Sampling minimum: 20000 logic samples per map, extends beyond 10 seconds, 900-second limit\n");
		for (i = 0; i < numaddons; i++)
			fprintf(out, "Addon %s\n", addons[i]);
		fflush(out);
	}
	I_OutputMsg("Benchmark: %d items queued\n", (int)numitems);
	itemidx = -1;
	state = BS_WAITTITLE;
	benchstart = statestart = VITA_FrameClock();
}

boolean VITA_BenchActive(void)
{
	return state != BS_OFF;
}

static void Bench_Finish(void)
{
	UINT64 total = VITA_FrameClock() - benchstart;
	VITA_FrameStability(NULL);
	PC_SetOutput(NULL);

	I_OutputMsg("Benchmark: finished in %u s\n", (unsigned)(total / 1000000));
	if (out)
	{
		fprintf(out, "Finished in %u s\n", (unsigned)(total / 1000000));
		fclose(out);
		out = NULL;
	}
	state = BS_OFF;
	COM_BufAddText("quit\n");
}

static const char *Bench_Label(void)
{
	const benchitem_t *it = &items[itemidx];

	return it->map ? va("Bench %s", G_BuildMapName(it->map)) : va("Bench demo %s", it->demo);
}

static void Bench_NextItem(void)
{
	const benchitem_t *it;

	if (++itemidx >= numitems)
	{
		if (stabilityseconds && VITA_FrameClock() - stabilitystart < (UINT64)stabilityseconds * 1000000)
			itemidx = 0;
		else
		{
			Bench_Finish();
			return;
		}
	}
	it = &items[itemidx];
	if (it->map)
		COM_BufAddText(va("map %s -f\n", G_BuildMapName(it->map)));
	else if (stabilityseconds)
	{
		char path[512];
		// Start before the next title ticker can launch an attract demo.
		if (demoplayback)
			G_StopDemo();
		advancedemo = false;
		titledemo = singledemo = false;
		demofileoverride = DFILE_OVERRIDE_SKIP;
		snprintf(path, sizeof path, "%s" PATHSEP "%s", srb2home, it->demo);
		G_DoPlayDemo(path);
	}
	else
		COM_BufAddText(va("playdemo \"%s\"\n", it->demo));
	state = BS_LOADING;
	statestart = VITA_FrameClock();
}

static void Bench_StartSegment(UINT64 now)
{
	if (stabilityseconds && !stabilitystart)
	{
		stabilitystart = now;
		VITA_FrameStability(out);
	}
	state = BS_RUNNING;
	statestart = now;
	VITA_FrameSegmentBegin();
	P_DormantResetStats();
#ifdef SRB2_PERF_COUNT
	if (counter)
	{
		pcount_on = true;
		PC_Reset();
		PC_SetOutput(out);
	}
#endif
	if (profile)
		VITA_ProfBegin();
	if (sample)
		VITA_SampleBegin();
}

static void Bench_EndSegment(void)
{
	const char *label = Bench_Label();
	char copy[160];

	strlcpy(copy, label, sizeof copy);
	VITA_FrameSegmentEnd(copy, out);
	P_DormantReport(out);
#ifdef SRB2_PERF_COUNT
	if (counter)
	{
		PC_Dump(out);
		PC_SetOutput(NULL);
		pcount_on = false;
	}
#endif
	if (profile)
		VITA_ProfEnd(copy, out);
	if (sample)
		VITA_SampleEnd(copy, out);
}

void VITA_BenchFrame(void)
{
	UINT64 now, elapsed;
	player_t *player;
	INT32 i;

	if (state == BS_OFF)
		return;

	now = VITA_FrameClock();
	elapsed = now - statestart;
	player = &players[consoleplayer];

	switch (state)
	{
		case BS_WAITTITLE:
			if (gamestate == GS_TITLESCREEN)
			{
				for (i = 0; i < numaddons; i++)
					COM_BufAddText(va("addfile \"%s\"\n", addons[i]));
				Bench_NextItem();
			}
			break;
		case BS_LOADING:
			if (items[itemidx].map)
			{
				if (gamestate == GS_LEVEL && gamemap == items[itemidx].map && leveltime > 0 && player->mo)
				{
					startangle = player->mo->angle;
					Bench_StartSegment(now);
					break;
				}
			}
			else if (gamestate == GS_LEVEL && demoplayback && leveltime > 0)
			{
				Bench_StartSegment(now);
				break;
			}
			if (elapsed > BENCH_TIMEOUT_US)
			{
				if (stabilityseconds)
				{
					I_OutputMsg("Stability FAILED: demo did not start\n");
					if (out)
						fprintf(out, "Stability FAILED: demo did not start\n");
					Bench_Finish();
					break;
				}
				I_OutputMsg("%s: did not start, skipped\n", Bench_Label());
				if (out)
					fprintf(out, "%s: did not start, skipped\n", Bench_Label());
				Bench_NextItem();
			}
			break;
		case BS_RUNNING:
			if (stabilityseconds && gamestate == GS_LEVEL && gamemap != 14)
			{
				I_OutputMsg("Stability FAILED: expected MAP14, got map %d\n", gamemap);
				if (out)
					fprintf(out, "Stability FAILED: expected MAP14, got map %d\n", gamemap);
				Bench_Finish();
				break;
			}
			if (items[itemidx].demo[0])
			{
				// Demos run to their own end
				if (!demoplayback || gamestate != GS_LEVEL)
				{
					Bench_EndSegment();
					Bench_NextItem();
				}
				break;
			}
			if (sample && elapsed >= 900000000ULL && VITA_SampleLogicSamples() < 20000)
			{
				if (out) fprintf(out, "Sampling FAILED: insufficient logic samples within 900 seconds\n");
				Bench_EndSegment();
				Bench_Finish();
				break;
			}
			if ((elapsed >= (stabilityseconds ? (UINT64)stabilityseconds * 1000000 : (counter ? 60000000ULL : BENCH_RUN_US))
				&& (!sample || VITA_SampleLogicSamples() >= 20000)) || gamestate != GS_LEVEL)
			{
				Bench_EndSegment();
				Bench_NextItem();
				break;
			}
			// One full turn over the run, so every side of the spawn gets drawn
			if (player->mo)
				P_ForceLocalAngle(player, startangle + (angle_t)((elapsed << 32) / BENCH_RUN_US));
			break;
		default:
			break;
	}
}
