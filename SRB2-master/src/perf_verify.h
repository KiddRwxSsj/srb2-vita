// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  perf_verify.h
/// \brief Host-only helpers for performance shadow verification

#ifndef __PERF_VERIFY_H__
#define __PERF_VERIFY_H__

#include "doomtype.h"

#ifdef SRB2_PERF_VERIFY
#ifdef __vita__
#error SRB2_PERF_VERIFY is host-only
#endif

void PERF_VerifyValue(const char *query, const char *field, UINT64 fast, UINT64 original);
// Byte snapshots must be explicitly serialized, without struct padding.
void PERF_VerifyBytes(const char *query, const char *field,
	const void *fast, const void *original, size_t size);
#endif

#endif
