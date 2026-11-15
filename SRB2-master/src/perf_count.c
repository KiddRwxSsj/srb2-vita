/* Count a conservative, replay-safe memo policy while executing every query. */
#include <stdlib.h>
#include <string.h>
#include "doomdef.h"
#include "doomstat.h"
#include "g_game.h"
#include "p_local.h"
#include "p_setup.h"
#include "r_state.h"
#include "i_system.h"
#include "lua_hook.h"
#include "perf_count.h"
#include "p_slopes.h"

#define PC_SLOTS 32768u
#define PC_DEPS 1024u
#define PC_DEPTH 8u
#define PC_OUTPUTS 64u
#define PC_SLOPES 8192u
#define PC_DEP_HASH (PC_DEPS * 2u)
static const char *const named_inputs[] = {"tmdrpoffceilz", "opentop", "openbottom", "openrange",
    "lowfloor", "highceiling", "openfloorrover", "openceilingrover", "opentopslope", "openbottomslope",
    "rover->fofflags", "rover->secnum", "line->special", "line->sidenum[0]", "line->sidenum[1]"};
#define PC_NAMES (sizeof named_inputs / sizeof named_inputs[0])
enum { R_PLAYER, R_REMOVED, R_HOOK, R_SLOPE, R_POLY, R_GOO, R_MIDTEX,
    R_PAPER, R_XY, R_NESTED, R_CAPACITY, R_COLD, R_INPUT, R_DEP, R_MISMATCH, R_COUNT };
static const char *const reasons[R_COUNT] = {"player", "removed", "lua", "slope", "poly",
    "goo", "midtexture", "paper", "non_xy_visit", "nested", "capacity", "cold",
    "input_changed", "dependency_changed", "output_mismatch"};
static const char *const callers[PC_CALLERS] = {"other", "scenery_z", "mobj_z",
    "height_first", "height_second", "trymove"};
typedef struct
{
    const void *address;
    UINT64 value, inplace;
    size_t size;
    pcdepkind_t kind;
    const char *name;
} pcdep_t;
typedef struct
{
    pslope_t *slope;
    boolean dynamic, lua;
} pcslope_t;
typedef struct
{
    mobj_t *last_thing;
    pcmutation_t last_kind;
    UINT64 inplace;
} pccell_t;
typedef struct
{
    fixed_t qx, qy, x, y, z, radius, height, momz, scale;
    UINT64 flags, flags2, eflags, angle, type;
    const player_t *player;
} pcinput_t;
typedef struct
{
    boolean valid, answer;
    pcinput_t input;
    pcdep_t *deps;
    size_t ndeps, capacity, noutputs;
    UINT64 outputs[PC_OUTPUTS];
} pcrecord_t;
typedef struct
{
    mobj_t *thing;
    pcrecord_t *record;
    fixed_t unlink_x, unlink_y, unlink_radius;
    INT32 *unlink_cells;
    size_t nunlink, unlink_capacity;
    boolean pending;
} pcslot_t;
typedef struct
{
    mobj_t *thing;
    pcinput_t input;
    pcdep_t deps[PC_DEPS];
    UINT16 dep_hash[PC_DEP_HASH];
    size_t ndeps;
    UINT32 reject;
    pccaller_t caller;
    pcrecord_t *previous;
    boolean potential;
    boolean static_slope, dynamic_slope, lua_slope, paper_far;
} pcframe_t;
typedef struct
{
    UINT64 calls, eligible, hits, reject[R_COUNT], dep[PC_DEP_KINDS];
    UINT64 named_dep[PC_NAMES + 1];
    UINT64 static_slope_queries, static_slope_hits, dynamic_slope, lua_slope;
    UINT64 paper_far_visits, paper_far_queries, paper_far_hits, paper_inside;
    UINT64 cell_last_link, cell_last_unlink, cell_last_write, cell_last_other;
    UINT64 cell_last_neighbor, cell_inplace_relink;
} pcstats_t;

#ifdef __vita__
boolean pcount_on;
#else
boolean pcount_on = true; /* Only separate diagnostic builds link this file. */
#endif
static pcslot_t slots[PC_SLOTS];
static pcframe_t frames[PC_DEPTH];
static pcstats_t stats[PC_CALLERS];
static size_t depth, ncells;
static UINT64 *generations;
static pccell_t *cellmeta;
static pcslope_t slope_states[PC_SLOPES];
static boolean slope_state_failed;
static UINT32 epoch = 1;
static INT16 countmap;
static boolean registered, generation_failed;
static FILE *report_output;

