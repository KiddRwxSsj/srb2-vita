// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vita_framelog.h
/// \brief Frame time and hitch log for PS Vita

#ifndef __VITA_FRAMELOG_H__
#define __VITA_FRAMELOG_H__

#include <stdio.h>
#include <stddef.h>
#include "../doomtype.h"

typedef enum
{
	VFL_TICS,
	VFL_DISPLAY,
	VFL_OTHER,
	VFL_NUMPHASES
} vflphase_t;

UINT64 VITA_FrameClock(void);
void VITA_FrameBegin(void);
void VITA_FramePhase(vflphase_t phase);
void VITA_FrameEnd(UINT32 tics);

// Engine perfstats, sampled right after each gameplay tic and each level frame
void VITA_FrameTicStats(void);
void VITA_FrameRenderStats(void);

// Measurement segments, used by the benchmark run
void VITA_FrameSegmentBegin(void);
void VITA_FrameSegmentEnd(const char *label, FILE *out);
// Opt-in stability log; NULL disarms before the file is closed.
void VITA_FrameStability(FILE *out);

// Safe to call from any thread
void VITA_FrameCountRead(size_t bytes, UINT32 us);
void VITA_FrameCountUpload(size_t bytes);
void VITA_FrameCountPresentWait(UINT32 us);
void VITA_FrameCountRender(UINT32 us);
void VITA_FrameCountZoneWait(UINT32 us);

#endif
