#include "tactics.h"

#include <math.h>
#include <string.h>

#include <melee/ft/inlines.h>
#include <melee/ft/kinds/ftCommon/forward.h>
#include <melee/ft/types.h>
#include <melee/gm/gm_1A3F.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <pc/pc.h>

/* Controller macros only: no forced action states, hitboxes or damage.
 * Final Destination ledges sit near |x| = 88; past this the fighter is
 * offstage and the universal recovery runs before the next planning break. */
#define FD_EDGE 77.0f
#define FD_FLOOR -8.0f

typedef struct Brain {
    int slot, phase, age, cooldown, frames;
    bool executing, saw_attack;
} Brain;

static TacticsLoadout loadouts[2];
static Brain brains[2];
static bool active[2];

void tactics_SetLoadout(int p, const TacticsLoadout* l)
{
    int i;

    if (p < 0 || p >= 2 || l == NULL || l->ckind < 0 || l->count > TACTICS_SLOTS) {
        return;
    }
    for (i = 0; i < l->count; i++) {
        if (l->moves[i] == TM_NONE) {
            continue;
        }
        if (!tactics_MoveAllowed(l->ckind, l->moves[i])) {
            return;
        }
    }
    loadouts[p] = *l;
    active[p] = true;
}

void tactics_ClearLoadouts(void)
{
    memset(loadouts, 0, sizeof(loadouts));
    active[0] = active[1] = false;
}

void tactics_BeginMatch(void)
{
    memset(brains, 0, sizeof(brains));
}

void tactics_RestartQueues(void)
{
    int p;

    for (p = 0; p < 2; p++) {
        brains[p].slot = 0;
        brains[p].phase = 0;
        brains[p].age = 0;
        brains[p].cooldown = 2;
        brains[p].executing = false;
        brains[p].saw_attack = false;
    }
}

bool tactics_Controls(Fighter* fp)
{
    return gm_GetCurrentGameMode() == GM_TACTICS && fp->player_id < 2 &&
           !fp->is_sub_fighter && active[fp->player_id];
}

static bool inRange(int s, int lo, int hi)
{
    return s >= lo && s <= hi;
}

static bool actionable(Fighter* f)
{
    int s = f->motion_id;

    return inRange(s, ftCo_MS_Wait, ftCo_MS_RunBrake) ||
           inRange(s, ftCo_MS_JumpF, ftCo_MS_FallAerialB) ||
           s == ftCo_MS_SquatWait || s == ftCo_MS_OttottoWait;
}

/* Still inside an attack, a grab, or hitstun, including the endlag and the
 * tumble that follows a hit. A combo is not a break. */
static bool exchangeBusy(Fighter* f)
{
    int s = f->motion_id;

    if (f->x221C_b6 || f->dmg.x195c_hitlag_frames > 0.0f || f->victim_gobj != NULL) {
        return true;
    }
    if (s <= ftCo_MS_RebirthWait) {
        return true;
    }
    if (inRange(s, ftCo_MS_Attack11, ftCo_MS_LandingAirLw) ||
        inRange(s, ftCo_MS_DamageHi1, ftCo_MS_DamageFlyRoll) ||
        s == ftCo_MS_DamageFall ||
        inRange(s, ftCo_MS_Catch, ftCo_MS_ThrowLw) ||
        inRange(s, ftCo_MS_CapturePulledHi, ftCo_MS_CaptureFoot) ||
        inRange(s, ftCo_MS_ThrownF, ftCo_MS_ThrownlwWomen) ||
        inRange(s, ftCo_MS_EscapeF, ftCo_MS_EscapeAir) ||
        inRange(s, ftCo_MS_ReboundStop, ftCo_MS_Rebound) ||
        inRange(s, ftCo_MS_FlyReflectWall, ftCo_MS_StopCeil) ||
        s == ftCo_MS_CliffCatch ||
        inRange(s, ftCo_MS_CliffClimbSlow, ftCo_MS_CliffJumpQuick2) ||
        inRange(s, ftCo_MS_Passive, ftCo_MS_PassiveCeil) ||
        inRange(s, ftCo_MS_ShieldBreakFly, ftCo_MS_ShieldBreakStandD) ||
        s == ftCo_MS_Furafura || s == ftCo_MS_MissFoot ||
        (inRange(s, ftCo_MS_DownBoundU, ftCo_MS_DownSpotD) &&
         s != ftCo_MS_DownWaitU && s != ftCo_MS_DownWaitD) ||
        inRange(s, ftCo_MS_ThrownFF, ftCo_MS_Count - 1) || s >= ftCo_MS_Count)
    {
        return true;
    }
    /* Ledge and knockdown are stable. Anywhere else past the FD edge is
     * still the recovery, so the jump and up-B finish before planning. */
    if (s == ftCo_MS_CliffWait || s == ftCo_MS_DownWaitU || s == ftCo_MS_DownWaitD) {
        return false;
    }
    if (fabsf(f->cur_pos.x) > FD_EDGE || f->cur_pos.y < FD_FLOOR) {
        return true;
    }
    return false;
}

