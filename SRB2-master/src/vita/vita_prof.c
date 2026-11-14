// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vita_prof.c
/// \brief Per thinker type profiler for PS Vita benchmark runs

#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>

#include "../doomdef.h"
#include "../deh_tables.h"
#include "../i_system.h"
#include "../m_perfstats.h"
#include "../p_local.h"
#include "../p_mobj.h"
#include "vita_framelog.h"
#include "vita_prof.h"

#define VPROF_MAXFUNCS 64
#define VPROF_TOPTYPES 12
#define VPROF_TOPFUNCS 8
#define VPROF_MAXACTIONS 160
#define VPROF_TOPACTIONS 12
#define VPROF_ACTIONDEPTH 16

typedef struct
{
	UINT32 count, checkpos, hooks, sights;
	UINT64 us, phaseus[VPROF_NUMPHASES], cpus, sightus;
} vproftype_t;

typedef struct
{
	actionf_p1 func;
	UINT32 list, count, checkpos;
	UINT64 us, cpus;
} vproffunc_t;

typedef struct
{
	void *func;
	UINT32 count;
	UINT64 us, selfus;
} vprofaction_t;

boolean vprof_on = false;
UINT32 vprof_cpthings, vprof_cplines;

static vproftype_t *types;
static vproffunc_t funcs[VPROF_MAXFUNCS];
static INT32 numfuncs;
static UINT64 listus[NUM_THINKERLISTS];
static UINT32 tics;

static vproftype_t *curtype;
static vproffunc_t *curfunc;
static vprofphase_t curphase;
static UINT64 phasestart;

// P_CheckPosition totals across all callers
static UINT64 cpus[VPROF_CP_NUMSECTIONS], cpstart;
static UINT32 cpcalls, cpdepth;
static UINT64 cpthings, cplines;

static UINT64 sightus;
static UINT32 clockns;
static UINT32 sightcalls;

static vprofaction_t actions[VPROF_MAXACTIONS];
static INT32 numactions;
static struct
{
	vprofaction_t *action;
	UINT64 start, childus;
} actionstack[VPROF_ACTIONDEPTH];
static INT32 actiondepth;

UINT64 VITA_ProfCPBegin(void)
{
	cpcalls++;
	if (cpdepth++)
		return 0;
	return cpstart = VITA_FrameClock();
}

void VITA_ProfCPSection(vprofcpsection_t section, UINT64 *mark)
{
	UINT64 now = VITA_FrameClock();

	cpus[section] += now - *mark;
	*mark = now;
}

void VITA_ProfCPEnd(void)
{
	UINT64 took = VITA_FrameClock() - cpstart;

	if (curtype)
		curtype->cpus += took;
	else if (curfunc)
		curfunc->cpus += took;
	cpdepth = 0;
}

UINT64 VITA_ProfSightBegin(void)
{
	return VITA_FrameClock();
}

void VITA_ProfSightEnd(UINT64 start)
{
	UINT64 took = VITA_FrameClock() - start;

	sightcalls++;
	sightus += took;
	if (curtype)
	{
		curtype->sights++;
		curtype->sightus += took;
	}
}

static vprofaction_t *Prof_Action(void *func)
{
	INT32 i;

	for (i = 0; i < numactions; i++)
		if (actions[i].func == func)
			return &actions[i];
	if (numactions == VPROF_MAXACTIONS)
		return NULL;
	actions[numactions].func = func;
	return &actions[numactions++];
}

void VITA_ProfActionBegin(void *action)
{
	if (actiondepth < VPROF_ACTIONDEPTH)
	{
		actionstack[actiondepth].action = Prof_Action(action);
		actionstack[actiondepth].childus = 0;
		actionstack[actiondepth].start = VITA_FrameClock();
	}
	actiondepth++;
}

