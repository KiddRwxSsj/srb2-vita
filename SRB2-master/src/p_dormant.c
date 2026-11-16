// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for details.
//-----------------------------------------------------------------------------
#include <stdlib.h>
#include <string.h>
#include "doomstat.h"
#include "g_game.h"
#include "g_demo.h"
#include "p_local.h"
#include "p_setup.h"
#include "p_dormant.h"
#include "console.h"

#define DORMANT_ASLEEP 1u
#define DORMANT_DIRTY 2u
#define DORMANT_REFRESH 4u
#define DORMANT_WAKE 8u

boolean p_dormant_active, p_dormant_used;
static boolean dormant_requested, dormant_forced;
static UINT64 dormant_checks, dormant_skips, dormant_wakes, dormant_refreshes, dormant_clips;
static void DormantChanged(void);
static CV_PossibleValue_t dormant_distance[] = {{512, "MIN"}, {16384, "MAX"}, {0, NULL}};
#ifdef __vita__
#define DORMANT_DEFAULT "On"
#else
#define DORMANT_DEFAULT "Off"
#endif
static consvar_t cv_dormant = CVAR_INIT("vitadormant", DORMANT_DEFAULT,
	CV_SAVE|CV_CALL, CV_OnOff, DormantChanged);
static consvar_t cv_dormantdist = CVAR_INIT("dormantdist", "4096",
	CV_SAVE, dormant_distance, NULL);

static boolean DormantBlocked(void)
{
	return netgame || multiplayer || demorecording || demoplayback || metalrecording || metalplayback;
}
void P_DormantUpdate(void)
{
	boolean blocked = DormantBlocked();
	if (blocked && dormant_requested)
	{
		if (!dormant_forced)
			CONS_Printf("Dormant objects forced Off for multiplayer and demos. Single-player mode changes simulation.\n");
		if (cv_dormant.value) CV_StealthSet(&cv_dormant, "Off");
		dormant_forced = true;
	}
	else if (!blocked && dormant_forced)
	{
		CV_StealthSet(&cv_dormant, dormant_requested ? "On" : "Off");
		dormant_forced = false;
	}
	p_dormant_active = !blocked && dormant_requested && cv_dormant.value
		&& gamestate == GS_LEVEL && !levelloading;
}
static void DormantChanged(void)
{
	dormant_requested = cv_dormant.value != 0;
	dormant_forced = false;
	P_DormantUpdate();
}
void P_DormantInit(void)
{
	CV_RegisterVar(&cv_dormant);
	CV_RegisterVar(&cv_dormantdist);
}
void P_DormantResetStats(void)
{
	dormant_checks = dormant_skips = dormant_wakes = dormant_refreshes = dormant_clips = 0;
}
void P_DormantReset(void)
{
	p_dormant_active = p_dormant_used = false;
	P_DormantResetStats();
}
void P_DormantTouch(mobj_t *mo)
{
	if (!mo || (!p_dormant_active && !p_dormant_used) || levelloading) return;
	if (mo->dormant_flags & DORMANT_ASLEEP) dormant_wakes++;
	if (mo->dormant_flags & (DORMANT_ASLEEP|DORMANT_DIRTY)) mo->dormant_flags |= DORMANT_WAKE;
	mo->dormant_flags &= ~DORMANT_ASLEEP;
	mo->dormant_touch = leveltime;
}
mobj_t *P_DormantLua(mobj_t *mo)
{
	P_DormantTouch(mo);
	return mo;
}
static boolean DormantNear(mobj_t *mo, mobj_t *view)
{
	INT64 scale, reach;
	if (!view || P_MobjWasRemoved(view)) return false;
	scale = max(FRACUNIT, max(mo->scale, view->scale));
	reach = (INT64)cv_dormantdist.value * scale + mo->radius + view->radius;
	return llabs((INT64)mo->x - view->x) <= reach && llabs((INT64)mo->y - view->y) <= reach;
}
static boolean DormantEligible(mobj_t *mo)
{
	INT32 i;
	INT64 reach;
	mobj_t *refs[5];
	if (!p_dormant_active || DormantBlocked() || leveltime < TICRATE
		|| mo->thinker.removing || mo->player || mo->health <= 0 || mo->fuse
		|| mo->momx || mo->momy || mo->momz || mo->scale != mo->destscale
		|| mo->type >= MT_FIRSTFREESLOT || mo->dormant_touch == leveltime
		|| (mo->dormant_flags & DORMANT_WAKE)
		|| (mo->flags & (MF_ENEMY|MF_BOSS|MF_MISSILE|MF_PUSHABLE|MF_SPRING|MF_SOLID))
		|| (mo->info->flags & MF_PUSHABLE) || LUA_DormantHook(mo->type)) return false;
	refs[0] = mo->target; refs[1] = mo->tracer; refs[2] = mo->hnext;
	refs[3] = mo->hprev; refs[4] = mo->dontdrawforviewmobj;
	for (i = 0; i < 5; ++i)
		if (refs[i] && (P_MobjWasRemoved(refs[i]) || refs[i]->thinker.removing || refs[i]->player)) return false;
	for (i = 0; i < MAXPLAYERS; ++i)
		if (playeringame[i] && (DormantNear(mo, players[i].mo)
			|| (players[i].awayviewtics && DormantNear(mo, players[i].awayviewmobj)))) return false;
	reach = (INT64)cv_dormantdist.value * max(FRACUNIT, mo->scale) + mo->radius;
	if (camera.chase && llabs((INT64)mo->x - camera.x) <= reach
		&& llabs((INT64)mo->y - camera.y) <= reach) return false;
	if (splitscreen && camera2.chase && llabs((INT64)mo->x - camera2.x) <= reach
		&& llabs((INT64)mo->y - camera2.y) <= reach) return false;
	return true;
}
boolean P_DormantSkip(mobj_t *mo)
{
	if (p_dormant_active) dormant_checks++;
	if (DormantEligible(mo))
	{
		mo->dormant_flags |= DORMANT_ASLEEP|DORMANT_DIRTY;
		p_dormant_used = true;
		dormant_skips++;
		return true;
	}
	P_DormantTouch(mo);
	if ((mo->dormant_flags & DORMANT_DIRTY) && !(mo->dormant_flags & DORMANT_REFRESH))
	{
		mo->dormant_flags = DORMANT_REFRESH;
		dormant_refreshes++;
		P_DormantRefresh(mo);
		mo->dormant_flags = 0;
		if (P_MobjWasRemoved(mo)) return true;
	}
	mo->dormant_flags &= ~DORMANT_WAKE;
	return false;
}
boolean P_DormantClip(mobj_t *mo)
{
	if (p_dormant_active && (mo->dormant_flags & DORMANT_ASLEEP) && DormantEligible(mo))
	{
		mo->dormant_flags |= DORMANT_DIRTY;
		dormant_clips++;
		return true;
	}
	P_DormantTouch(mo);
	return false;
}
void P_DormantReport(FILE *out)
{
	if (!out) out = stdout;
	fprintf(out, "Dormancy|map=MAP%02d|active=%u|distance=%d|checks=%llu|skipped=%llu|wakes=%llu|refreshes=%llu|clipping_skipped=%llu\n",
		gamemap, (unsigned)p_dormant_active, cv_dormantdist.value,
		(unsigned long long)dormant_checks, (unsigned long long)dormant_skips,
		(unsigned long long)dormant_wakes, (unsigned long long)dormant_refreshes,
		(unsigned long long)dormant_clips);
	fflush(out);
}
