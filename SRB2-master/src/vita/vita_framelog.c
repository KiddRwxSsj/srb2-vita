// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vita_framelog.c
/// \brief Frame time and hitch log for PS Vita

#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <psp2/kernel/processmgr.h>

#include "../doomdef.h"
#include "../doomstat.h"
#include "../g_state.h"
#include "../i_system.h"
#include "../i_video.h"
#include "../m_perfstats.h"
#include "../r_main.h"
#include "../hardware/hw_main.h"
#include "vita_framelog.h"
#include "vita_prof.h"

#define VFL_WINDOW_US  2000000
#define VFL_HITCH_US   50000
#define VFL_MAXHITCHES 8
#define VFL_MAXSAMPLES 4096

typedef struct
{
	UINT32 reads, readbytes, readus;
	UINT32 uploads, uploadbytes;
	UINT32 presentus, renderus, zoneus;
} vflcount_t;

typedef struct
{
	UINT32 frames, tics, gametics, hitches, maxus;
	UINT64 totalus, phaseus[VFL_NUMPHASES];
	vflcount_t counts;

	UINT64 logicus, playerus, thinkus, luaus, listus[NUM_THINKERLISTS];
	UINT32 mobjhooks, checkpos;

	UINT32 rframes;
	UINT64 rcallus, bspus, nodesortus, nodedrawus, spritesortus, spritedrawus, skyboxus, batchsortus, batchdrawus;
	UINT64 polys, calls;
} vflstats_t;

// Bumped from the game, render and geometry threads
static vflcount_t live;

static UINT64 framestart, phasestart, windowstart, segmentstart;
static vflcount_t framebase;
static vflstats_t cur, window, segment;
static boolean segmenton;
static UINT16 *samples;
static UINT32 numsamples;
static FILE *stabilityout;
static UINT64 stabilitystart;
static UINT32 stabilitymax, stabilityframes, stabilityexcluded;
static tic_t stabilitylasttic;
static INT16 stabilitylastmap;
static boolean stabilityinlevel;

void VITA_FrameStability(FILE *out)
{
	if (out && !stabilityout)
	{
		stabilitystart = VITA_FrameClock();
		stabilitymax = stabilityframes = stabilityexcluded = 0;
		stabilityinlevel = false;
	}
	stabilityout = out;
}

#define VFL_ADD(field, v) __atomic_fetch_add(&live.field, (UINT32)(v), __ATOMIC_RELAXED)
#define VFL_LOAD(field) __atomic_load_n(&live.field, __ATOMIC_RELAXED)

UINT64 VITA_FrameClock(void)
{
	return sceKernelGetProcessTimeWide();
}

void VITA_FrameCountRead(size_t bytes, UINT32 us)
{
	VFL_ADD(reads, 1);
	VFL_ADD(readbytes, bytes);
	VFL_ADD(readus, us);
}

void VITA_FrameCountUpload(size_t bytes)
{
	VFL_ADD(uploads, 1);
	VFL_ADD(uploadbytes, bytes);
}

void VITA_FrameCountPresentWait(UINT32 us)
{
	VFL_ADD(presentus, us);
}

void VITA_FrameCountRender(UINT32 us)
{
	VFL_ADD(renderus, us);
}

void VITA_FrameCountZoneWait(UINT32 us)
{
	VFL_ADD(zoneus, us);
}

static void VFL_Snapshot(vflcount_t *out)
{
	out->reads = VFL_LOAD(reads);
	out->readbytes = VFL_LOAD(readbytes);
	out->readus = VFL_LOAD(readus);
	out->uploads = VFL_LOAD(uploads);
	out->uploadbytes = VFL_LOAD(uploadbytes);
	out->presentus = VFL_LOAD(presentus);
	out->renderus = VFL_LOAD(renderus);
	out->zoneus = VFL_LOAD(zoneus);
}

static void VFL_CountDelta(vflcount_t *d, const vflcount_t *now, const vflcount_t *base)
{
	d->reads = now->reads - base->reads;
	d->readbytes = now->readbytes - base->readbytes;
	d->readus = now->readus - base->readus;
	d->uploads = now->uploads - base->uploads;
	d->uploadbytes = now->uploadbytes - base->uploadbytes;
	d->presentus = now->presentus - base->presentus;
	d->renderus = now->renderus - base->renderus;
	d->zoneus = now->zoneus - base->zoneus;
}