void VITA_ProfActionEnd(void)
{
	UINT64 took;
	vprofaction_t *a;

	if (--actiondepth >= VPROF_ACTIONDEPTH)
		return;
	took = VITA_FrameClock() - actionstack[actiondepth].start;
	a = actionstack[actiondepth].action;
	if (a)
	{
		a->count++;
		a->us += took;
		a->selfus += took - actionstack[actiondepth].childus;
	}
	if (actiondepth > 0 && actiondepth <= VPROF_ACTIONDEPTH)
		actionstack[actiondepth - 1].childus += took;
}

void VITA_ProfEnter(vprofphase_t phase)
{
	UINT64 now;

	if (!curtype)
		return;
	now = VITA_FrameClock();
	curtype->phaseus[curphase] += now - phasestart;
	curphase = phase;
	phasestart = now;
}

static vproffunc_t *Prof_Func(size_t list, actionf_p1 func)
{
	INT32 i;

	for (i = 0; i < numfuncs; i++)
		if (funcs[i].func == func && funcs[i].list == list)
			return &funcs[i];
	if (numfuncs == VPROF_MAXFUNCS)
		return NULL;
	funcs[numfuncs].func = func;
	funcs[numfuncs].list = (UINT32)list;
	return &funcs[numfuncs++];
}

void VITA_ProfThinker(size_t list, thinker_t *th)
{
	const think_t func = th->function;
	const boolean ismobj = (list == THINK_MOBJ && func == (think_t)P_MobjThinker);
	vproftype_t *t = NULL;
	INT32 checkpos = 0, hooks = 0;
	UINT64 start, took;

	if (ismobj)
	{
		mobjtype_t type = ((mobj_t *)th)->type;

		if ((unsigned)type < NUMMOBJTYPES)
		{
			t = &types[type];
			checkpos = ps_checkposition_calls.value.i;
			hooks = ps_lua_mobjhooks.value.i;
		}
	}

	if (!t)
	{
		curfunc = Prof_Func(list, ismobj ? NULL : func);
		checkpos = ps_checkposition_calls.value.i;
	}

	start = phasestart = VITA_FrameClock();
	curtype = t;
	curphase = VPROF_HEAD;
	th->function(th);
	took = VITA_FrameClock() - start;
	listus[list] += took;
	cpthings += vprof_cpthings;
	cplines += vprof_cplines;
	vprof_cpthings = vprof_cplines = 0;

	if (t)
	{
		t->phaseus[curphase] += start + took - phasestart;
		t->count++;
		t->us += took;
		t->checkpos += ps_checkposition_calls.value.i - checkpos;
		t->hooks += ps_lua_mobjhooks.value.i - hooks;
		curtype = NULL;
	}
	else if (curfunc)
	{
		curfunc->count++;
		curfunc->us += took;
		curfunc->checkpos += ps_checkposition_calls.value.i - checkpos;
		curfunc = NULL;
	}
}

void VITA_ProfTic(void)
{
	if (vprof_on)
		tics++;
}

void VITA_ProfBegin(void)
{
	if (!types)
		types = malloc(NUMMOBJTYPES * sizeof *types);
	if (!types)
		return;
	memset(types, 0, NUMMOBJTYPES * sizeof *types);
	{
		// Cost of one clock read, to judge how much the timers inflate sections
		UINT64 t0 = VITA_FrameClock(), t1 = t0;
		INT32 k;

		for (k = 0; k < 1000; k++)
			t1 = VITA_FrameClock();
		clockns = (UINT32)(t1 - t0);
	}
	memset(funcs, 0, sizeof funcs);
	memset(listus, 0, sizeof listus);
	memset(cpus, 0, sizeof cpus);
	memset(actions, 0, sizeof actions);
	numfuncs = numactions = 0;
	cpcalls = cpdepth = 0;
	cpthings = cplines = 0;
	vprof_cpthings = vprof_cplines = 0;
	sightus = 0;
	sightcalls = 0;
	actiondepth = 0;
	curtype = NULL;
	curfunc = NULL;
	tics = 0;
	vprof_on = true;
}