void PC_SetOutput(FILE *out) { report_output = out; }

static pcframe_t *Current(void) { return depth ? &frames[depth - 1] : NULL; }
static void Reject(unsigned reason)
{
    pcframe_t *frame = Current();
    if (frame) frame->reject |= 1u << reason;
}
static pcslot_t *Slot(mobj_t *thing, boolean create)
{
    size_t index = ((uintptr_t)thing >> 4) * 2654435761u & (PC_SLOTS - 1);
    size_t i;
    for (i = 0; i < PC_SLOTS; ++i, index = (index + 1) & (PC_SLOTS - 1))
    {
        pcslot_t *slot = &slots[index];
        if (slot->thing == thing) return slot;
        if (!slot->thing)
        {
            if (create) slot->thing = thing;
            return create ? slot : NULL;
        }
    }
    return NULL;
}
static UINT64 ReadValue(const void *address, size_t size)
{
    UINT64 value = 0;
    memcpy(&value, address, size);
    return value;
}
static void RecordDependency(const void *address, size_t size, pcdepkind_t kind, const char *name)
{
    pcframe_t *frame = Current();
    size_t index;
    if (!frame || frame->reject) return;
    if (!address || !size || size > sizeof(UINT64)) { Reject(R_CAPACITY); return; }
    index = ((((uintptr_t)address >> 2) * 2654435761u) ^ size) & (PC_DEP_HASH - 1);
    while (frame->dep_hash[index])
    {
        const pcdep_t *dep = &frame->deps[frame->dep_hash[index] - 1];
        if (dep->address == address && dep->size == size) return;
        index = (index + 1) & (PC_DEP_HASH - 1);
    }
    if (frame->ndeps == PC_DEPS) { Reject(R_CAPACITY); return; }
    frame->dep_hash[index] = (UINT16)(frame->ndeps + 1);
    frame->deps[frame->ndeps++] = (pcdep_t){address, ReadValue(address, size), 0, size, kind, name};
}
void PC_Dependency(const void *address, size_t size, pcdepkind_t kind) { RecordDependency(address, size, kind, NULL); }
void PC_Input(const void *address, size_t size) { RecordDependency(address, size, PC_DEP_OTHER, NULL); }
void PC_NamedInput(const void *address, size_t size, const char *name) { RecordDependency(address, size, PC_DEP_OTHER, name); }
static boolean SameDependencies(const pcrecord_t *record, pcframe_t *frame)
{
    size_t i;
    /* Root pointers precede dereferences; stop before reading an obsolete child. */
    for (i = 0; i < record->ndeps; ++i)
        if (ReadValue(record->deps[i].address, record->deps[i].size) != record->deps[i].value)
        {
            const pcdep_t *dep = &record->deps[i];
            pcstats_t *row = &stats[frame->caller];
            row->dep[dep->kind]++;
            if (dep->kind == PC_DEP_OTHER)
            {
                size_t n;
                for (n = 0; n < PC_NAMES; ++n)
                    if (dep->name && !strcmp(dep->name, named_inputs[n])) break;
                row->named_dep[n]++;
            }
            if (dep->kind == PC_DEP_CELL)
            {
                size_t cell = (const UINT64 *)dep->address - generations;
                const pccell_t *meta = &cellmeta[cell];
                if (meta->last_kind == PC_MUT_LINK) row->cell_last_link++;
                else if (meta->last_kind == PC_MUT_UNLINK) row->cell_last_unlink++;
                else if (meta->last_kind == PC_MUT_WRITE) row->cell_last_write++;
                else row->cell_last_other++;
                if (meta->last_thing && meta->last_thing != frame->thing) row->cell_last_neighbor++;
                if (meta->inplace != dep->inplace) row->cell_inplace_relink++;
            }
            return false;
        }
    return true;
}
static boolean SameInput(const pcinput_t *a, const pcinput_t *b)
{
#define EQ(field) (a->field == b->field)
    return EQ(qx) && EQ(qy) && EQ(x) && EQ(y) && EQ(z) && EQ(radius)
        && EQ(height) && EQ(momz) && EQ(scale) && EQ(flags) && EQ(flags2)
        && EQ(eflags) && EQ(type) && EQ(player);
#undef EQ
}
static void Shutdown(void) { PC_Dump(stdout); }
UINT64 PC_Begin(mobj_t *thing, fixed_t x, fixed_t y, pccaller_t caller)
{
    pcframe_t *frame;
    pcslot_t *slot;
    if (!pcount_on || levelloading || gamestate != GS_LEVEL || !leveltime) return 0;
    if (!registered) { atexit(Shutdown); registered = true; }
    if (depth == PC_DEPTH) { Reject(R_NESTED); return 0; }
    if (depth) Reject(R_NESTED);
    frame = &frames[depth++];
    memset(frame, 0, sizeof *frame);
    frame->thing = thing;
    frame->caller = depth > 1 ? PC_OTHER : caller;
    countmap = gamemap;
    frame->input = (pcinput_t){0};
    frame->input.qx = x; frame->input.qy = y;
    if (!thing || P_MobjWasRemoved(thing) || thing->thinker.removing) Reject(R_REMOVED);
    else
    {
        pcinput_t *in = &frame->input;
        in->x = thing->x; in->y = thing->y; in->z = thing->z;
        in->radius = thing->radius; in->height = thing->height;
        in->momz = thing->momz; in->scale = thing->scale;
        in->flags = thing->flags; in->flags2 = thing->flags2;
        in->eflags = thing->eflags; in->angle = thing->angle;
        in->type = thing->type; in->player = thing->player;
        if (thing->player) Reject(R_PLAYER);
        if (thing->flags & MF_PAPERCOLLISION) Reject(R_PAPER);
        if (LUA_PerfCollisionHooks(thing->type)) Reject(R_HOOK);
    }
    if (depth > 1) Reject(R_NESTED);
    slot = Slot(thing, false);
    frame->previous = slot ? slot->record : NULL;
    if (!frame->reject && frame->previous && frame->previous->valid)
    {
        if (!SameInput(&frame->input, &frame->previous->input))
            stats[frame->caller].reject[R_INPUT]++;
        else if (!SameDependencies(frame->previous, frame))
            stats[frame->caller].reject[R_DEP]++;
        else frame->potential = true;
    }
    else stats[frame->caller].reject[R_COLD]++;
    return ((UINT64)epoch << 32) | depth;
}
static pcslope_t *SlopeState(pslope_t *slope)
{
    size_t index = ((uintptr_t)slope >> 4) * 2654435761u & (PC_SLOPES - 1);
    size_t i;
    for (i = 0; i < PC_SLOPES; ++i, index = (index + 1) & (PC_SLOPES - 1))
    {
        pcslope_t *state = &slope_states[index];
        if (state->slope == slope) return state;
        if (!state->slope) { state->slope = slope; return state; }
    }
    slope_state_failed = true;
    return NULL;
}
void PC_ThinkerAdded(thinker_t *thinker)
{
    pslope_t *slope = NULL;
    pcslope_t *state;
    if (!pcount_on) return;
    if (thinker->function == (actionf_p1)T_DynamicSlopeLine)
        slope = ((dynlineplanethink_t *)thinker)->slope;
    else if (thinker->function == (actionf_p1)T_DynamicSlopeVert)
        slope = ((dynvertexplanethink_t *)thinker)->slope;
    if (slope && (state = SlopeState(slope))) state->dynamic = true;
}
void PC_SlopeLuaWrite(pslope_t *slope)
{
    pcslope_t *state;
    if (pcount_on && slope && (state = SlopeState(slope))) state->lua = true;
}
static void Slope(pslope_t *slope)
{
    pcframe_t *frame = Current();
    pcslope_t *state;
    if (!slope || !frame || frame->reject) return;
    PC_DEP(slope_state_failed, PC_DEP_ROOT);
    if (slope_state_failed || !(state = SlopeState(slope))) { Reject(R_CAPACITY); return; }
    PC_DEP(state->dynamic, PC_DEP_ROOT); PC_DEP(state->lua, PC_DEP_ROOT);
    PC_DEP(slope->flags, PC_DEP_SLOPE);
    if (state->dynamic || (slope->flags & SL_DYNAMIC))
    {
        frame->dynamic_slope = true; Reject(R_SLOPE); return;
    }
    if (state->lua) { frame->lua_slope = true; Reject(R_SLOPE); return; }
    frame->static_slope = true;
    PC_DEP(slope->o.x, PC_DEP_SLOPE); PC_DEP(slope->o.y, PC_DEP_SLOPE); PC_DEP(slope->o.z, PC_DEP_SLOPE);
    PC_DEP(slope->d.x, PC_DEP_SLOPE); PC_DEP(slope->d.y, PC_DEP_SLOPE);
    PC_DEP(slope->zdelta, PC_DEP_SLOPE);
}
static void SectorGeometry(sector_t *sector)
{
    size_t i;
    pcframe_t *frame = Current();
    if (!frame || frame->reject) return;
    PC_DEP(sector->linecount, PC_DEP_ROOT); PC_DEP(sector->lines, PC_DEP_ROOT);
    for (i = 0; i < sector->linecount && !frame->reject; ++i)
    {
        line_t *line = sector->lines[i];
        PC_DEP(sector->lines[i], PC_DEP_ROOT);
        PC_DEP(line->frontsector, PC_DEP_ROOT); PC_DEP(line->backsector, PC_DEP_ROOT);
        PC_DEP(line->v1, PC_DEP_ROOT); PC_DEP(line->v2, PC_DEP_ROOT);
        PC_DEP(line->v1->x, PC_DEP_SLOPE); PC_DEP(line->v1->y, PC_DEP_SLOPE);
        PC_DEP(line->v2->x, PC_DEP_SLOPE); PC_DEP(line->v2->y, PC_DEP_SLOPE);
        PC_DEP(line->dx, PC_DEP_SLOPE); PC_DEP(line->dy, PC_DEP_SLOPE);
    }
}
static void FlatSector(sector_t *sector)
{
    if (!sector) return;
    PC_DEP(sector->floorheight, PC_DEP_SECTOR_HEIGHT); PC_DEP(sector->ceilingheight, PC_DEP_SECTOR_HEIGHT);
    PC_DEP(sector->f_slope, PC_DEP_ROOT); PC_DEP(sector->c_slope, PC_DEP_ROOT);
    Slope(sector->f_slope); Slope(sector->c_slope);
    if (sector->f_slope || sector->c_slope) SectorGeometry(sector);
}
void PC_Sector(sector_t *sector)
{
    ffloor_t *rover;
    pcframe_t *frame = Current();
    if (!frame || frame->reject || !sector) return;
    FlatSector(sector);
    PC_DEP(sector->ffloors, PC_DEP_ROOT);
    for (rover = sector->ffloors; rover && !frame->reject; rover = rover->next)
    {
        PC_DEP(rover->next, PC_DEP_ROOT); PC_INPUT(rover->fofflags); PC_INPUT(rover->secnum);
        PC_DEP(rover->topheight, PC_DEP_ROOT); PC_DEP(rover->bottomheight, PC_DEP_ROOT);
        PC_DEP(rover->t_slope, PC_DEP_ROOT); PC_DEP(rover->b_slope, PC_DEP_ROOT); PC_DEP(rover->master, PC_DEP_ROOT);
        if (rover->topheight) PC_DEP(*rover->topheight, PC_DEP_FOF_HEIGHT);
        if (rover->bottomheight) PC_DEP(*rover->bottomheight, PC_DEP_FOF_HEIGHT);
        if (rover->t_slope) { PC_DEP(*rover->t_slope, PC_DEP_ROOT); Slope(*rover->t_slope); }
        if (rover->b_slope) { PC_DEP(*rover->b_slope, PC_DEP_ROOT); Slope(*rover->b_slope); }
        if (rover->master) PC_DEP(rover->master->flags, PC_DEP_LINE_FLAGS);
        if ((size_t)rover->secnum >= (size_t)numsectors) { Reject(R_CAPACITY); return; }
        FlatSector(&sectors[rover->secnum]);
        if ((rover->t_slope && *rover->t_slope) || (rover->b_slope && *rover->b_slope)
            || sectors[rover->secnum].f_slope || sectors[rover->secnum].c_slope)
            SectorGeometry(sector);
        if (!(frame->input.flags & MF_NOGRAVITY)
            && (rover->fofflags & (FOF_EXISTS|FOF_SWIMMABLE|FOF_GOOWATER)) == (FOF_EXISTS|FOF_SWIMMABLE|FOF_GOOWATER))
            Reject(R_GOO);
    }
}
void PC_Line(line_t *line)
{
    pcframe_t *frame = Current();
    if (!frame || frame->reject) return;
    PC_DEP(line->flags, PC_DEP_LINE_FLAGS); PC_INPUT(line->special);
    PC_DEP(line->frontsector, PC_DEP_ROOT); PC_DEP(line->backsector, PC_DEP_ROOT); PC_DEP(line->polyobj, PC_DEP_ROOT);
    PC_INPUT(line->sidenum[0]); PC_INPUT(line->sidenum[1]);
    if (line->polyobj) { Reject(R_POLY); return; }
    if (line->flags & ML_MIDSOLID) { Reject(R_MIDTEX); return; }
    PC_Sector(line->frontsector); PC_Sector(line->backsector);
}
void PC_Thing(mobj_t *thing)
{
    pcframe_t *frame = Current();
    fixed_t distance;
    if (!frame || frame->reject || thing == frame->thing) return;
    if (P_MobjWasRemoved(thing)) { Reject(R_REMOVED); return; }
    distance = thing->radius + frame->thing->radius;
    if (abs(thing->x - frame->input.qx) >= distance || abs(thing->y - frame->input.qy) >= distance)
    {
        if (thing->flags & MF_PAPERCOLLISION)
        {
            stats[frame->caller].paper_far_visits++;
            frame->paper_far = true;
        }
        return;
    }
    if (thing->flags & MF_PAPERCOLLISION) stats[frame->caller].paper_inside++;
    Reject(R_XY);
}
static boolean Generations(void)
{
    size_t size;
    if (generation_failed) return false;
    if (generations) return true;
    if (bmapwidth <= 0 || bmapheight <= 0 || (size_t)bmapwidth > 4194304u / (size_t)bmapheight)
        return false;
    size = (size_t)bmapwidth * (size_t)bmapheight;
    generations = calloc(size, sizeof *generations);
    cellmeta = calloc(size, sizeof *cellmeta);
    if (!generations || !cellmeta)
    {
        free(generations); free(cellmeta); generations = NULL; cellmeta = NULL;
        generation_failed = true; return false;
    }
    ncells = size;
    return true;
}
void PC_Cells(INT32 xl, INT32 xh, INT32 yl, INT32 yh)
{
    INT32 x, y;
    pcframe_t *frame = Current();
    if (!frame || frame->reject) return;
    if (!Generations()) { Reject(R_CAPACITY); return; }
    for (y = max(0, yl); y <= min(bmapheight - 1, yh) && !frame->reject; ++y)
        for (x = max(0, xl); x <= min(bmapwidth - 1, xh) && !frame->reject; ++x)
        {
            size_t cell = (size_t)y * bmapwidth + x;
            size_t before = frame->ndeps;
            PC_DEP(generations[cell], PC_DEP_CELL);
            if (frame->ndeps > before) frame->deps[frame->ndeps - 1].inplace = cellmeta[cell].inplace;
            PC_DEP(polyblocklinks[cell], PC_DEP_ROOT);
            if (polyblocklinks[cell]) Reject(R_POLY);
        }
}
void PC_CellMutationFrom(INT32 cell, mobj_t *thing, pcmutation_t kind)
{
    size_t i;
    if (!pcount_on || !Generations() || cell < 0 || (size_t)cell >= ncells) return;
    cellmeta[cell].last_thing = thing;
    cellmeta[cell].last_kind = kind;
    if (++generations[cell]) return;
    for (i = 0; i < PC_SLOTS; ++i)
        if (slots[i].record) slots[i].record->valid = false;
    generations[cell] = 1;
}
void PC_CellMutation(INT32 cell) { PC_CellMutationFrom(cell, NULL, PC_MUT_OTHER); }
void PC_UnlinkBegin(mobj_t *thing)
{
    pcslot_t *slot;
    blocknode_t *node;
    size_t count = 0;
    if (!pcount_on || !thing || thing->thinker.removing || !thing->blocknode) return;
    slot = Slot(thing, true);
    if (!slot) return;
    slot->pending = false;
    for (node = thing->blocknode; node; node = node->bnext) count++;
    if (count > PC_DEPS) return;
    if (slot->unlink_capacity < count)
    {
        INT32 *next = realloc(slot->unlink_cells, count * sizeof *next);
        if (!next) return;
        slot->unlink_cells = next; slot->unlink_capacity = count;
    }
    slot->nunlink = count;
    for (node = thing->blocknode, count = 0; node; node = node->bnext)
        slot->unlink_cells[count++] = node->blockindex;
    slot->unlink_x = thing->x; slot->unlink_y = thing->y; slot->unlink_radius = thing->radius;
    slot->pending = true;
}
void PC_LinkEnd(mobj_t *thing)
{
    pcslot_t *slot;
    blocknode_t *node;
    size_t i = 0;
    if (!pcount_on || !thing || !(slot = Slot(thing, false)) || !slot->pending) return;
    slot->pending = false;
    if (!Generations() || thing->x != slot->unlink_x || thing->y != slot->unlink_y
        || thing->radius != slot->unlink_radius) return;
    for (node = thing->blocknode; node; node = node->bnext, ++i)
        if (i == slot->nunlink || node->blockindex != slot->unlink_cells[i]) return;
    if (i != slot->nunlink) return;
    for (i = 0; i < slot->nunlink; ++i)
        if (slot->unlink_cells[i] >= 0 && (size_t)slot->unlink_cells[i] < ncells)
            cellmeta[slot->unlink_cells[i]].inplace++;
}
fixed_t PC_Write(mobj_t *thing, fixed_t *field, fixed_t value)
{
    if (pcount_on && *field != value && !P_MobjWasRemoved(thing) && !thing->thinker.removing)
    {
        blocknode_t *node;
        for (node = thing->blocknode; node; node = node->bnext)
            PC_CellMutationFrom(node->blockindex, thing, PC_MUT_WRITE);
    }
    *field = value;
    return value;
}
void PC_Forget(mobj_t *thing)
{
    pcslot_t *slot;
    if (!pcount_on) return;
    slot = Slot(thing, false);
    if (slot)
    {
        if (slot->record) slot->record->valid = false;
        slot->pending = false;
    }
}
void PC_End(UINT64 session, boolean result, const UINT64 *outputs, size_t noutputs)
{
    pcframe_t *frame;
    pcstats_t *row;
    pcslot_t *slot;
    pcrecord_t *record;
    size_t i;
    if (!session || (UINT32)(session >> 32) != epoch || (UINT32)session != depth) return;
    frame = Current(); row = &stats[frame->caller]; row->calls++;
    if (frame->static_slope) row->static_slope_queries++;
    if (frame->dynamic_slope) row->dynamic_slope++;
    if (frame->lua_slope) row->lua_slope++;
    if (frame->paper_far) row->paper_far_queries++;
    if (noutputs > PC_OUTPUTS) Reject(R_CAPACITY);
    /* A real memo decides before walking. Do not hide an invalid generation
       or dependency proof by filtering its proposed hit using the full walk. */
    if (frame->potential && frame->reject)
    {
        Reject(R_MISMATCH);
#ifdef SRB2_PERF_VERIFY
        I_Error("Count stale candidate: MAP%02d tic %u caller %s rejection mask 0x%x",
            gamemap, (unsigned)leveltime, callers[frame->caller], frame->reject);
#endif
    }
    if (!frame->reject && frame->potential)
    {
        record = frame->previous;
        if (record->answer != result || record->noutputs != noutputs || memcmp(record->outputs, outputs, noutputs * sizeof *outputs))
        {
            Reject(R_MISMATCH);
#ifdef SRB2_PERF_VERIFY
            if (record->answer != result || record->noutputs != noutputs)
                I_Error("Count shadow mismatch: MAP%02d tic %u caller %s return/count cached %u/%u original %u/%u",
                    gamemap, (unsigned)leveltime, callers[frame->caller], (unsigned)record->answer,
                    (unsigned)record->noutputs, (unsigned)result, (unsigned)noutputs);
            for (i = 0; i < noutputs; ++i)
                if (record->outputs[i] != outputs[i])
                    I_Error("Count shadow mismatch: MAP%02d tic %u caller %s output %u cached %llu original %llu",
                        gamemap, (unsigned)leveltime, callers[frame->caller], (unsigned)i,
                        (unsigned long long)record->outputs[i], (unsigned long long)outputs[i]);
#endif
        }
        else
        {
            row->hits++;
            if (frame->static_slope) row->static_slope_hits++;
            if (frame->paper_far) row->paper_far_hits++;
        }
    }
    if (!frame->reject)
    {
        slot = Slot(frame->thing, true);
        if (!slot) Reject(R_CAPACITY);
        else
        {
            if (!slot->record) slot->record = calloc(1, sizeof *slot->record);
            record = slot->record;
            if (!record) Reject(R_CAPACITY);
            else
            {
                record->valid = false;
                if (record->capacity < frame->ndeps)
                {
                    pcdep_t *next = realloc(record->deps, frame->ndeps * sizeof *next);
                    if (!next) Reject(R_CAPACITY);
                    else { record->deps = next; record->capacity = frame->ndeps; }
                }
                if (!frame->reject)
                {
                    record->input = frame->input;
                    record->answer = result;
                    record->ndeps = frame->ndeps;
                    record->noutputs = noutputs;
                    memcpy(record->deps, frame->deps, frame->ndeps * sizeof *frame->deps);
                    memcpy(record->outputs, outputs, noutputs * sizeof *outputs);
                    record->valid = true;
                    row->eligible++;
                }
            }
        }
    }
    if (frame->reject && frame->previous) frame->previous->valid = false;
    for (i = 0; i < R_COUNT; ++i)
        if (frame->reject & (1u << i)) row->reject[i]++;
    depth--;
}
void PC_Dump(FILE *out)
{
    static const char *const dependencies[PC_DEP_KINDS] = {"other", "cell", "sector_height",
        "fof_height", "line_flags", "root", "slope_definition"};
    size_t i, j;
    if (!pcount_on || !countmap) return;
    if (!out) out = stdout;
    for (i = 0; i < PC_CALLERS; ++i)
    {
        const pcstats_t *row = &stats[i];
        fprintf(out, "PERFCOUNT|map=MAP%02d|caller=%s|calls=%llu|eligible=%llu|hits=%llu",
            countmap, callers[i], (unsigned long long)row->calls,
            (unsigned long long)row->eligible, (unsigned long long)row->hits);
        for (j = 0; j < R_COUNT; ++j)
            fprintf(out, "|%s=%llu", reasons[j], (unsigned long long)row->reject[j]);
        for (j = 0; j < PC_DEP_KINDS; ++j)
            fprintf(out, "|dep_%s=%llu", dependencies[j], (unsigned long long)row->dep[j]);
        for (j = 0; j <= PC_NAMES; ++j)
            fprintf(out, "|dep_input_%s=%llu", j == PC_NAMES ? "unnamed" : named_inputs[j],
                (unsigned long long)row->named_dep[j]);
        fprintf(out, "|paper_query_rejected=%llu", (unsigned long long)row->reject[R_PAPER]);
#define EXTRA(field) fprintf(out, "|" #field "=%llu", (unsigned long long)row->field)
        EXTRA(static_slope_queries); EXTRA(static_slope_hits); EXTRA(dynamic_slope); EXTRA(lua_slope);
        EXTRA(paper_far_visits); EXTRA(paper_far_queries); EXTRA(paper_far_hits); EXTRA(paper_inside);
        EXTRA(cell_last_link); EXTRA(cell_last_unlink); EXTRA(cell_last_write); EXTRA(cell_last_other);
        EXTRA(cell_last_neighbor); EXTRA(cell_inplace_relink);
#undef EXTRA
        fputc('\n', out);
    }
    fflush(out);
}
void PC_Reset(void)
{
    size_t i;
    if (!pcount_on) return;
    PC_Dump(report_output ? report_output : stdout);
    for (i = 0; i < PC_SLOTS; ++i)
    {
        if (slots[i].record) { free(slots[i].record->deps); free(slots[i].record); }
        free(slots[i].unlink_cells);
    }
    memset(slots, 0, sizeof slots);
    memset(stats, 0, sizeof stats);
    free(generations); generations = NULL; ncells = 0;
    free(cellmeta); cellmeta = NULL;
    memset(slope_states, 0, sizeof slope_states); slope_state_failed = false;
    generation_failed = false; countmap = 0; depth = 0; epoch++;
}
