#include "tactics.h"

#include <math.h>
#include <stdlib.h>
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
    int slot, phase, age, cooldown, frames, approach, aim;
    bool executing, saw_attack, connected;
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
/* An air pick needs this many frames of air left to come out at all: a
 * reaction's startup, or a chaser's jump squat and swing. Closer to the
 * ground the CPU handles the landing, with no pause. */
#define REACT_AIR_FRAMES 18
#define CHASE_AIR_FRAMES 12
/* Frames an aerial should hit before its user or its target lands. */
#define AIR_MARGIN 3
/* Frames an aerial keeps aiming, across hops, before its pick is dropped. */
#define AIM_LIMIT 75
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
    b->aim = 0;
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
    if (getenv("MELEE_TACTICS_DEBUG") != NULL) {
        float gx = fs[0]->cur_pos.x - fs[1]->cur_pos.x;
        float gy = fs[0]->cur_pos.y - fs[1]->cur_pos.y;

        if (gx * gx + gy * gy <= MEET_TRIGGER * MEET_TRIGGER) {
            pc_log_line("tactics: meet gap=%.0f,%.0f P1 state=%d busy=%d done=%d  "
                        "P2 state=%d busy=%d done=%d",
                        gx, gy, fs[0]->motion_id, exchangeBusy(fs[0]), queueDone(0),
                        fs[1]->motion_id, exchangeBusy(fs[1]), queueDone(1));
        }
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

bool tactics_Downed(Fighter* fp)
{
    return inRange(fp->motion_id, ftCo_MS_DownBoundU, ftCo_MS_DownDamageU) ||
           inRange(fp->motion_id, ftCo_MS_DownBoundD, ftCo_MS_DownDamageD);
}

static bool downWait(Fighter* f)
{
    return f->motion_id == ftCo_MS_DownWaitU || f->motion_id == ftCo_MS_DownWaitD;
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

static int framesToLand(Fighter* f);
int tactics_AerialFrames(Fighter* f, int move)
{
    int frames = tactics_AiFrames(f->kind, move, true);
    const TacticsMoveInfo* info;

    if (frames >= 0) {
        return frames;
    }
    info = tactics_GetMove(-1, move);
    return info != NULL ? info->startup : 12;
}

int tactics_JumpSquat(Fighter* f)
{
    return (int) f->co_attrs.jump_startup_time;
}

/* The character's quickest aerial: the least air a swing needs. */
static int quickestAerial(Fighter* f)
{
    int best = 99;
    int m;

    for (m = TM_NAIR; m <= TM_DAIR; m++) {
        int frames = tactics_AerialFrames(f, m);

        if (frames < best) {
            best = frames;
        }
    }
    return best;
}

/* Frames until a fighter lands on the stage, by its current drift; 99 for
 * longer, or for a fall past the stage. */
int tactics_FramesToLand(Fighter* f)
{
    return framesToLand(f);
}

static int framesToLand(Fighter* f)
{
    float vy = f->pos_delta.y;
    float x = f->cur_pos.x;
    float y = f->cur_pos.y;
    int t;

    if (f->ground_or_air != GA_Air) {
        return 0;
    }
    for (t = 1; t < 99; t++) {
        vy -= f->co_attrs.gravity;
        if (vy < -f->co_attrs.terminal_velocity) {
            vy = -f->co_attrs.terminal_velocity;
        }
        y += vy;
        x += f->pos_delta.x;
        if (y <= 0.0f && fabsf(x) <= FD_EDGE) {
            return t;
        }
    }
    return 99;
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
        int air_left;

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
        air_left = framesToLand(v);
        /* A chase pick is a swing: it needs the chaser's jump squat (from
         * the ground) and its quickest aerial to fit before the foe lands,
         * and an airborne chaser must not land first either. */
        if (chaser_free) {
            int swing = quickestAerial(a) + AIR_MARGIN;
            int need = swing + (a->ground_or_air == GA_Air ? 0 : tactics_JumpSquat(a));

            chaser_free = air_left >= CHASE_AIR_FRAMES && air_left >= need &&
                          (a->ground_or_air != GA_Air || framesToLand(a) >= swing);
        }
        meet = !chase_seen[p] && chaser_free && chaseArriving(a, v);
        /* The launched fighter picks only once it can act, so its pick comes
         * out right away: when hitstun ends, or when the chaser arrives
         * after that. With no time left before landing there is no pick. */
        react = actionable(v) && queueDone(p) && air_left >= REACT_AIR_FRAMES &&
                (ended[p] || meet);
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
    /* As the CPU's own PressR does: a shield reads the analog trigger. */
    f->cpu.ltrigger = 0;
    f->cpu.rtrigger = (buttons & HSD_PAD_R) ? 0xFF : 0;
}

static bool dashing(Fighter* f);

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
        /* Neutral first, so the next frame's full push is a dash; already
         * dashing or running, keep it going. */
        pad(f, dashing(f) ? dir * 127 : 0, 0, 0, 0, 0);
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
    case TI_SHIELD:
        pad(f, 0, 0, HSD_PAD_R, 0, 0);
        b->phase = 0;
        break;
    case TI_BACKOFF:
        pad(f, -dir * 127, 0, 0, 0, 0);
        b->phase = 0;
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

enum { AIM_SWING, AIM_HOLD, AIM_GIVE_UP };

/* Keep an airborne aerial aimed until the swing will land: drift to where the
 * foe is headed, and spend the midair jump on a foe still above. A blind
 * swing almost never lands, so none is thrown; a fighter that lands without
 * an opening jumps again (see the phase 0 path). After AIM_LIMIT frames with
 * no opening, give the pick up. */
static int aimSwing(Fighter* f, Fighter* enemy, Brain* b, int move, const TacticsMoveInfo* info)
{
    float ex, ey;

    if (swingConnects(f, enemy, move, info)) {
        return AIM_SWING;
    }
    if (++b->aim >= AIM_LIMIT) {
        return AIM_GIVE_UP;
    }
    project(enemy, info->startup, &ex, &ey);
    f->cpu.lstick.x = fabsf(ex - f->cur_pos.x) > 3.0f ? (ex > f->cur_pos.x ? 127 : -127) : 0;
    /* Pressing every other frame gives the button a fresh press. */
    if (f->pos_delta.y <= 0.0f && ey - f->cur_pos.y > info->air.y1 &&
        f->x1968_jumpsUsed < f->co_attrs.max_jumps && (b->frames & 1))
    {
        f->cpu.buttons = HSD_PAD_X;
    }
    return AIM_HOLD;
}

static void giveUp(Fighter* f, Brain* b, const TacticsMoveInfo* info)
{
    pc_log_line("tactics: P%d %s gave up (no opening)", f->player_id + 1, info->name);
    b->executing = false;
    b->slot++;
    b->aim = 0;
}

static bool running(Fighter* f)
{
    return inRange(f->motion_id, ftCo_MS_TurnRun, ftCo_MS_RunBrake);
}

/* In a dash or a run proper, where A is a dash attack. */
static bool dashing(Fighter* f)
{
    return inRange(f->motion_id, ftCo_MS_Dash, ftCo_MS_RunDirect);
}

/* A run only gives way to a dash attack, a grab, a jump or side B. Anything
 * else crouches out of it first; every ground attack comes out of a crouch. */
static bool needsStop(int move, const TacticsMoveInfo* info)
{
    return info->input == TI_GROUND || info->input == TI_SMASH ||
           (info->input == TI_SPECIAL && move != TM_SIDE_B);
}

/* The foe is swinging or grabbing: what a shield waits for and a back-off
 * punishes. */
static bool foeSwinging(Fighter* enemy)
{
    return inRange(enemy->motion_id, ftCo_MS_Attack11, ftCo_MS_AttackAirLw) ||
           inRange(enemy->motion_id, ftCo_MS_Catch, ftCo_MS_CatchDash) ||
           enemy->motion_id >= ftCo_MS_Count;
}

/* Shield > Grab: hold shield. Once it takes a hit (the shield stun ends) or
 * the foe swings close by, grab out of it. Nothing comes, it drops. */
static void shieldThink(Fighter* f, Fighter* enemy, Brain* b, float dx)
{
    bool shielding = inRange(f->motion_id, ftCo_MS_GuardOn, ftCo_MS_GuardSetOff);

    if (f->motion_id == ftCo_MS_GuardSetOff) {
        b->phase = 1;
    }
    /* The shield waits for the foe to arrive, then half a second more; a
     * foe still in the air is still coming. */
    if (fabsf(dx) < 40.0f && enemy->ground_or_air != GA_Air) {
        b->approach++;
    }
    if ((b->approach > 40 || b->age > 120) && b->phase == 0) {
        pc_log_line("tactics: P%d shield dropped (nothing came)", f->player_id + 1);
        b->saw_attack = true;
        return;
    }
    if (shielding && f->motion_id != ftCo_MS_GuardSetOff && b->age > 3 &&
        (b->phase == 1 || (foeSwinging(enemy) && fabsf(dx) < 26.0f)))
    {
        pad(f, 0, 0, HSD_PAD_R | HSD_PAD_A, 0, 0);
        return;
    }
    pad(f, 0, 0, HSD_PAD_R, 0, 0);
}

/* Back off > Punish: dash away, wait for the foe to swing at the air, then
 * dash attack into it. Nothing to punish, it ends. */
/* Dash in, then dash attack once it will reach. A dash only starts when the
 * stick snaps from neutral to full, so until the fighter is dashing the
 * stick flicks between the two; A pressed from a walk or a stand would be a
 * forward tilt or a jab instead. 1 once A is in, -1 when no dash came. */
static int dashAttack(Fighter* f, Fighter* enemy, Brain* b, int dir, const TacticsMoveInfo* info)
{
    float dx = fabsf(enemy->cur_pos.x - f->cur_pos.x);
    bool close;

    if (!dashing(f)) {
        if (b->age > 30) {
            return -1;
        }
        pad(f, (b->age & 1) ? dir * 127 : 0, 0, 0, 0, 0);
        return 0;
    }
    close = tactics_AiKnows(f, TM_DASH_ATTACK) ? tactics_AiConnects(f, enemy, TM_DASH_ATTACK)
                                              : dx <= info->ground.x1;
    if (!close && b->age < 45) {
        pad(f, dir * 127, 0, 0, 0, 0);
        return 0;
    }
    pad(f, dir * 127, 0, HSD_PAD_A, 0, 0);
    return 1;
}

static void backOffThink(Fighter* f, Fighter* enemy, Brain* b, float dx, int dir,
                         const TacticsMoveInfo* dash)
{
    if (b->phase == 0) {
        if (b->age < 12 && f->ground_or_air == GA_Ground &&
            f->cur_pos.x * -dir < FD_EDGE - 20.0f)
        {
            pad(f, -dir * 127, 0, 0, 0, 0);
            return;
        }
        b->phase = 1;
    }
    if (b->phase == 1) {
        if (b->age > 55) {
            b->saw_attack = true;
        } else if (foeSwinging(enemy) && fabsf(dx) < 55.0f &&
                   f->ground_or_air == GA_Ground && actionable(f))
        {
            b->phase = 2;
            b->age = 0;
        }
        return;
    }
    /* phase 2: run in on the whiff. No dash, no punish. */
    if (dash == NULL || dashAttack(f, enemy, b, dir, dash) < 0 || b->age > 50) {
        b->saw_attack = true;
    }
}

/* From lying down: press the way up. Staying down just waits a moment; the
 * CPU gets up after that. */
static void getUpThink(Fighter* f, Brain* b, int move, int dir)
{
    if (!b->executing) {
        b->executing = true;
        b->age = 0;
        b->saw_attack = false;
        b->connected = false;
    }
    b->age++;
    switch (move) {
    case TM_GETUP:
        pad(f, 0, 127, 0, 0, 0);
        break;
    case TM_ROLL_IN:
        pad(f, dir * 127, 0, 0, 0, 0);
        break;
    case TM_ROLL_AWAY:
        pad(f, -dir * 127, 0, 0, 0, 0);
        break;
    case TM_GETUP_ATTACK:
        pad(f, 0, 0, (b->age & 1) ? HSD_PAD_A : 0, 0, 0);
        break;
    default:
        if (b->age > 40) {
            b->executing = false;
            b->slot++;
        }
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
    /* Lying down with a way up queued: that is the pick, not a reason to
     * drop it. */
    if (downWait(f) && !f->x221C_b6) {
        int next = nextSlot(f->player_id);
        const TacticsMoveInfo* up =
            next < l->count ? tactics_GetMove(l->ckind, l->moves[next]) : NULL;

        if (up != NULL && up->input == TI_GETUP) {
            b->slot = next;
            getUpThink(f, b, l->moves[next], dir);
            return;
        }
    }
    /* Getting hit, the ledge, a knockdown, or the air past the stage end
     * the pick: whatever was left of it would only play late, and the CPU
     * handles all of those better. It takes over next frame. */
    if (f->x221C_b6 || f->motion_id == ftCo_MS_CliffWait ||
        f->motion_id == ftCo_MS_DownWaitU || f->motion_id == ftCo_MS_DownWaitD ||
        (air && (fabsf(f->cur_pos.x) > FD_EDGE || f->cur_pos.y < FD_FLOOR)))
    {
        if (b->slot < l->count) {
            const TacticsMoveInfo* cut = tactics_GetMove(l->ckind, l->moves[b->slot]);

            pc_log_line("tactics: P%d %s cut short (%s)", f->player_id + 1,
                        cut != NULL ? cut->name : "?",
                        f->x221C_b6                        ? "hit"
                        : f->motion_id == ftCo_MS_CliffWait ? "ledge"
                        : air                               ? "offstage"
                                                            : "knockdown");
        }
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
        case TI_SHIELD:
            b->saw_attack |= inRange(f->motion_id, ftCo_MS_Catch, ftCo_MS_ThrowLw);
            break;
        case TI_BACKOFF:
            b->saw_attack |= f->motion_id == ftCo_MS_AttackDash;
            break;
        case TI_GETUP:
            b->saw_attack |= !downWait(f);
            break;
        default:
            b->saw_attack |= inRange(f->motion_id, ftCo_MS_Attack11, ftCo_MS_AttackAirLw) ||
                             inRange(f->motion_id, ftCo_MS_Catch, ftCo_MS_ThrowLw) ||
                             f->motion_id >= ftCo_MS_Count;
            break;
        }
        /* A grab lands without hitlag: holding the foe is the hit. */
        b->connected |= b->saw_attack &&
                        (enemy->dmg.x195c_hitlag_frames > 0.0f || f->victim_gobj != NULL);
        if ((b->saw_attack && actionable(f)) || b->age > 150) {
            if (info->input <= TI_THROW || info->input == TI_SHIELD || info->input == TI_BACKOFF) {
                pc_log_line("tactics: P%d %s %s", f->player_id + 1, info->name,
                            b->connected ? "hit" : "missed");
            }
            b->executing = false;
            b->slot++;
            b->aim = 0;
            b->cooldown = 1;
            return;
        }
        if (info->input == TI_SHIELD && !b->saw_attack) {
            shieldThink(f, enemy, b, dx);
            return;
        }
        if (info->input == TI_BACKOFF && !b->saw_attack) {
            backOffThink(f, enemy, b, dx, dir, tactics_GetMove(l->ckind, TM_DASH_ATTACK));
            return;
        }
        if (info->input == TI_SHIELD && f->motion_id == ftCo_MS_CatchWait) {
            pad(f, 0, 127, 0, 0, 0); /* up throw: safe, sets up the next read */
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

            /* A target far overhead gets a full hop. Landed without an
             * opening, jump again. */
            if (!air) {
                if (dy > info->ground.y1 && f->motion_id == ftCo_MS_KneeBend) {
                    pad(f, 0, 0, HSD_PAD_X, 0, 0);
                } else if (actionable(f) && (b->frames & 1)) {
                    pad(f, 0, 0, HSD_PAD_X, 0, 0);
                }
                if (++b->aim >= AIM_LIMIT) {
                    giveUp(f, b, info);
                }
                return;
            }
            switch (aimSwing(f, enemy, b, move, info)) {
            case AIM_HOLD:
                return;
            case AIM_GIVE_UP:
                giveUp(f, b, info);
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
            switch (dashAttack(f, enemy, b, dir, info)) {
            case 1:
                b->phase = 1;
                break;
            case -1:
                giveUp(f, b, info);
                break;
            }
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
        switch (aimSwing(f, enemy, b, move, info)) {
        case AIM_SWING:
            beginMove(f, b, dir, move, info, air);
            break;
        case AIM_GIVE_UP:
            giveUp(f, b, info);
            break;
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
        bool wait;
        int face;

        /* Melee's CPU selector says when the character's own hitbox will
         * land; moves it has no entry for use the generic reach. Either way,
         * after half a second of closing in, swing anyway. */
        /* A dash attack closes the distance itself: walking in first would
         * only leave the stick half pushed, which never becomes a dash. */
        wait = info->input == TI_DASH           ? false
               : tactics_AiKnows(f, move) ? !tactics_AiConnects(f, enemy, move)
                                          : fabsf(dx) > reach;
        if (wait && ++b->approach < APPROACH_LIMIT) {
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