static int nextSlot(int port)
{
    int slot = brains[port].slot;

    while (slot < loadouts[port].count && loadouts[port].moves[slot] == TM_NONE) {
        slot++;
    }
    return slot;
}

static bool queueDone(int port)
{
    return !brains[port].executing && nextSlot(port) >= loadouts[port].count;
}

bool tactics_BreakInAction(void)
{
    HSD_GObj* g;
    int seen = 0;

    for (g = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_FIGHTER]; g != NULL; g = g->next) {
        Fighter* f = GET_FIGHTER(g);

        if (f->player_id >= 2 || f->is_sub_fighter) {
            continue;
        }
        if (!active[f->player_id]) {
            return false;
        }
        seen++;
        if (exchangeBusy(f) || !queueDone(f->player_id)) {
            return false;
        }
    }
    return seen >= 2;
}

static void pad(Fighter* f, int x, int y, unsigned buttons, int cx, int cy)
{
    f->cpu.lstick.x = x;
    f->cpu.lstick.y = y;
    f->cpu.cstick.x = cx;
    f->cpu.cstick.y = cy;
    f->cpu.buttons = buttons;
    f->cpu.ltrigger = f->cpu.rtrigger = 0;
}

static void beginMove(Fighter* f, Brain* b, int dir, int move, const TacticsMoveInfo* info,
                       bool air)
{
    b->executing = true;
    b->age = 0;
    b->phase = 1;
    b->saw_attack = false;
    switch (info->input) {
    case TI_AERIAL:
        if (!air) {
            pad(f, 0, 0, HSD_PAD_X, 0, 0);
            b->phase = 0;
        } else {
            pad(f,
                move == TM_FAIR ? (int) f->facing_dir * 127
                                : move == TM_BAIR ? -(int) f->facing_dir * 127 : 0,
                move == TM_UAIR ? 127 : move == TM_DAIR ? -127 : 0, HSD_PAD_A, 0, 0);
        }
        break;
    case TI_DASH:
        pad(f, dir * 127, 0, 0, 0, 0);
        b->phase = 0;
        break;
    case TI_SMASH:
        pad(f, 0, 0, 0, move == TM_FSMASH ? dir * 127 : 0,
            move == TM_USMASH ? 127 : move == TM_DSMASH ? -127 : 0);
        break;
    case TI_SPECIAL:
        pad(f, move == TM_SIDE_B ? dir * 127 : 0,
            move == TM_UP_B ? 127 : move == TM_DOWN_B ? -127 : 0, HSD_PAD_B, 0, 0);
        break;
    case TI_THROW:
        pad(f, 0, 0, HSD_PAD_Z, 0, 0);
        break;
    default:
        pad(f, move == TM_FTILT ? dir * 45 : 0,
            move == TM_UTILT ? 45 : move == TM_DTILT ? -60 : 0, HSD_PAD_A, 0, 0);
        break;
    }
}

