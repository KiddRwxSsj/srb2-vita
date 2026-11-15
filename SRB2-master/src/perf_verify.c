// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  perf_verify.c
/// \brief Host-only helpers for performance shadow verification

#include "perf_verify.h"
#include "i_system.h"

void PERF_VerifyValue(const char *query, const char *field, UINT64 fast, UINT64 original)
{
	if (fast != original)
		I_Error("PERF_VERIFY %s: %s fast=0x%llx original=0x%llx", query, field,
			(unsigned long long)fast, (unsigned long long)original);
}

void PERF_VerifyBytes(const char *query, const char *field,
	const void *fast, const void *original, size_t size)
{
	const UINT8 *a = fast, *b = original;
	size_t i;

	for (i = 0; i < size; i++)
		if (a[i] != b[i])
			I_Error("PERF_VERIFY %s: %s byte %lu fast=0x%02x original=0x%02x", query, field,
				(unsigned long)i, (unsigned)a[i], (unsigned)b[i]);
}