static void VFL_CountAdd(vflcount_t *a, const vflcount_t *b)
{
	a->reads += b->reads;
	a->readbytes += b->readbytes;
	a->readus += b->readus;
	a->uploads += b->uploads;
	a->uploadbytes += b->uploadbytes;
	a->presentus += b->presentus;
	a->renderus += b->renderus;
	a->zoneus += b->zoneus;
}

static void VFL_Merge(vflstats_t *a, const vflstats_t *b)
{
	INT32 i;

	a->frames += b->frames;
	a->tics += b->tics;
	a->gametics += b->gametics;
	a->hitches += b->hitches;
	if (b->maxus > a->maxus)
		a->maxus = b->maxus;
	a->totalus += b->totalus;
	for (i = 0; i < VFL_NUMPHASES; i++)
		a->phaseus[i] += b->phaseus[i];
	VFL_CountAdd(&a->counts, &b->counts);

	a->logicus += b->logicus;
	a->playerus += b->playerus;
	a->thinkus += b->thinkus;
	a->luaus += b->luaus;
	for (i = 0; i < NUM_THINKERLISTS; i++)
		a->listus[i] += b->listus[i];
	a->mobjhooks += b->mobjhooks;
	a->checkpos += b->checkpos;

	a->rframes += b->rframes;
	a->rcallus += b->rcallus;
	a->bspus += b->bspus;
	a->nodesortus += b->nodesortus;
	a->nodedrawus += b->nodedrawus;
	a->spritesortus += b->spritesortus;
	a->spritedrawus += b->spritedrawus;
	a->skyboxus += b->skyboxus;
	a->batchsortus += b->batchsortus;
	a->batchdrawus += b->batchdrawus;
	a->polys += b->polys;
	a->calls += b->calls;
}

static UINT64 VFL_Us(precise_t t)
{
	static UINT64 freq;

	if (!freq)
		freq = I_GetPrecisePrecision();
	return (UINT64)t * 1000000 / freq;
}

void VITA_FrameTicStats(void)
{
	INT32 i;

	cur.tics++;
	cur.logicus += VFL_Us(ps_tictime.value.p);
	if (gamestate != GS_LEVEL)
		return;
	cur.gametics++;
	VITA_ProfTic();
	VITA_SampleTic(VFL_Us(ps_tictime.value.p));
	cur.playerus += VFL_Us(ps_playerthink_time.value.p);
	cur.thinkus += VFL_Us(ps_thinkertime.value.p);
	cur.luaus += VFL_Us(ps_lua_prethinkframe_time.value.p) + VFL_Us(ps_lua_thinkframe_time.value.p)
		+ VFL_Us(ps_lua_postthinkframe_time.value.p);
	for (i = 0; i < NUM_THINKERLISTS; i++)
		cur.listus[i] += VFL_Us(ps_thlist_times[i].value.p);
	cur.mobjhooks += ps_lua_mobjhooks.value.i;
	cur.checkpos += ps_checkposition_calls.value.i;
}

void VITA_FrameRenderStats(void)
{
	if (gamestate != GS_LEVEL || rendermode != render_opengl)
		return;
	cur.rframes++;
	cur.rcallus += VFL_Us(ps_rendercalltime.value.p);
	cur.bspus += VFL_Us(ps_bsptime.value.p);
	cur.nodesortus += VFL_Us(ps_hw_nodesorttime.value.p);
	cur.nodedrawus += VFL_Us(ps_hw_nodedrawtime.value.p);
	cur.spritesortus += VFL_Us(ps_hw_spritesorttime.value.p);
	cur.spritedrawus += VFL_Us(ps_hw_spritedrawtime.value.p);
	cur.skyboxus += VFL_Us(ps_hw_skyboxtime.value.p);
	cur.batchsortus += VFL_Us(ps_hw_batchsorttime.value.p);
	cur.batchdrawus += VFL_Us(ps_hw_batchdrawtime.value.p);
	cur.polys += ps_hw_numpolys.value.i;
	cur.calls += ps_hw_numcalls.value.i;
}