static const char *Prof_TypeName(INT32 type)
{
	if (type < MT_FIRSTFREESLOT)
		return MOBJTYPE_LIST[type];
	if (FREE_MOBJS[type - MT_FIRSTFREESLOT])
		return va("MT_%s", FREE_MOBJS[type - MT_FIRSTFREESLOT]);
	return va("MT_%d", (int)type);
}

static void Prof_Out(FILE *out, const char *line)
{
	I_OutputMsg("%s\n", line);
	if (out)
		fprintf(out, "%s\n", line);
}

#define US10(v) (unsigned)((v) / 10), (unsigned)((v) % 10)
// Microsecond total to "ms per tic" with two decimals
#define MSTIC(v) (unsigned)((v) / n / 1000), (unsigned)((v) / n / 10 % 100)

void VITA_ProfEnd(const char *label, FILE *out)
{
	static const char *listnames[NUM_THINKERLISTS] = {"polyobj", "main", "mobj", "dynslope", "precip"};
	char line[768];
	INT32 i, j, rank;
	const UINT32 n = tics ? tics : 1;
	boolean *used;

	if (!vprof_on)
		return;
	vprof_on = false;

	snprintf(line, sizeof line, "%s lists per tic:", label);
	for (i = 0; i < NUM_THINKERLISTS; i++)
	{
		size_t len = strlen(line);
		UINT64 v = listus[i] / n / 10;

		snprintf(line + len, sizeof line - len, " %s %u.%02u ms", listnames[i], (unsigned)(v / 100), (unsigned)(v % 100));
	}
	Prof_Out(out, line);

	used = calloc(NUMMOBJTYPES, sizeof *used);
	for (rank = 0; used && rank < VPROF_TOPTYPES; rank++)
	{
		INT32 best = -1;
		vproftype_t *t;

		for (i = 0; i < NUMMOBJTYPES; i++)
			if (!used[i] && types[i].us && (best < 0 || types[i].us > types[best].us))
				best = i;
		if (best < 0)
			break;
		used[best] = true;
		t = &types[best];
		snprintf(line, sizeof line,
			"%s type %s: %u.%02u ms/tic, %u/tic, %u.%u us each, checkpos %u/tic %u.%02u ms, sight %u/tic %u.%02u ms, luahooks %u/tic"
			" | head %u.%02u scenery %u.%02u lua %u.%02u think %u.%02u xy %u.%02u z %u.%02u state %u.%02u ms/tic",
			label, Prof_TypeName(best),
			(unsigned)(t->us / n / 1000), (unsigned)(t->us / n / 10 % 100),
			(unsigned)(t->count / n), US10(t->count ? t->us * 10 / t->count : 0),
			(unsigned)(t->checkpos / n), MSTIC(t->cpus),
			(unsigned)(t->sights / n), MSTIC(t->sightus), (unsigned)(t->hooks / n),
			(unsigned)(t->phaseus[VPROF_HEAD] / n / 1000), (unsigned)(t->phaseus[VPROF_HEAD] / n / 10 % 100),
			(unsigned)(t->phaseus[VPROF_SCENERY] / n / 1000), (unsigned)(t->phaseus[VPROF_SCENERY] / n / 10 % 100),
			(unsigned)(t->phaseus[VPROF_LUA] / n / 1000), (unsigned)(t->phaseus[VPROF_LUA] / n / 10 % 100),
			(unsigned)(t->phaseus[VPROF_THINK] / n / 1000), (unsigned)(t->phaseus[VPROF_THINK] / n / 10 % 100),
			(unsigned)(t->phaseus[VPROF_XY] / n / 1000), (unsigned)(t->phaseus[VPROF_XY] / n / 10 % 100),
			(unsigned)(t->phaseus[VPROF_Z] / n / 1000), (unsigned)(t->phaseus[VPROF_Z] / n / 10 % 100),
			(unsigned)(t->phaseus[VPROF_STATE] / n / 1000), (unsigned)(t->phaseus[VPROF_STATE] / n / 10 % 100));
		Prof_Out(out, line);
	}
	free(used);

	for (rank = 0; rank < VPROF_TOPFUNCS; rank++)
	{
		INT32 best = -1;

		for (j = 0; j < numfuncs; j++)
			if (funcs[j].us && (best < 0 || funcs[j].us > funcs[best].us))
				best = j;
		if (best < 0)
			break;
		snprintf(line, sizeof line, "%s func %s %p: %u.%02u ms/tic, %u/tic, checkpos %u/tic %u.%02u ms",
			label, listnames[funcs[best].list], (void *)funcs[best].func,
			(unsigned)(funcs[best].us / n / 1000), (unsigned)(funcs[best].us / n / 10 % 100),
			(unsigned)(funcs[best].count / n), (unsigned)(funcs[best].checkpos / n), MSTIC(funcs[best].cpus));
		Prof_Out(out, line);
		funcs[best].us = 0;
	}

	snprintf(line, sizeof line, "%s checkpos: %u/tic, subsec %u.%02u fof %u.%02u poly %u.%02u things %u.%02u lines %u.%02u ms/tic,"
		" visited %u things/tic %u lines/tic | sight %u/tic %u.%02u ms/tic | clock read %u ns",
		label, (unsigned)(cpcalls / n), MSTIC(cpus[VPROF_CP_SUBSEC]), MSTIC(cpus[VPROF_CP_FOF]),
		MSTIC(cpus[VPROF_CP_POLY]), MSTIC(cpus[VPROF_CP_THINGS]),
		MSTIC(cpus[VPROF_CP_LINES]), (unsigned)(cpthings / n), (unsigned)(cplines / n),
		(unsigned)(sightcalls / n), MSTIC(sightus), (unsigned)clockns);
	Prof_Out(out, line);

	for (rank = 0; rank < VPROF_TOPACTIONS; rank++)
	{
		INT32 best = -1;

		for (j = 0; j < numactions; j++)
			if (actions[j].us && (best < 0 || actions[j].us > actions[best].us))
				best = j;
		if (best < 0)
			break;
		snprintf(line, sizeof line, "%s action %p: %u.%02u ms/tic, self %u.%02u ms/tic, %u/tic",
			label, actions[best].func, MSTIC(actions[best].us), MSTIC(actions[best].selfus),
			(unsigned)(actions[best].count / n));
		Prof_Out(out, line);
		actions[best].us = 0;
	}
	if (out)
		fflush(out);
}

