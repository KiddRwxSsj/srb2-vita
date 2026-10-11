// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vita_rthread.h
/// \brief Runs the OpenGL backend on a second core for PS Vita

#ifndef __VITA_RTHREAD_H__
#define __VITA_RTHREAD_H__

#include "../doomtype.h"
#include "../hardware/hw_batching.h"

extern boolean vita_rt_enabled;
extern boolean vita_pgen_enabled;

void VITA_RT_Init(void);
void VITA_RT_FinishUpdate(INT32 waitvbl);
void VITA_RT_Sync(void);
void VITA_RT_Stop(void);
void VITA_ZoneLock(void);
void VITA_ZoneUnlock(void);
boolean VITA_RT_RenderBatches(PolygonArrayEntry *polys, INT32 numpolys, FOutVector *verts, INT32 numverts);

#endif
