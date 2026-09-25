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
    int slot, phase, age, cooldown, frames, approach;
    bool executing, saw_attack;
} Brain;

/* With nothing queued, a fighter runs in. The break comes as the gap
 * closes past MEET_TRIGGER, still running; a fighter already inside
 * MEET_STOP stands and waits. The auto run stays this far inside the
 * edges. */
#define MEET_TRIGGER 34.0f
#define MEET_STOP 22.0f
#define AUTO_EDGE (FD_EDGE - 12.0f)
/* A launched fighter is chased, and the break comes when the chaser is
 * this close. */
#define CHASE_MEET 30.0f
/* The chaser jumps only for a target this far overhead at most. */
#define CHASE_JUMP 50.0f
/* Frames a queued move spends closing to its reach before it goes anyway. */
#define APPROACH_LIMIT 60

static TacticsLoadout loadouts[2];
static Brain brains[2];
static bool active[2];
static bool chase_seen[2];

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
    memset(chase_seen, 0, sizeof(chase_seen));
}

void tactics_RestartQueues(void)
{
    int p;

    for (p = 0; p < 2; p++) {
        brains[p].slot = 0;
        brains[p].phase = 0;
        brains[p].age = 0;
        brains[p].cooldown = 2;
        brains[p].approach = 0;
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

static bool tumbling(Fighter* f)
{
    int s = f->motion_id;

    return f->ground_or_air == GA_Air &&
           (inRange(s, ftCo_MS_DamageFlyHi, ftCo_MS_DamageFlyRoll) || s == ftCo_MS_DamageFall);
}

static bool actionable(Fighter* f)
{
    int s = f->motion_id;

    /* Once hitstun ends, a launched fighter can act out of tumble. */
    if (tumbling(f) && !f->x221C_b6 && f->dmg.x195c_hitlag_frames <= 0.0f) {
        return true;
    }
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

static bool fighterPair(Fighter** fs)
{
    HSD_GObj* g;

    fs[0] = fs[1] = NULL;
    for (g = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_FIGHTER]; g != NULL; g = g->next) {
        Fighter* f = GET_FIGHTER(g);

        if (f->player_id < 2 && !f->is_sub_fighter) {
            fs[f->player_id] = f;
        }
    }
    return fs[0] != NULL && fs[1] != NULL && active[0] && active[1];
}

bool tactics_BreakInAction(void)
{
    Fighter* fs[2];

    if (!fighterPair(fs)) {
        return false;
    }
    if (exchangeBusy(fs[0]) || !queueDone(0) || exchangeBusy(fs[1]) || !queueDone(1)) {
        return false;
    }
    {
        /* Someone high overhead has to come down before they meet. */
        float dx = fs[0]->cur_pos.x - fs[1]->cur_pos.x;
        float dy = fs[0]->cur_pos.y - fs[1]->cur_pos.y;

        return dx * dx + dy * dy <= MEET_TRIGGER * MEET_TRIGGER;
    }
}

static bool onStage(Fighter* f)
{
    return fabsf(f->cur_pos.x) <= FD_EDGE && f->cur_pos.y >= FD_FLOOR;
}

int tactics_ChaseMeet(void)
{
    Fighter* fs[2];
    int hit = -1;
    int p;

    if (!fighterPair(fs)) {
        return -1;
    }
    for (p = 0; p < 2; p++) {
        Fighter* v = fs[p];
        Fighter* a = fs[p ^ 1];
        float dx, dy;

        /* A fresh hit starts a fresh launch. */
        if (!tumbling(v) || v->dmg.x195c_hitlag_frames > 0.0f) {
            chase_seen[p] = false;
            continue;
        }
        if (chase_seen[p] || hit >= 0) {
            continue;
        }
        /* Offstage the recovery is automatic, and a trade has no one free to
         * follow up. */
        if (!onStage(v) || !onStage(a) || tumbling(a) || a->victim_gobj != NULL ||
            a->dmg.x195c_hitlag_frames > 0.0f || !queueDone(p ^ 1) || !actionable(a))
        {
            continue;
        }
        dx = v->cur_pos.x - a->cur_pos.x;
        dy = v->cur_pos.y - a->cur_pos.y;
        if (dx * dx + dy * dy <= CHASE_MEET * CHASE_MEET) {
            chase_seen[p] = true;
            hit = p;
        }
    }
    return hit;
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
    b->approach = 0;
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
    case TI_DODGE:
        pad(f, -dir * 127, 40, HSD_PAD_R, 0, 0);
        break;
    case TI_JUMP:
        pad(f, -dir * 127, 0, f->x1968_jumpsUsed < f->co_attrs.max_jumps ? HSD_PAD_X : 0, 0,
            0);
        break;
    case TI_DRIFT:
        pad(f, -dir * 127, 0, 0, 0, 0);
        break;
    default:
        pad(f, move == TM_FTILT ? dir * 45 : 0,
            move == TM_UTILT ? 45 : move == TM_DTILT ? -60 : 0, HSD_PAD_A, 0, 0);
        break;
    }
}

static bool running(Fighter* f)
{
    return inRange(f->motion_id, ftCo_MS_TurnRun, ftCo_MS_RunBrake);
}

/* A run only gives way to a dash attack, a grab, a jump or side B. Anything
 * else crouches out of it first; every ground attack comes out of a crouch. */
static bool needsStop(int move, const TacticsMoveInfo* info)
{
    return info->input == TI_GROUND || info->input == TI_SMASH ||
           (info->input == TI_SPECIAL && move != TM_SIDE_B);
}

static float clampEdge(float x)
{
    return x > AUTO_EDGE ? AUTO_EDGE : x < -AUTO_EDGE ? -AUTO_EDGE : x;
}

/* Nothing queued: run in at a standing foe, or chase a launched one. The
 * mode breaks before either reaches the other. */
static void autoMove(Fighter* f, Fighter* enemy, Brain* b, bool air)
{
    float gx = clampEdge(enemy->cur_pos.x) - f->cur_pos.x;
    float dy = enemy->cur_pos.y - f->cur_pos.y;
    int gdir = gx >= 0 ? 1 : -1;

    if (f->motion_id == ftCo_MS_KneeBend && tumbling(enemy)) {
        f->cpu.buttons = HSD_PAD_X;
        return;
    }
    if (!actionable(f) || tumbling(f)) {
        return;
    }
    if (!tumbling(enemy)) {
        if (fabsf(enemy->cur_pos.x - f->cur_pos.x) > MEET_STOP && fabsf(gx) > 4.0f) {
            f->cpu.lstick.x = gdir * 127;
        }
        return;
    }
    if (air) {
        f->cpu.lstick.x = fabsf(gx) > 4.0f ? gdir * 127 : 0;
        /* Falling short of a target still overhead: spend the midair jump.
         * Pressing only every other frame gives the button a fresh press. */
        if (f->self_vel.y <= 0.0f && dy > 12.0f && dy < CHASE_JUMP &&
            f->x1968_jumpsUsed < f->co_attrs.max_jumps && (b->frames & 1))
        {
            f->cpu.buttons = HSD_PAD_X;
        }
        return;
    }
    if (fabsf(gx) > 12.0f) {
        f->cpu.lstick.x = gdir * 127;
    } else if (dy > 18.0f && dy < CHASE_JUMP) {
        /* Held through the jump squat, so it is a full hop. */
        f->cpu.buttons = HSD_PAD_X;
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
        autoMove(f, enemy, b, air);
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
        switch (info->input) {
        case TI_AERIAL:
            b->saw_attack |= air && f->motion_id >= ftCo_MS_AttackAirN;
            break;
        case TI_DODGE:
            b->saw_attack |= f->motion_id == ftCo_MS_EscapeAir;
            break;
        case TI_JUMP:
            /* Out of jumps, this is a drift. */
            b->saw_attack |= inRange(f->motion_id, ftCo_MS_JumpAerialF, ftCo_MS_JumpAerialB) ||
                             !air;
            break;
        case TI_DRIFT:
            b->saw_attack |= !air;
            break;
        default:
            b->saw_attack |= inRange(f->motion_id, ftCo_MS_Attack11, ftCo_MS_AttackAirLw) ||
                             inRange(f->motion_id, ftCo_MS_Catch, ftCo_MS_ThrowLw) ||
                             f->motion_id >= ftCo_MS_Count;
            break;
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
        if (info->input == TI_JUMP || info->input == TI_DRIFT) {
            f->cpu.lstick.x = -dir * 127;
            return;
        }
        if (info->input == TI_AERIAL && b->phase == 0) {
            float dy = enemy->cur_pos.y - f->cur_pos.y;

            /* A target far overhead gets a full hop, and the swing waits
             * until the rise brings it into reach. */
            if (!air) {
                if (dy > info->ground.y1) {
                    pad(f, 0, 0, HSD_PAD_X, 0, 0);
                }
                return;
            }
            if (dy > info->air.y1 && f->self_vel.y > 0.0f && b->age < 45) {
                f->cpu.lstick.x = dir * 60;
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
    /* A reaction was for the air. Landed before it could come out, it is
     * gone. */
    if (!air && info->ground.x0 > info->ground.x1) {
        b->slot++;
        return;
    }
    {
        float reach = air ? info->air.x1 : info->ground.x1;
        int face;

        /* Bodies can keep two fighters farther apart than a short reach.
         * After a second of closing in, swing anyway. */
        if (fabsf(dx) > reach && ++b->approach < APPROACH_LIMIT) {
            /* Keep a run going rather than dropping to a walk. */
            pad(f, dir * (running(f) ? 127 : 65), 0, 0, 0, 0);
            return;
        }
        if (!air && running(f) && needsStop(move, info)) {
            pad(f, 0, -127, 0, 0, 0);
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