#define VSAMPLE_BUCKETS 16384
#define VSAMPLE_LOGIC 0x80000000u
#define VSAMPLE_PHASE_SHIFT 5
#define VSAMPLE_LIST_SHIFT 8
#define VSAMPLE_TYPE_SHIFT 11
#define VSAMPLE_TYPE_MASK 0x1ffffu
typedef char vsample_type_capacity[(NUMMOBJTYPES + 1 <= VSAMPLE_TYPE_MASK) ? 1 : -1];

typedef struct
{
	UINT32 key, count;
} vsamplebucket_t;

boolean vsample_on;
UINT32 vsample_context;
static UINT32 samplestop;
static UINT32 samplelogicpublished;

UINT32 VITA_SampleLogicSamples(void)
{
	return __atomic_load_n(&samplelogicpublished, __ATOMIC_RELAXED);
}
static SceUID samplethread = -1;
static vsamplebucket_t *samplebuckets;
static UINT32 samplecount, samplelost, samplemin, samplemax;
static UINT64 sampleelapsed, samplelogicus;
static UINT32 sampletics;

static int Sample_Thread(SceSize args, void *argp)
{
	UINT32 jitter = 0x53425232u;
	UINT32 logiccount = 0;
	UINT64 previous = VITA_FrameClock();

	(void)args;
	(void)argp;
	while (!__atomic_load_n(&samplestop, __ATOMIC_ACQUIRE))
	{
		UINT32 key, slot, tries;
		UINT64 now, interval;

		jitter = jitter * 1664525u + 1013904223u;
		sceKernelDelayThread(25 + jitter % 51);
		now = VITA_FrameClock();
		interval = now - previous;
		previous = now;
		sampleelapsed += interval;
		if (interval < samplemin)
			samplemin = (UINT32)interval;
		if (interval > samplemax)
			samplemax = (UINT32)interval;
		key = __atomic_load_n(&vsample_context, __ATOMIC_RELAXED);
		if (key & VSAMPLE_LOGIC)
			__atomic_store_n(&samplelogicpublished, ++logiccount, __ATOMIC_RELAXED);
		slot = (key * 2654435761u) >> 18;
		for (tries = 0; tries < VSAMPLE_BUCKETS; tries++)
		{
			vsamplebucket_t *b = &samplebuckets[slot];

			if (!b->count || b->key == key)
			{
				b->key = key;
				b->count++;
				break;
			}
			slot = (slot + 1) & (VSAMPLE_BUCKETS - 1);
		}
		if (tries == VSAMPLE_BUCKETS)
			samplelost++;
		samplecount++;
	}
	return 0;
}

