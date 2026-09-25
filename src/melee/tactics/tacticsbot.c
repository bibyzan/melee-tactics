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
    bool executing, saw_attack, connected, forced;
} Brain;

/* With nothing queued, Melee's own CPU moves the fighter. The break comes as
 * the two close within MEET_TRIGGER. */
#define MEET_TRIGGER 34.0f
/* A launched fighter is chased. The break comes when the two are predicted
 * to come within CHASE_REACH in the next CHASE_LEAD frames; a grounded
 * chaser's jump covers CHASE_HOP of the height. */
#define CHASE_LEAD 16
#define CHASE_REACH 24.0f
#define CHASE_HOP 22.0f
/* Frames an airborne aerial waits for the swing to line up. */
#define AIR_WAIT 30
/* Frames a queued move spends closing to its reach before it goes anyway. */
#define APPROACH_LIMIT 30

static TacticsLoadout loadouts[2];
static Brain brains[2];
static bool active[2];
static bool chase_seen[2];
static bool was_stunned[2];

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
    memset(was_stunned, 0, sizeof(was_stunned));
}

void tactics_RestartQueue(int p)
{
    Brain* b;

    if (p < 0 || p >= 2) {
        return;
    }
    b = &brains[p];
    b->slot = 0;
    b->phase = 0;
    b->age = 0;
    b->cooldown = 0;
    b->approach = 0;
    b->executing = false;
    b->saw_attack = false;
}

static bool tacticsFighter(Fighter* fp)
{
    return gm_GetCurrentGameMode() == GM_TACTICS && fp->player_id < 2 &&
           !fp->is_sub_fighter && active[fp->player_id];
}

static bool queueDone(int port);

bool tactics_Controls(Fighter* fp)
{
    return tacticsFighter(fp) && !queueDone(fp->player_id);
}

void tactics_FilterAi(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    bool recovering;

    if (!tacticsFighter(fp)) {
        return;
    }
    tactics_DumpAi(fp);
    if (++brains[fp->player_id].frames % 600 == 0) {
        pc_log_line("tactics: P%d cpu mode=%d state=%d percent=%.1f x=%.1f y=%.1f",
                    fp->player_id + 1, fp->cpu.x18, fp->motion_id, fp->dmg.x1830_percent,
                    fp->cur_pos.x, fp->cur_pos.y);
    }
    /* Every attack is the player's call. The CPU keeps its movement, jumps,
     * shield, dodges and tech; B stays only for getting back to the stage. */
    recovering = fp->ground_or_air == GA_Air &&
                 (fabsf(fp->cur_pos.x) > FD_EDGE || fp->cur_pos.y < FD_FLOOR);
    fp->cpu.buttons &= ~(HSD_PAD_A | HSD_PAD_Z | (recovering ? 0 : HSD_PAD_B));
    fp->cpu.cstick.x = 0;
    fp->cpu.cstick.y = 0;
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
    /* Standing or teetering near the ledge is still on the stage; only the
     * air out there is the recovery. */
    return f->ground_or_air == GA_Air &&
           (fabsf(f->cur_pos.x) > FD_EDGE || f->cur_pos.y < FD_FLOOR);
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
    return f->ground_or_air == GA_Ground ||
           (fabsf(f->cur_pos.x) <= FD_EDGE && f->cur_pos.y >= FD_FLOOR);
}

static bool still(Fighter* f)
{
    return fabsf(f->pos_delta.x) + fabsf(f->pos_delta.y) < 0.05f;
}

bool tactics_BothIdle(void)
{
    Fighter* fs[2];

    return fighterPair(fs) && queueDone(0) && queueDone(1) && actionable(fs[0]) &&
           actionable(fs[1]) && still(fs[0]) && still(fs[1]);
}

/* Where f will be in t frames if it keeps moving as it is: a straight line,
 * plus gravity down to its fall speed in the air, stopping on the stage.
 * Close enough for the dozen or so frames a swing needs. */
static void project(Fighter* f, int t, float* x, float* y)
{
    float vy = f->pos_delta.y;
    int i;

    *x = f->cur_pos.x + f->pos_delta.x * (float) t;
    *y = f->cur_pos.y;
    if (f->ground_or_air != GA_Air) {
        return;
    }
    for (i = 0; i < t; i++) {
        vy -= f->co_attrs.gravity;
        if (vy < -f->co_attrs.terminal_velocity) {
            vy = -f->co_attrs.terminal_velocity;
        }
        *y += vy;
    }
    if (*y < 0.0f && fabsf(*x) <= FD_EDGE) {
        *y = 0.0f;
    }
}

/* The chaser is about to reach the launched fighter: within the next
 * CHASE_LEAD frames the two come within CHASE_REACH, with a grounded chaser's
 * jump counted in. Breaking this early leaves room for the jump squat and the
 * startup, and the bot times the swing itself. */
static bool chaseArriving(Fighter* a, Fighter* v)
{
    float lift = a->ground_or_air == GA_Air ? 0.0f : CHASE_HOP;
    int t;

    for (t = 0; t <= CHASE_LEAD; t++) {
        float ax, ay, vx, vy, dx, dy;

        project(a, t, &ax, &ay);
        project(v, t, &vx, &vy);
        dx = vx - ax;
        dy = vy - ay;
        dy = dy > lift ? dy - lift : dy > 0.0f ? 0.0f : dy;
        if (dx * dx + dy * dy <= CHASE_REACH * CHASE_REACH) {
            return true;
        }
    }
    return false;
}