static unsigned VFL_Ms10(UINT64 us)
{
	return (unsigned)((us + 50) / 100);
}

#define MS10(v) VFL_Ms10(v) / 10, VFL_Ms10(v) % 10

static int VFL_Compare(const void *a, const void *b)
{
	return (int)*(const UINT16 *)b - (int)*(const UINT16 *)a;
}

// Writes one summary line to the log and optionally to a file
static void VFL_Print(const char *label, const vflstats_t *s, UINT64 span, UINT32 lowus, FILE *out)
{
	const UINT32 n = s->frames ? s->frames : 1;
	const UINT32 t = s->gametics ? s->gametics : 1;
	const UINT32 r = s->rframes ? s->rframes : 1;
	char line[1024];

	snprintf(line, sizeof line,
		"%s: %u frames in %u.%u s, %u.%u fps, avg %u.%u ms, 1%% low %u.%u ms, max %u.%u ms, hitches %u, game speed %u%%"
		" | frame: tics %u.%u display %u.%u present-wait %u.%u render-thread %u.%u other %u.%u ms"
		" | tic: logic %u.%u player %u.%u thinkers %u.%u (mobj %u.%u main %u.%u precip %u.%u) lua %u.%u ms, mobjhooks %u checkpos %u"
		" | view: %u.%u ms (bsp %u.%u nodesort %u.%u nodedraw %u.%u spritesort %u.%u spritedraw %u.%u skybox %u.%u batchsort %u.%u batchdraw %u.%u) polys %u calls %u"
		" | io: reads %u (%u KB, %u.%u ms) uploads %u (%u KB) zonewait %u.%u ms",
		label, (unsigned)s->frames, MS10(span / 1000), MS10((UINT64)s->frames * 1000000000ULL / (span ? span : 1)),
		MS10(s->totalus / n), MS10(lowus), MS10(s->maxus), (unsigned)s->hitches,
		(unsigned)(span ? (UINT64)s->tics * 100 * 1000000 / ((UINT64)TICRATE * span) : 0),
		MS10(s->phaseus[VFL_TICS] / n), MS10(s->phaseus[VFL_DISPLAY] / n), MS10(s->counts.presentus / n),
		MS10(s->counts.renderus / n), MS10(s->phaseus[VFL_OTHER] / n),
		MS10(s->logicus / (s->tics ? s->tics : 1)), MS10(s->playerus / t), MS10(s->thinkus / t),
		MS10(s->listus[THINK_MOBJ] / t), MS10(s->listus[THINK_MAIN] / t), MS10(s->listus[THINK_PRECIP] / t),
		MS10(s->luaus / t), (unsigned)(s->mobjhooks / t), (unsigned)(s->checkpos / t),
		MS10(s->rcallus / r), MS10(s->bspus / r), MS10(s->nodesortus / r), MS10(s->nodedrawus / r),
		MS10(s->spritesortus / r), MS10(s->spritedrawus / r), MS10(s->skyboxus / r),
		MS10(s->batchsortus / r), MS10(s->batchdrawus / r), (unsigned)(s->polys / r), (unsigned)(s->calls / r),
		(unsigned)s->counts.reads, (unsigned)(s->counts.readbytes / 1024), MS10(s->counts.readus),
		(unsigned)s->counts.uploads, (unsigned)(s->counts.uploadbytes / 1024), MS10(s->counts.zoneus));
	I_OutputMsg("%s\n", line);
	if (out)
	{
		fprintf(out, "%s\n", line);
		fflush(out);
	}
}

void VITA_FrameBegin(void)
{
	framestart = phasestart = VITA_FrameClock();
	if (!windowstart)
		windowstart = framestart;
	memset(&cur, 0, sizeof cur);
	VFL_Snapshot(&framebase);
}

void VITA_FramePhase(vflphase_t phase)
{
	UINT64 now = VITA_FrameClock();

	cur.phaseus[phase] += now - phasestart;
	phasestart = now;
}