void VITA_SampleLogic(boolean active)
{
	if (vsample_on)
		__atomic_store_n(&vsample_context, active ? VSAMPLE_LOGIC : 0, __ATOMIC_RELAXED);
}

void VITA_SamplePhase(vprofphase_t phase)
{
	UINT32 key = __atomic_load_n(&vsample_context, __ATOMIC_RELAXED);

	key = (key & ~(7u << VSAMPLE_PHASE_SHIFT)) | ((UINT32)phase << VSAMPLE_PHASE_SHIFT);
	__atomic_store_n(&vsample_context, key, __ATOMIC_RELAXED);
}

void VITA_SampleThinker(size_t list, thinker_t *th)
{
	UINT32 saved = __atomic_load_n(&vsample_context, __ATOMIC_RELAXED);
	UINT32 type = 0;
	UINT32 key;

	if (list == THINK_MOBJ && th->function == (think_t)P_MobjThinker)
		type = (UINT32)((mobj_t *)th)->type + 1;
	key = (saved & VSAMPLE_LOGIC) | ((UINT32)(list + 1) << VSAMPLE_LIST_SHIFT)
		| (type << VSAMPLE_TYPE_SHIFT);
	__atomic_store_n(&vsample_context, key, __ATOMIC_RELAXED);
	th->function(th);
	__atomic_store_n(&vsample_context, saved, __ATOMIC_RELAXED);
}

void VITA_SampleTic(UINT32 logicus)
{
	if (vsample_on)
	{
		sampletics++;
		samplelogicus += logicus;
	}
}

