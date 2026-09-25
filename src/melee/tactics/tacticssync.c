#include "tactics.h"

#include <stdlib.h>

#include <melee/ft/inlines.h>
#include <melee/ft/kinds/ftCommon/ftCo_0A01.h>
#include <melee/ft/kinds/ftCommon/forward.h>
#include <melee/ft/types.h>
#include <pc/net.h>
#include <pc/pc.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/random.h>

/* Two machines play the same tactics match by running the same simulation
 * from the same seed. Between breaks every input comes from the machine
 * itself (Melee's CPU think and the pick player), so the only thing the
 * peers ever exchange is the picks. That holds only if nothing depends on the
 * local machine:
 *
 * - The fight's clock starts when both fighters are out of their entry. How
 *   many ticks the scene took to load before that differs between machines,
 *   and the CPU think has been counting them, so its state is rebuilt there.
 * - Every simulated tick starts from a seed that is a function of the match
 *   seed and the fight's tick alone, as the port's netplay does (fight_reseed
 *   in src/pc/net.c, after Slippi). A draw outside the simulation, such as a
 *   particle drawn at the display's rate, then heals on the next tick. Frozen
 *   ticks do not count: the two players take different times to pick.
 * - The CPU's picks come from a generator seeded the same way, and
 *   pc_net_deterministic() turns off retail behaviours seeded from the clock.
 *
 * A checksum of the fight at every break is how a desync gets caught. */

static bool armed, running;
static u32 match_seed, rng;
static s32 tick;
static int breaks;

void tactics_SyncBegin(u32 seed, bool on)
{
    armed = on;
    running = false;
    breaks = 0;
    tick = 0;
    match_seed = seed;
    pc_net_set_external_sync(on);
    if (on) {
        rng = seed != 0 ? seed ^ 0x9E3779B9u : 0x9E3779B9u;
        pc_log_line("tactics: sync seed=%08X", seed);
    }
}

void tactics_SyncEnd(void)
{
    armed = running = false;
    pc_net_set_external_sync(false);
}

bool tactics_Synced(void)
{
    return armed;
}

s32 tactics_SyncTick(void)
{
    return running ? tick : -1;
}

/* xorshift32: the same sequence on every machine. Offline it falls back to
 * rand(), which nothing else relies on. */
u32 tactics_SyncRand(void)
{
    if (!armed) {
        return (u32) rand();
    }
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static bool inEntry(Fighter* f)
{
    return f->motion_id <= ftCo_MS_RebirthWait ||
           (f->motion_id >= ftCo_MS_Entry && f->motion_id <= ftCo_MS_EntryEnd);
}

/* Murmur3's finalizer, as net.c's fight_reseed: consecutive ticks get
 * unrelated seeds. */
static u32 tickSeed(u32 seed, s32 t)
{
    u32 x = seed ^ ((u32) t * 0x9E3779B9u);

    x ^= x >> 16;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    x *= 0xC2B2AE35u;
    x ^= x >> 16;
    return x;
}

void tactics_SyncFrame(bool frozen)
{
    HSD_GObj* g;
    int ready = 0;

    if (!armed) {
        return;
    }
    if (!running) {
        for (g = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_FIGHTER]; g != NULL; g = g->next) {
            Fighter* f = GET_FIGHTER(g);

            if (f->player_id < 2 && !f->is_sub_fighter && !inEntry(f)) {
                ready++;
            }
        }
        if (ready < 2) {
            return;
        }
        running = true;
        tick = 0;
        *HSD_RandSeedPtr = tickSeed(match_seed, -1);
        /* The pick player's counters ran through the entry too. */
        tactics_BeginMatch();
        for (g = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_FIGHTER]; g != NULL; g = g->next) {
            Fighter* f = GET_FIGHTER(g);

            if (f->player_id < 2 && !f->is_sub_fighter) {
                ftCo_800A101C(f, f->cpu.kind, f->cpu.level, f->cpu.x14);
            }
        }
        pc_log_line("tactics: sync live");
    }
    if (!frozen) {
        *HSD_RandSeedPtr = tickSeed(match_seed, tick++);
    }
}

static u32 mix(u32 h, u32 v)
{
    h ^= v;
    h *= 16777619u;
    return h;
}

static u32 mixf(u32 h, float f)
{
    union {
        float f;
        u32 u;
    } bits;

    bits.f = f;
    return mix(h, bits.u);
}

/* The fighters and the fight's tick. The RNG is not in it: every tick
 * rederives it, and between ticks draws outside the simulation move it. */
u32 tactics_SyncChecksum(void)
{
    HSD_GObj* g;
    u32 h = mix(2166136261u, (u32) tick);

    for (g = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_FIGHTER]; g != NULL; g = g->next) {
        Fighter* f = GET_FIGHTER(g);

        h = mix(h, f->player_id | (f->is_sub_fighter << 8));
        h = mix(h, f->motion_id);
        h = mixf(h, f->cur_pos.x);
        h = mixf(h, f->cur_pos.y);
        h = mixf(h, f->self_vel.x);
        h = mixf(h, f->self_vel.y);
        h = mixf(h, f->x8c_kb_vel.x);
        h = mixf(h, f->x8c_kb_vel.y);
        h = mixf(h, f->facing_dir);
        h = mixf(h, f->dmg.x1830_percent);
    }
    return h;
}

int tactics_SyncBreak(void)
{
    if (!armed) {
        return 0;
    }
    pc_log_line("tactics: sync break=%d tick=%d sum=%08X", ++breaks, tick,
                tactics_SyncChecksum());
    return breaks;
}