void VITA_FrameEnd(UINT32 tics)
{
	UINT64 now = VITA_FrameClock();
	UINT32 total = (UINT32)(now - framestart);
	vflcount_t snap;

	(void)tics;
	if (stabilityout)
	{
		boolean inlevel = gamestate == GS_LEVEL && leveltime > 1;
		if (inlevel && stabilityinlevel && gamemap == stabilitylastmap && leveltime >= stabilitylasttic)
		{
			stabilitymax = max(stabilitymax, total);
			stabilityframes++;
		}
		else
			stabilityexcluded++;
		stabilityinlevel = inlevel;
		stabilitylastmap = gamemap;
		stabilitylasttic = leveltime;
	}
	cur.phaseus[VFL_OTHER] += now - phasestart;
	cur.frames = 1;
	cur.totalus = cur.maxus = total;
	VFL_Snapshot(&snap);
	VFL_CountDelta(&cur.counts, &snap, &framebase);

	if (total >= VFL_HITCH_US)
	{
		cur.hitches = 1;
		if (window.hitches < VFL_MAXHITCHES)
			I_OutputMsg("Hitch: %u.%u ms | tics %u (%u.%u ms) display %u.%u ms present-wait %u.%u ms other %u.%u ms"
				" | view %u.%u ms | reads %u (%u KB, %u.%u ms) uploads %u (%u KB) zonewait %u.%u ms | state %d map %d\n",
				MS10(total), (unsigned)cur.tics, MS10(cur.phaseus[VFL_TICS]), MS10(cur.phaseus[VFL_DISPLAY]),
				MS10(cur.counts.presentus), MS10(cur.phaseus[VFL_OTHER]), MS10(cur.rcallus),
				(unsigned)cur.counts.reads, (unsigned)(cur.counts.readbytes / 1024), MS10(cur.counts.readus),
				(unsigned)cur.counts.uploads, (unsigned)(cur.counts.uploadbytes / 1024), MS10(cur.counts.zoneus),
				(int)gamestate, (int)gamemap);
	}

	VFL_Merge(&window, &cur);
	if (segmenton)
	{
		VFL_Merge(&segment, &cur);
		if (samples && numsamples < VFL_MAXSAMPLES)
			samples[numsamples++] = (UINT16)min(VFL_Ms10(total), 65535);
	}

	if (now - windowstart >= VFL_WINDOW_US)
	{
		if (stabilityout)
		{
			struct mallinfo heap = mallinfo();
			fprintf(stabilityout, "Stability|elapsed_us=%llu|map=%d|state=%d|tic=%u|heap_used=%lu|heap_arena=%lu|postload_max_us=%u|frames=%u|excluded=%u\n",
				(unsigned long long)(now - stabilitystart), (int)gamemap, (int)gamestate, (unsigned)leveltime,
				(unsigned long)heap.uordblks, (unsigned long)heap.arena, (unsigned)stabilitymax,
				(unsigned)stabilityframes, (unsigned)stabilityexcluded);
			fflush(stabilityout);
			stabilitymax = stabilityframes = stabilityexcluded = 0;
		}
		VFL_Print("Frames", &window, now - windowstart, 0, NULL);
		windowstart = now;
		memset(&window, 0, sizeof window);
	}
}

void VITA_FrameSegmentBegin(void)
{
	if (!samples)
		samples = malloc(VFL_MAXSAMPLES * sizeof *samples);
	memset(&segment, 0, sizeof segment);
	numsamples = 0;
	segmentstart = VITA_FrameClock();
	segmenton = true;
}

void VITA_FrameSegmentEnd(const char *label, FILE *out)
{
	UINT32 lowus = 0;

	if (!segmenton)
		return;
	segmenton = false;
	if (numsamples)
	{
		// 1% low is the average of the slowest 1% of frames
		UINT32 i, count = max(numsamples / 100, 1);
		UINT64 sum = 0;

		qsort(samples, numsamples, sizeof *samples, VFL_Compare);
		for (i = 0; i < count; i++)
			sum += samples[i];
		lowus = (UINT32)(sum * 100 / count);
	}
	VFL_Print(label, &segment, VITA_FrameClock() - segmentstart, lowus, out);
}
