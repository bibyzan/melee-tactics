#ifndef MELEE_TACTICS_TACTICS_H
#define MELEE_TACTICS_TACTICS_H

/* Melee Tactics: short calls inside one real Melee stock. At each break you
 * pick one exchange, a single move or a two-move combo, from the options
 * that fit the frozen positions. The fighters play it on the real engine.
 * When the exchange ends, they run at each other and the match freezes again
 * just before they meet; a launched fighter is chased, and the freeze comes
 * just before the chaser reaches it. That repeats until
 * someone is knocked out (GM_TACTICS, tacticsmode.c).
 *
 * tacticsmoves.c  move catalogue: inputs, reach, names, draft weights
 * tacticsbot.c    per-fighter brain; replaces the CPU think for queued moves
 * tacticsmode.c   draft, the in-fight planning pause, and the VS rules
 * Draft rendering reuses the native menu backdrop and SIS lobby view. */

#include <Runtime/platform.h>

#include <melee/ft/forward.h>

#define TACTICS_SLOTS 2
#define TACTICS_PORTS 2

typedef enum TacticsMove {
    TM_JAB,
    TM_FTILT,
    TM_UTILT,
    TM_DTILT,
    TM_DASH_ATTACK,
    TM_FSMASH,
    TM_USMASH,
    TM_DSMASH,
    TM_NAIR,
    TM_FAIR,
    TM_BAIR,
    TM_UAIR,
    TM_DAIR,
    TM_NEUTRAL_B,
    TM_SIDE_B,
    TM_UP_B,
    TM_DOWN_B,
    TM_FTHROW,
    TM_BTHROW,
    TM_UTHROW,
    TM_DTHROW,
    TM_AIRDODGE, ///< reactions: only offered to a launched fighter
    TM_JUMP,
    TM_DRIFT,
    TM_COUNT,
    TM_NONE = 0xFF,
} TacticsMove;

/// How a move is put in: decides the macro the bot runs.
typedef enum TacticsInput {
    TI_GROUND,  ///< standing attack, A + left stick
    TI_DASH,    ///< dash attack: dash, then A
    TI_SMASH,   ///< C-stick on the ground
    TI_AERIAL,  ///< short hop from the ground, or directly in the air
    TI_SPECIAL, ///< B + left stick, ground or air
    TI_THROW,   ///< Z grab, then a stick direction once holding
    TI_DODGE,   ///< R in the air, stick away from the foe
    TI_JUMP,    ///< midair jump away from the foe
    TI_DRIFT,   ///< hold away from the foe until landing
} TacticsInput;

/// Which way the fighter must face for the move to connect.
typedef enum TacticsFacing {
    TF_FRONT, ///< target in front (turn first if not)
    TF_BACK,  ///< target behind (back air)
    TF_ANY,   ///< hits both sides, reach is |dx|
    TF_AUTO,  ///< the input itself picks the side (C-stick smash, side B)
} TacticsFacing;

/// Reach relative to the attacker, in stage units; x grows toward the target
/// side the move needs (see TacticsFacing), y grows upward.
typedef struct TacticsZone {
    float x0, x1;
    float y0, y1;
} TacticsZone;

typedef struct TacticsMoveInfo {
    const char* name;  ///< display name; specials carry the character's own
    u8 input;          ///< ::TacticsInput
    u8 facing;         ///< ::TacticsFacing
    u8 startup;        ///< frames to the first hitbox, used to lead the target
    u8 weight[3];      ///< draft weight as opener, link, finisher (0-3)
    TacticsZone ground; ///< reach when started on the ground
    TacticsZone air;    ///< reach when started in the air (x0 > x1: not usable)
} TacticsMoveInfo;

typedef struct TacticsLoadout {
    s8 ckind;                 ///< ::CharacterKind, or -1 for an empty slot
    u8 moves[TACTICS_SLOTS];  ///< this exchange only, ::TacticsMove or TM_NONE
    u8 count;
} TacticsLoadout;

/* tacticsmoves.c */
/// The move as this character does it: specials have their own names and
/// reach (a projectile, a lunge, a recovery...).
const TacticsMoveInfo* tactics_GetMove(int ckind, int move);
/// False for moves this character cannot sensibly draft (Zelda/Sheik's
/// transform).
bool tactics_MoveAllowed(int ckind, int move);
const char* tactics_FighterName(int ckind);

/* tacticsbot.c */
void tactics_SetLoadout(int port, const TacticsLoadout* loadout);
void tactics_ClearLoadouts(void);
/// Reset every bot's per-match state; call when a tactics match starts.
void tactics_BeginMatch(void);
/// Play this port's queue again from the first move. Positions are untouched.
void tactics_RestartQueue(int port);
/// An air break: the port of a launched fighter that just came out of
/// hitstun, or whose chaser is about to reach it (once per launch); -1 if
/// none. picks[] says which ports choose: the launched one only once it can
/// act, the chaser only when it is arriving. Call once per sim tick while not
/// planning.
int tactics_AirBreak(bool* picks);
/// Both queues are spent, neither fighter is attacking, grabbed, in hitstun,
/// or still recovering to the stage, and the two are about to meet.
bool tactics_BreakInAction(void);
/// True when this fighter is driven by a tactics bot instead of the CPU AI.
bool tactics_Controls(Fighter* fp);
/// Fill fp->cpu's pad state for this frame; runs in place of ftCo_800B3900.
void tactics_Think(Fighter_GObj* gobj);

/* tacticsmode.c */
/// True while the match is held for the next exchange.
bool tactics_IsPlanning(void);
/// Advance the planning pause. Call once per sim tick, before GObj procs,
/// and only while the game mode is GM_TACTICS.
void tactics_MatchFrame(void);

#endif
