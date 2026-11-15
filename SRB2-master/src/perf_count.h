/* Diagnostic query eligibility. No query answer is ever substituted. */
#ifndef __PERF_COUNT_H__
#define __PERF_COUNT_H__
#include <stdio.h>
#include "doomtype.h"
#include "p_mobj.h"

typedef enum
{
    PC_OTHER, PC_SCENERY_Z, PC_MOBJ_Z, PC_HEIGHT_FIRST, PC_HEIGHT_SECOND,
    PC_TRYMOVE, PC_CALLERS
} pccaller_t;

typedef enum
{
    PC_DEP_OTHER, PC_DEP_CELL, PC_DEP_SECTOR_HEIGHT, PC_DEP_FOF_HEIGHT,
    PC_DEP_LINE_FLAGS, PC_DEP_ROOT, PC_DEP_SLOPE, PC_DEP_KINDS
} pcdepkind_t;
typedef enum { PC_MUT_OTHER, PC_MUT_LINK, PC_MUT_UNLINK, PC_MUT_WRITE } pcmutation_t;

#if defined(SRB2_PERF_COUNT) || defined(__vita__)
boolean P_CheckPositionCaller(mobj_t *, fixed_t, fixed_t, pccaller_t);
#define PC_CHECK(mo, x, y, caller) P_CheckPositionCaller(mo, x, y, caller)
#else
#define PC_CHECK(mo, x, y, caller) P_CheckPosition(mo, x, y)
#endif

#ifdef SRB2_PERF_COUNT
extern boolean pcount_on;
boolean LUA_PerfCollisionHooks(mobjtype_t type);
UINT64 PC_Begin(mobj_t *, fixed_t, fixed_t, pccaller_t);
void PC_Input(const void *, size_t);
void PC_NamedInput(const void *, size_t, const char *);
void PC_Dependency(const void *, size_t, pcdepkind_t);
void PC_ThinkerAdded(thinker_t *);
void PC_SlopeLuaWrite(struct pslope_s *);
void PC_UnlinkBegin(mobj_t *);
void PC_LinkEnd(mobj_t *);
void PC_Sector(sector_t *);
void PC_Line(line_t *);
void PC_Thing(mobj_t *);
void PC_Cells(INT32, INT32, INT32, INT32);
void PC_End(UINT64, boolean, const UINT64 *, size_t);
void PC_CellMutation(INT32);
void PC_CellMutationFrom(INT32, mobj_t *, pcmutation_t);
fixed_t PC_Write(mobj_t *, fixed_t *, fixed_t);
void PC_Forget(mobj_t *);
void PC_Reset(void);
void PC_Dump(FILE *);
void PC_SetOutput(FILE *);
#define PC_WRITE(mo, field, value) PC_Write((mo), &(mo)->field, (value))
#define PC_INPUT(value) PC_NamedInput(&(value), sizeof(value), #value)
#define PC_DEP(value, kind) PC_Dependency(&(value), sizeof(value), (kind))
#else
#define PC_WRITE(mo, field, value) ((mo)->field = (value))
#define PC_INPUT(value) ((void)0)
#define PC_DEP(value, kind) ((void)0)
#define PC_ThinkerAdded(thinker) ((void)0)
#define PC_SlopeLuaWrite(slope) ((void)0)
#define PC_UnlinkBegin(thing) ((void)0)
#define PC_LinkEnd(thing) ((void)0)
#define PC_Sector(sector) ((void)0)
#define PC_Line(line) ((void)0)
#define PC_Thing(thing) ((void)0)
#define PC_Cells(xl, xh, yl, yh) ((void)0)
#define PC_CellMutation(cell) ((void)0)
#define PC_CellMutationFrom(cell, thing, reason) ((void)0)
#define PC_Forget(thing) ((void)0)
#define PC_Reset() ((void)0)
#define PC_Dump(out) ((void)0)
#define PC_SetOutput(out) ((void)0)
#endif
#endif