int tactics_AirBreak(bool* picks)
{
    Fighter* fs[2];
    bool ended[2];
    int p;

    if (!fighterPair(fs)) {
        return -1;
    }
    for (p = 0; p < 2; p++) {
        bool stunned = fs[p]->x221C_b6;

        ended[p] = was_stunned[p] && !stunned;
        was_stunned[p] = stunned;
    }
    for (p = 0; p < 2; p++) {
        Fighter* v = fs[p];
        Fighter* a = fs[p ^ 1];
        bool chaser_free, meet, react;

        /* A fresh hit starts a fresh launch. */
        if (!tumbling(v) || v->dmg.x195c_hitlag_frames > 0.0f) {
            chase_seen[p] = false;
            continue;
        }
        /* Offstage the recovery is automatic. */
        if (!onStage(v)) {
            continue;
        }
        /* A trade has no one free to follow up. */
        chaser_free = onStage(a) && !tumbling(a) && a->victim_gobj == NULL &&
                      a->dmg.x195c_hitlag_frames <= 0.0f && queueDone(p ^ 1) && actionable(a);
        meet = !chase_seen[p] && chaser_free && chaseArriving(a, v);
        /* The launched fighter picks only once it can act, so its pick comes
         * out right away: when hitstun ends, or when the chaser arrives
         * after that. */
        react = actionable(v) && queueDone(p) && (ended[p] || meet);
        if (!meet && !react) {
            continue;
        }
        if (meet) {
            chase_seen[p] = true;
        }
        picks[p] = react;
        picks[p ^ 1] = meet;
        return p;
    }
    return -1;
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
    b->connected = false;
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

/* An aerial started now would meet the foe: where both will be when its
 * hitbox comes out is inside the move's air zone, with a little slack. */
static bool swingLands(Fighter* f, Fighter* enemy, const TacticsMoveInfo* info)
{
    float fx, fy, ex, ey, x, y;

    project(f, info->startup, &fx, &fy);
    project(enemy, info->startup, &ex, &ey);
    x = ex - fx;
    y = ey - fy;
    if (info->facing == TF_FRONT) {
        x *= f->facing_dir;
    } else if (info->facing == TF_BACK) {
        x *= -f->facing_dir;
    } else {
        x = fabsf(x);
    }
    return x >= info->air.x0 - 4.0f && x <= info->air.x1 + 4.0f && y >= info->air.y0 - 6.0f &&
           y <= info->air.y1 + 6.0f;
}

/* Melee's own CPU attack selector decides when the character has the move in
 * its tables; the generic zone is the fallback. */
static bool swingConnects(Fighter* f, Fighter* enemy, int move, const TacticsMoveInfo* info)
{
    if (tactics_AiKnows(f, move)) {
        return tactics_AiConnects(f, enemy, move);
    }
    return swingLands(f, enemy, info);
}

/* Hold an airborne aerial until it lines up, drifting to where the foe is
 * headed. True while holding. Gives up after AIR_WAIT frames, or just before
 * landing, where the swing would be lost. */
static bool holdSwing(Fighter* f, Fighter* enemy, Brain* b, int move, const TacticsMoveInfo* info)
{
    float ex, ey;

    if (swingConnects(f, enemy, move, info)) {
        b->forced = false;
        return false;
    }
    if (++b->approach >= AIR_WAIT || (f->cur_pos.y < 6.0f && f->pos_delta.y < 0.0f)) {
        b->forced = true;
        return false;
    }
    project(enemy, info->startup, &ex, &ey);
    f->cpu.lstick.x = fabsf(ex - f->cur_pos.x) > 3.0f ? (ex > f->cur_pos.x ? 127 : -127) : 0;
    return true;
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
    /* Getting hit, the ledge, a knockdown, or the air past the stage end
     * the pick: whatever was left of it would only play late, and the CPU
     * handles all of those better. It takes over next frame. */
    if (f->x221C_b6 || f->motion_id == ftCo_MS_CliffWait ||
        f->motion_id == ftCo_MS_DownWaitU || f->motion_id == ftCo_MS_DownWaitD ||
        (air && (fabsf(f->cur_pos.x) > FD_EDGE || f->cur_pos.y < FD_FLOOR)))
    {
        b->executing = false;
        b->slot = l->count;
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
        b->connected |= b->saw_attack && enemy->dmg.x195c_hitlag_frames > 0.0f;
        if ((b->saw_attack && actionable(f)) || b->age > 150) {
            if (info->input <= TI_THROW) {
                pc_log_line("tactics: P%d %s %s%s", f->player_id + 1, info->name,
                            b->connected ? "hit" : "missed",
                            info->input != TI_AERIAL ? ""
                            : b->forced              ? " (swung without a read)"
                                                     : " (timed by the CPU)");
            }
            b->executing = false;
            b->slot++;
            b->cooldown = 1;
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
            if (holdSwing(f, enemy, b, move, info)) {
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
    /* Already airborne, an aerial waits for its timing instead of its
     * range. */
    if (air && info->input == TI_AERIAL) {
        if (!holdSwing(f, enemy, b, move, info)) {
            beginMove(f, b, dir, move, info, air);
        }
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
