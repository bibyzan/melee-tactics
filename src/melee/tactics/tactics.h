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
 * tacticsbot.c    plays a queued pick; between picks Melee's CPU drives, attacks held
 * tacticsai.c     reads the CPU attack tables and asks its selector about timing
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
    TM_SHIELD,  ///< neutral: shield, then grab out of it
    TM_BACKOFF, ///< neutral: dash away, then punish a whiff
    TM_GETUP,   ///< knocked down: only these are offered
    TM_ROLL_IN,
    TM_ROLL_AWAY,
    TM_GETUP_ATTACK,
    TM_STAY_DOWN,
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
    TI_SHIELD,  ///< hold shield; grab out of it once hit or once the foe whiffs
    TI_BACKOFF, ///< dash away; dash attack a whiff
    TI_GETUP,   ///< from lying down: stand, roll, get-up attack or stay
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
/// The character's special that fires from range (neutral B first), or
/// TM_NONE.
int tactics_Projectile(int ckind);
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
/// Both fighters can act, neither has anything queued, and neither is moving.
bool tactics_BothIdle(void);
/// Lying on the ground after a missed tech, bounce or knockdown hit.
bool tactics_Downed(Fighter* fp);
/// True while a queued pick drives this fighter. Between picks Melee's own
/// CPU AI drives it (see tactics_FilterAi).
bool tactics_Controls(Fighter* fp);
/// Run after the CPU think for a tactics fighter: strips the attack inputs,
/// so the CPU moves, shields and recovers but never attacks on its own.
void tactics_FilterAi(Fighter_GObj* gobj);
/// Fill fp->cpu's pad state for this frame; runs in place of ftCo_800B3900.
void tactics_Think(Fighter_GObj* gobj);

/* tacticsai.c */
/// With MELEE_TACTICS_AI_DUMP set, log this fighter's CPU attack tables and
/// their input scripts once per fighter kind.
void tactics_DumpAi(Fighter* fp);
/// True when this fighter's CPU attack table has an entry for the move in its
/// current state (ground or air).
bool tactics_AiKnows(Fighter* fp, int move);
/// Asks Melee's own CPU attack selector whether this move, started now, will
/// hit the target: the character's real frames to the hitbox and hitbox box,
/// with both fighters' motion predicted to that frame.
bool tactics_AiConnects(Fighter* fp, Fighter* target, int move);
/// Frames from the input to the move's hitbox by the CPU table, or -1 when
/// the table has no entry.
int tactics_AiFrames(int kind, int move, bool air);

/* tacticssync.c */
/// Start a match. With on, the fight runs from seed so that another machine
/// given the same seed and the same picks plays it identically.
void tactics_SyncBegin(u32 seed, bool on);
void tactics_SyncEnd(void);
bool tactics_Synced(void);
/// Once per sim tick, last thing before the fighters run: starts the fight's
/// clock when both are out of their entry, then reseeds every tick that is
/// not frozen.
void tactics_SyncFrame(bool frozen);
/// Ticks simulated since the fight went live, or -1 before.
s32 tactics_SyncTick(void);
/// Random numbers for the tactics layer's own choices: seeded and shared
/// while synced, rand() otherwise.
u32 tactics_SyncRand(void);
/// A hash of both fighters' state and the game RNG.
u32 tactics_SyncChecksum(void);
/// Count a break and log its checksum; returns the break number (0 when not
/// synced).
int tactics_SyncBreak(void);

/* tacticsnet.c: a match against another machine over pc_link */
/// This build can play online (pc_link_available).
bool tactics_NetAvailable(void);
/// Open a lobby under name (this side hosts, P1), or join one by its room.
bool tactics_NetHost(const char* name, bool open);
bool tactics_NetJoin(const char* room);
/// End the session.
void tactics_NetLeave(void);
/// A session is under way (hosting or joined).
bool tactics_NetOn(void);
/// The other player is connected.
bool tactics_NetConnected(void);
/// 0 on the host (P1), 1 on the guest (P2).
int tactics_NetLocalPort(void);
/// What to tell the player about the connection.
const char* tactics_NetStatus(void);
bool tactics_NetClosed(void);
void tactics_NetPoll(void);
/// The draft's handshake: true once both sides agreed on the match.
bool tactics_NetLobby(int my_ckind, u32* seed, int* p1_ckind, int* p2_ckind);
/// Start break n: forget the last break's picks.
void tactics_NetBreak(int n);
/// Commit this machine's pick (-1: not choosing) with the break's checksum.
void tactics_NetSendPick(int pick, u32 sum);
/// True once the other side's pick is revealed and verified (-1: not choosing).
bool tactics_NetTheirPick(int* pick);
/// The other side has committed its pick for this break.
bool tactics_NetTheyCommitted(void);
/// This side has committed and the other side's reveal is still out.
bool tactics_NetWaiting(void);

/* tacticsmode.c */
/// True while the match is held for the next exchange.
bool tactics_IsPlanning(void);
/// A row tapped on the page's buttons: picked and confirmed on the next frame
/// of the pick menu (src/pc/plan_ui.h).
void tactics_PlanTap(int index);
/// A fighter tapped on the page's grid: slot 0 or 1, character kind or -1
/// for random (pc_fighter_ui).
void tactics_FighterTap(int slot, int ckind);
/// A menu row tapped on the page (pc_fighter_ui): chosen as if with A.
void tactics_MenuTap(int row);
/// Advance the planning pause. Call once per sim tick, before GObj procs,
/// and only while the game mode is GM_TACTICS.
void tactics_MatchFrame(void);

#endif
