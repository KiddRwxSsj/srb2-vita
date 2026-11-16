// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for details.
//-----------------------------------------------------------------------------
#ifndef __P_DORMANT_H__
#define __P_DORMANT_H__
#include <stdio.h>
#include "p_mobj.h"

extern boolean p_dormant_active, p_dormant_used;
void P_DormantInit(void);
void P_DormantUpdate(void);
void P_DormantReset(void);
void P_DormantTouch(mobj_t *);
mobj_t *P_DormantLua(mobj_t *);
boolean P_DormantSkip(mobj_t *);
boolean P_DormantClip(mobj_t *);
void P_DormantRefresh(mobj_t *);
boolean LUA_DormantHook(mobjtype_t);
void P_DormantReport(FILE *);
void P_DormantResetStats(void);
#define P_DORMANT_SKIP(mo) ((p_dormant_active || p_dormant_used) && P_DormantSkip(mo))
#endif
