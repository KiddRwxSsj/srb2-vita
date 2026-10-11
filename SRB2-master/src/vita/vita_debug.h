// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vita_debug.h
/// \brief Touch debug panel for PS Vita

#ifndef __VITA_DEBUG_H__
#define __VITA_DEBUG_H__

#include "../doomtype.h"

#define VDBG_NOWALLS   0x1
#define VDBG_NOPLANES  0x2
#define VDBG_NOSPRITES 0x4
#define VDBG_NODRAW    0x8

extern UINT32 vita_dbgflags;
extern boolean vita_dbg_in3d;

void VITA_DebugInit(void);
void VITA_DebugPollTouch(void);
void VITA_DebugDraw(void);

#endif
