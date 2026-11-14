// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vita_prof.h
/// \brief Per thinker type profiler for PS Vita benchmark runs

#ifndef __VITA_PROF_H__
#define __VITA_PROF_H__

#include <stdio.h>
#include "../doomtype.h"
#include "../d_think.h"

typedef enum
{
	VPROF_HEAD,
	VPROF_SCENERY,
	VPROF_LUA,
	VPROF_THINK,
	VPROF_XY,
	VPROF_Z,
	VPROF_STATE,
	VPROF_NUMPHASES
} vprofphase_t;

typedef enum
{
	VPROF_CP_SUBSEC, // subsector lookup and base floor/ceiling
	VPROF_CP_FOF,
	VPROF_CP_POLY,
	VPROF_CP_THINGS,
	VPROF_CP_LINES,
	VPROF_CP_NUMSECTIONS
} vprofcpsection_t;

extern boolean vprof_on;
extern UINT32 vprof_cpthings, vprof_cplines;

void VITA_ProfThinker(size_t list, thinker_t *th);
void VITA_ProfEnter(vprofphase_t phase);
void VITA_ProfTic(void);
void VITA_ProfBegin(void);
void VITA_ProfEnd(const char *label, FILE *out);

// P_CheckPosition sections. Only the outermost call is timed, nested calls
// count toward the section that triggered them.
UINT64 VITA_ProfCPBegin(void);
void VITA_ProfCPSection(vprofcpsection_t section, UINT64 *mark);
void VITA_ProfCPEnd(void);
// Line of sight checks
UINT64 VITA_ProfSightBegin(void);
void VITA_ProfSightEnd(UINT64 start);
// State action functions, inclusive and exclusive of nested actions
void VITA_ProfActionBegin(void *action);
void VITA_ProfActionEnd(void);

typedef enum
{
	VSAMPLE_NONE,
	VSAMPLE_CP_SUBSEC,
	VSAMPLE_CP_FOF,
	VSAMPLE_CP_POLY,
	VSAMPLE_CP_THINGS,
	VSAMPLE_CP_LINES,
	VSAMPLE_CHANGESECTOR,
	VSAMPLE_SIGHT,
	VSAMPLE_UNLINK,
	VSAMPLE_LINK,
	VSAMPLE_SECNODES,
	VSAMPLE_ACTION,
	VSAMPLE_NUMSITES
} vsamplesite_t;

extern boolean vsample_on;
extern UINT32 vsample_context;

void VITA_SampleBegin(void);
void VITA_SampleEnd(const char *label, FILE *out);
void VITA_SampleThinker(size_t list, thinker_t *th);
void VITA_SamplePhase(vprofphase_t phase);
void VITA_SampleLogic(boolean active);
void VITA_SampleTic(UINT32 logicus);
UINT32 VITA_SampleLogicSamples(void);

static inline UINT32 VITA_SamplePush(vsamplesite_t site)
{
	UINT32 saved = 0;

	if (vsample_on)
	{
		saved = __atomic_load_n(&vsample_context, __ATOMIC_RELAXED);
		__atomic_store_n(&vsample_context, (saved & ~31u) | site, __ATOMIC_RELAXED);
	}
	return saved;
}

static inline void VITA_SamplePop(UINT32 saved)
{
	if (vsample_on)
		__atomic_store_n(&vsample_context, saved, __ATOMIC_RELAXED);
}

// Bits 28..30 are reserved for the exact P_CheckPosition caller.
static inline UINT32 VITA_SamplePushCaller(UINT32 caller)
{
	UINT32 saved = 0;
	if (vsample_on)
	{
		saved = __atomic_load_n(&vsample_context, __ATOMIC_RELAXED);
		__atomic_store_n(&vsample_context, (saved & ~(7u << 28)) | (caller << 28), __ATOMIC_RELAXED);
	}
	return saved;
}

static inline void VITA_SampleSite(vsamplesite_t site)
{
	(void)VITA_SamplePush(site);
}

#define VPROF_ENTER(phase) do { if (vprof_on) VITA_ProfEnter(phase); if (vsample_on) VITA_SamplePhase(phase); } while (0)
#define VPROF_CP_BEGIN(mark) UINT32 mark ## _sample = VITA_SamplePush(VSAMPLE_CP_SUBSEC); UINT64 mark = vprof_on ? VITA_ProfCPBegin() : 0
#define VPROF_CP_SECTION(section, mark) do { if (mark) VITA_ProfCPSection(section, &mark); if ((section) < VPROF_CP_LINES) VITA_SampleSite((vsamplesite_t)(VSAMPLE_CP_SUBSEC + (section) + 1)); } while (0)
#define VPROF_CP_END(mark) do { if (mark) VITA_ProfCPEnd(); VITA_SamplePop(mark ## _sample); } while (0)
#define VPROF_COUNT(counter) do { if (vprof_on) counter++; } while (0)
#define VPROF_SIGHT_BEGIN(mark) UINT32 mark ## _sample = VITA_SamplePush(VSAMPLE_SIGHT); UINT64 mark = vprof_on ? VITA_ProfSightBegin() : 0
#define VPROF_SIGHT_END(mark) do { if (mark) VITA_ProfSightEnd(mark); VITA_SamplePop(mark ## _sample); } while (0)
#define VPROF_ACTION(action, call) do { UINT32 vsaved = VITA_SamplePush(VSAMPLE_ACTION); if (vprof_on) { VITA_ProfActionBegin((void *)(action)); call; VITA_ProfActionEnd(); } else { call; } VITA_SamplePop(vsaved); } while (0)

#endif