void VITA_SampleBegin(void)
{
	INT32 result;

	if (vsample_on || vprof_on)
		return;
	samplebuckets = calloc(VSAMPLE_BUCKETS, sizeof *samplebuckets);
	if (!samplebuckets)
	{
		I_OutputMsg("Sampling unavailable: histogram allocation failed\n");
		return;
	}
	samplecount = samplelost = samplemax = sampletics = 0;
	__atomic_store_n(&samplelogicpublished, 0, __ATOMIC_RELAXED);
	samplemin = UINT32_MAX;
	sampleelapsed = samplelogicus = 0;
	__atomic_store_n(&vsample_context, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&samplestop, 0, __ATOMIC_RELEASE);
	samplethread = sceKernelCreateThread("srb2_sample", Sample_Thread, 0x100000F0,
		16 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
	result = samplethread < 0 ? samplethread : sceKernelStartThread(samplethread, 0, NULL);
	if (result < 0)
	{
		I_OutputMsg("Sampling unavailable: thread start 0x%08x\n", (unsigned)result);
		if (samplethread >= 0)
			sceKernelDeleteThread(samplethread);
		samplethread = -1;
		free(samplebuckets);
		samplebuckets = NULL;
		return;
	}
	vsample_on = true;
}

void VITA_SampleEnd(const char *label, FILE *out)
{
	static const char *sitenames[VSAMPLE_NUMSITES] = {"other", "cp-subsec", "cp-fof", "cp-poly",
		"cp-things", "cp-lines", "change-sector", "sight", "unlink", "link", "secnodes", "action"};
	char line[512];
	UINT32 i, logic = 0, sites[VSAMPLE_NUMSITES] = {0};
	UINT32 *typecounts;
	INT32 result;

	if (!vsample_on)
	{
		Prof_Out(out, "Sampling unavailable: no active sampler");
		return;
	}
	vsample_on = false;
	__atomic_store_n(&samplestop, 1, __ATOMIC_RELEASE);
	result = sceKernelWaitThreadEnd(samplethread, NULL, NULL);
	if (result < 0)
		I_Error("Sampler join failed: 0x%08x", (unsigned)result);
	sceKernelDeleteThread(samplethread);
	samplethread = -1;
	typecounts = calloc(NUMMOBJTYPES + 1, sizeof *typecounts);
	for (i = 0; i < VSAMPLE_BUCKETS; i++)
	{
		vsamplebucket_t *b = &samplebuckets[i];
		UINT32 site = b->key & 31u;
		UINT32 type = (b->key >> VSAMPLE_TYPE_SHIFT) & VSAMPLE_TYPE_MASK;

		if (!b->count || !(b->key & VSAMPLE_LOGIC))
			continue;
		logic += b->count;
		if (site < VSAMPLE_NUMSITES)
			sites[site] += b->count;
		if (typecounts && type <= NUMMOBJTYPES)
			typecounts[type] += b->count;
	}
	snprintf(line, sizeof line,
		"%s sample: total %u logic %u lost %u tics %u elapsed %llu us interval %llu us min %u max %u | logic sampled %llu us/tic logger %llu us/tic",
		label, (unsigned)samplecount, (unsigned)logic, (unsigned)samplelost, (unsigned)sampletics,
		(unsigned long long)sampleelapsed,
		(unsigned long long)(samplecount ? sampleelapsed / samplecount : 0),
		(unsigned)samplemin, (unsigned)samplemax,
		(unsigned long long)(samplecount && sampletics ? sampleelapsed * logic / samplecount / sampletics : 0),
		(unsigned long long)(sampletics ? samplelogicus / sampletics : 0));
	Prof_Out(out, line);
	for (i = 0; i < VSAMPLE_NUMSITES; i++)
	{
		snprintf(line, sizeof line, "%s sample-site %s: %u %.2f%% logic", label, sitenames[i],
			(unsigned)sites[i], logic ? sites[i] * 100.0 / logic : 0.0);
		Prof_Out(out, line);
	}
	for (i = 0; typecounts && i <= NUMMOBJTYPES; i++)
	{
		if (!typecounts[i])
			continue;
		snprintf(line, sizeof line, "%s sample-type %s: %u %.2f%% logic", label,
			i ? Prof_TypeName((INT32)i - 1) : "none", (unsigned)typecounts[i],
			logic ? typecounts[i] * 100.0 / logic : 0.0);
		Prof_Out(out, line);
	}
	for (i = 0; i < VSAMPLE_BUCKETS; i++)
	{
		vsamplebucket_t *b = &samplebuckets[i];
		UINT32 site = b->key & 31u;
		UINT32 type = (b->key >> VSAMPLE_TYPE_SHIFT) & VSAMPLE_TYPE_MASK;

		if (!b->count)
			continue;
		snprintf(line, sizeof line, "%s sample-tuple logic %u list %u type %u phase %u site %s caller %u: %u",
			label, (unsigned)!!(b->key & VSAMPLE_LOGIC),
			(unsigned)((b->key >> VSAMPLE_LIST_SHIFT) & 7u), (unsigned)type,
			(unsigned)((b->key >> VSAMPLE_PHASE_SHIFT) & 7u),
			site < VSAMPLE_NUMSITES ? sitenames[site] : "invalid", (unsigned)((b->key >> 28) & 7u), (unsigned)b->count);
		Prof_Out(out, line);
	}
	free(typecounts);
	free(samplebuckets);
	samplebuckets = NULL;
	if (out)
		fflush(out);
}