void tactics_Think(Fighter_GObj* gobj)
{
    Fighter* f = GET_FIGHTER(gobj);
    Brain* b = &brains[f->player_id];
    TacticsLoadout* l = &loadouts[f->player_id];
    Fighter* enemy = NULL;
    HSD_GObj* g;
    float dx;
    int dir, move;
    bool air;
    const TacticsMoveInfo* info;

    for (g = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_FIGHTER]; g != NULL; g = g->next) {
        Fighter* other = GET_FIGHTER(g);
        if (other->player_id != f->player_id && !other->is_sub_fighter) {
            enemy = other;
            break;
        }
    }
    pad(f, 0, 0, 0, 0, 0);
    if (enemy == NULL) {
        return;
    }
    b->frames++;
    dx = enemy->cur_pos.x - f->cur_pos.x;
    dir = dx >= 0 ? 1 : -1;
    air = f->ground_or_air == GA_Air;
    if (b->frames % 600 == 0) {
        pc_log_line("tactics: P%d frame=%d state=%d route=%d percent=%.1f x=%.1f",
                    f->player_id + 1, b->frames, f->motion_id, b->slot,
                    f->dmg.x1830_percent, f->cur_pos.x);
    }
    /* Recovery is not part of the queue. It is available in every exchange. */
    if (f->motion_id == ftCo_MS_CliffWait) {
        pad(f, 0, 0, HSD_PAD_X, 0, 0);
        return;
    }
    if (f->motion_id == ftCo_MS_DownWaitU || f->motion_id == ftCo_MS_DownWaitD) {
        pad(f, dir * 80, 0, 0, 0, 0);
        return;
    }
    if (air && (fabsf(f->cur_pos.x) > FD_EDGE || f->cur_pos.y < FD_FLOOR)) {
        int inward = f->cur_pos.x > 0 ? -1 : 1;
        pad(f, inward * 100, 0, 0, 0, 0);
        if (actionable(f) && b->frames % 12 == 0) {
            if (f->x1968_jumpsUsed < f->co_attrs.max_jumps) {
                f->cpu.buttons = HSD_PAD_X;
            } else {
                f->cpu.buttons = HSD_PAD_B;
                f->cpu.lstick.y = 127;
            }
        }
        return;
    }

    while (b->slot < l->count && l->moves[b->slot] == TM_NONE) {
        b->slot++;
    }
    if (!b->executing && b->slot >= l->count) {
        return;
    }

    move = l->moves[b->slot];
    info = tactics_GetMove(l->ckind, move);
    if (info == NULL) {
        b->executing = false;
        b->slot++;
        return;
    }
    if (b->executing) {
        b->age++;
        if ((info->input == TI_AERIAL && air && f->motion_id >= ftCo_MS_AttackAirN) ||
            (info->input != TI_AERIAL &&
             (inRange(f->motion_id, ftCo_MS_Attack11, ftCo_MS_AttackAirLw) ||
              inRange(f->motion_id, ftCo_MS_Catch, ftCo_MS_ThrowLw) ||
              f->motion_id >= ftCo_MS_Count)))
        {
            b->saw_attack = true;
        }
        if ((b->saw_attack && actionable(f)) || b->age > 150) {
            b->executing = false;
            b->slot++;
            b->cooldown = 3;
            return;
        }
        if (info->input == TI_THROW && f->motion_id == ftCo_MS_CatchWait) {
            pad(f,
                move == TM_FTHROW ? (int) f->facing_dir * 127
                                  : move == TM_BTHROW ? -(int) f->facing_dir * 127 : 0,
                move == TM_UTHROW ? 127 : move == TM_DTHROW ? -127 : 0, 0, 0, 0);
            return;
        }
        if (info->input == TI_AERIAL && b->phase == 0) {
            if (!air) {
                return;
            }
            b->phase = 1;
            pad(f,
                move == TM_FAIR ? (int) f->facing_dir * 127
                                : move == TM_BAIR ? -(int) f->facing_dir * 127 : 0,
                move == TM_UAIR ? 127 : move == TM_DAIR ? -127 : 0, HSD_PAD_A, 0, 0);
            return;
        }
        if (info->input == TI_DASH && b->phase == 0) {
            if (b->age < 3) {
                pad(f, dir * 127, 0, 0, 0, 0);
                return;
            }
            pad(f, dir * 127, 0, HSD_PAD_A, 0, 0);
            b->phase = 1;
            return;
        }
        if (air) {
            f->cpu.lstick.x = dir * 40;
        }
        return;
    }
    if (b->cooldown > 0) {
        b->cooldown--;
        return;
    }
    if (!actionable(f)) {
        return;
    }
    if (air && info->air.x0 > info->air.x1) {
        f->cpu.lstick.x = dir * 50;
        return;
    }
    {
        float reach = air ? info->air.x1 : info->ground.x1;
        int face;

        if (fabsf(dx) > reach) {
            pad(f, dir * 65, 0, 0, 0, 0);
            return;
        }
        face = info->facing == TF_BACK ? -dir : dir;
        if (!air && (info->facing == TF_FRONT || info->facing == TF_BACK) &&
            f->facing_dir * face < 0)
        {
            pad(f, face * 50, 0, 0, 0, 0);
            return;
        }
    }
    beginMove(f, b, dir, move, info, air);
}
