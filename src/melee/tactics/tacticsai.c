#include "tactics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <melee/ft/fighter.h>
#include <melee/ft/ftcmdscript.h>
#include <melee/ft/ftcpuattack.h>
#include <melee/ft/types.h>
#include <pc/pc.h>

/* The real CPU AI (ftcpuattack.c) picks an attack from per-character tables
 * in PlCo.dat. Each entry names a command script (a global index into
 * Fighter_804D64FC->cmdscripts) and the box, relative to the attacker, that
 * the hitbox covers a set number of frames after the input. The AI only
 * throws an attack whose box will hold the target at that frame. */

/* Same layout as ftcpuattack.c's entry; disc data, read in place. */
typedef struct DISC_STRUCT TacticsAiEntry {
    s32 cmd;
    s32 frames;    ///< frames from the input to the hitbox
    f32 x_front;   ///< box edges, scaled by the fighter's size
    f32 x_back;
    f32 y_low;
    f32 y_high;
    f32 weight;
    s32 period;    ///< only considered on AI ticks divisible by this
    s32 level;     ///< lowest CPU level that uses it
} TacticsAiEntry;

static const char* cmdName(int op)
{
    static const char* zero[] = {
        "?",        "A",        "-A",   "B",     "-B",     "X",      "-X",      "Y",     "-Y",
        "R",        "-R",       "L",    "-L",    "Z",      "-Z",     "Up",      "-Up",   "Down",
        "-Down",    "Right",    "-Right", "Left", "-Left", "Start",  "-Start",  "-All",
    };
    static const char* one[] = {
        "LX",      "LY",      "CX",      "CY",       "RTrig",     "LTrig",       "A for",
        "-A for",  "B for",   "-B for",  "X for",    "-X for",    "Y for",       "-Y for",
        "wait",    "L->dest", "LX->dest", "LX fwd",  "wait if motion", "scenario", "L->foe",
        "LX->foe",
    };

    if (op > 0 && op < (int) (sizeof(zero) / sizeof(zero[0]))) {
        return zero[op];
    }
    if (op == CpuCmd_Done) {
        return "end";
    }
    if (op >= CpuCmd_SetLstickX && op < CpuCmd_SetLstickX + (int) (sizeof(one) / sizeof(one[0]))) {
        return one[op - CpuCmd_SetLstickX];
    }
    return "op";
}

/* The input script behind an attack command, as text. */
static void describeScript(int cmd, char* out, size_t n)
{
    u8* p = DP(u8, DP(DiscU32, Fighter_804D64FC->cmdscripts)[cmd].v);
    size_t len = 0;
    int guard = 0;

    out[0] = '\0';
    while (p != NULL && *p != CpuCmd_Done && guard++ < 24 && len + 16 < n) {
        int op = *p++;

        if (op > CpuCmd_OneArgEnd) {
            len += snprintf(out + len, n - len, "%s(%d,%d) ", cmdName(op), (s8) p[0], (s8) p[1]);
            p += 2;
        } else if (op > CpuCmd_ZeroArgEnd) {
            len += snprintf(out + len, n - len, "%s(%d) ", cmdName(op), (s8) p[0]);
            p++;
        } else {
            len += snprintf(out + len, n - len, "%s ", cmdName(op));
        }
    }
}

static void dumpTable(const char* label, DiscU32* tables, int kind)
{
    TacticsAiEntry* e;
    char script[160];

    if (tables == NULL) {
        return;
    }
    e = DP(TacticsAiEntry, tables[kind].v);
    pc_log_line("tactics-ai: %s table", label);
    for (; e != NULL && e->cmd != 0; e++) {
        describeScript(e->cmd, script, sizeof(script));
        pc_log_line("tactics-ai:   cmd=0x%02X f=%d x=[%.1f,%.1f] y=[%.1f,%.1f] w=%.2f per=%d lv=%d  %s",
                    e->cmd, e->frames, e->x_front, e->x_back, e->y_low, e->y_high, e->weight,
                    e->period, e->level, script);
    }
}

/* Command ids are global script indices, so one id means the same input for
 * every character; the ground and air tables tell apart, say, up tilt and up
 * air (both 0x06). Side B is 0x1B, or 0x1C (a multi-hit string) or 0x1D (a
 * charge) depending on the character; down B is 0x26. */
static int moveCmd(int move, bool air)
{
    switch (move) {
    case TM_JAB:
        return air ? 0 : 0x02;
    case TM_FTILT:
        return air ? 0 : 0x07;
    case TM_UTILT:
        return air ? 0 : 0x06;
    case TM_DTILT:
        return air ? 0 : 0x0A;
    case TM_DASH_ATTACK:
        return air ? 0 : 0x27;
    case TM_FSMASH:
        return air ? 0 : 0x0F;
    case TM_USMASH:
        return air ? 0 : 0x0D;
    case TM_DSMASH:
        return air ? 0 : 0x0E;
    case TM_NAIR:
        return air ? 0x02 : 0;
    case TM_FAIR:
        return air ? 0x08 : 0;
    case TM_BAIR:
        return air ? 0x09 : 0;
    case TM_UAIR:
        return air ? 0x06 : 0;
    case TM_DAIR:
        return air ? 0x0A : 0;
    case TM_NEUTRAL_B:
        return 0x11;
    case TM_UP_B:
        return 0x1F;
    case TM_DOWN_B:
        return 0x26;
    case TM_FTHROW:
    case TM_BTHROW:
    case TM_UTHROW:
    case TM_DTHROW:
        return air ? 0 : 0x28; /* the grab; the throw comes once holding */
    default:
        return 0;
    }
}

static TacticsAiEntry* aiTable(int kind, bool air)
{
    DiscU32* tables;

    if (Fighter_804D64FC == NULL || kind < 0 || kind >= Ft_Kind_Max) {
        return NULL;
    }
    tables = DP(DiscU32, air ? Fighter_804D64FC->x8 : Fighter_804D64FC->x4);
    return tables != NULL ? DP(TacticsAiEntry, tables[kind].v) : NULL;
}

/* The entry a fighter kind's AI has for a move, or NULL. */
static TacticsAiEntry* findEntry(int kind, int move, bool air)
{
    TacticsAiEntry* e = aiTable(kind, air);
    int cmd = moveCmd(move, air);

    for (; e != NULL && e->cmd != 0; e++) {
        if (e->cmd == cmd ||
            (move == TM_SIDE_B && (e->cmd == 0x1B || e->cmd == 0x1C || e->cmd == 0x1D)))
        {
            return e;
        }
    }
    return NULL;
}

bool tactics_AiKnows(Fighter* fp, int move)
{
    return findEntry(fp->kind, move, fp->ground_or_air == GA_Air) != NULL;
}

bool tactics_AiConnects(Fighter* fp, Fighter* target, int move)
{
    bool air = fp->ground_or_air == GA_Air;
    TacticsAiEntry* e = findEntry(fp->kind, move, air);
    struct CpuFighter* cpu = &fp->cpu;
    struct CpuFighter saved;
    int got;

    if (e == NULL) {
        return false;
    }
    /* The real selector, with this one attack as the whole allow list. An
     * allow list also skips the selector's period gate, so the answer is
     * purely whether the hitbox will cover the target. The selector writes
     * a few CPU fields that the CPU think reads later; put them back. */
    saved = *cpu;
    cpu->xEC = 0;
    cpu->xC8 = 1;
    cpu->xA8_array[0] = e->cmd;
    got = ftCo_800B4AB0(fp, target, aiTable(fp->kind, air));
    *cpu = saved;
    return got == e->cmd;
}

/* One line per fighter kind: the ids in each table, and which catalogue
 * moves have no entry (those fall back to the generic zones). */
static void coverage(int kind)
{
    static const char* short_names[TM_COUNT] = {
        [TM_JAB] = "jab",      [TM_FTILT] = "ftilt",    [TM_UTILT] = "utilt",
        [TM_DTILT] = "dtilt",  [TM_DASH_ATTACK] = "dash", [TM_FSMASH] = "fsmash",
        [TM_USMASH] = "usmash", [TM_DSMASH] = "dsmash", [TM_NAIR] = "nair",
        [TM_FAIR] = "fair",    [TM_BAIR] = "bair",      [TM_UAIR] = "uair",
        [TM_DAIR] = "dair",    [TM_NEUTRAL_B] = "nB",   [TM_SIDE_B] = "sB",
        [TM_UP_B] = "uB",      [TM_DOWN_B] = "dB",
    };
    char ids[2][128];
    char missing[160];
    size_t len;
    int a, m;

    for (a = 0; a < 2; a++) {
        TacticsAiEntry* e = aiTable(kind, a != 0);

        len = 0;
        ids[a][0] = '\0';
        for (; e != NULL && e->cmd != 0 && len + 6 < sizeof(ids[a]); e++) {
            len += snprintf(ids[a] + len, sizeof(ids[a]) - len, "%02X ", e->cmd);
        }
    }
    len = 0;
    missing[0] = '\0';
    for (m = TM_JAB; m <= TM_DOWN_B; m++) {
        bool air = m >= TM_NAIR && m <= TM_DAIR;

        if (findEntry(kind, m, air) == NULL &&
            (m < TM_NEUTRAL_B || findEntry(kind, m, !air) == NULL))
        {
            len += snprintf(missing + len, sizeof(missing) - len, "%s ", short_names[m]);
        }
    }
    pc_log_line("tactics-ai: kind %2d  ground[%s] air[%s] missing[%s]", kind, ids[0], ids[1],
                missing);
}

void tactics_DumpAi(Fighter* fp)
{
    static bool done[Ft_Kind_Max];
    static bool all_done;
    const char* mode = getenv("MELEE_TACTICS_AI_DUMP");
    int k;

    if (mode == NULL || fp->kind < 0 || fp->kind >= Ft_Kind_Max || Fighter_804D64FC == NULL) {
        return;
    }
    /* "all": the coverage line for every playable kind, once, then the full
     * tables for the fighters in this match. */
    if (strcmp(mode, "all") == 0) {
        if (!all_done) {
            all_done = true;
            for (k = 0; k <= Ft_Kind_Emblem; k++) {
                if (k != Ft_Kind_Nana) {
                    coverage(k);
                }
            }
            for (k = 0; k <= Ft_Kind_Emblem; k++) {
                if (k != Ft_Kind_Nana) {
                    pc_log_line("tactics-ai: fighter kind %d", k);
                    dumpTable("ground", DP(DiscU32, Fighter_804D64FC->x4), k);
                    dumpTable("air", DP(DiscU32, Fighter_804D64FC->x8), k);
                }
            }
            /* A scripted dump run has what it came for. */
            if (getenv("MELEE_TACTICS_AUTOSTART") != NULL) {
                pc_log_line("tactics-ai: dump done, exiting");
                exit(0);
            }
        }
    }
    if (done[fp->kind]) {
        return;
    }
    done[fp->kind] = true;
    pc_log_line("tactics-ai: fighter kind %d", fp->kind);
    dumpTable("ground", DP(DiscU32, Fighter_804D64FC->x4), fp->kind);
    dumpTable("air", DP(DiscU32, Fighter_804D64FC->x8), fp->kind);
    dumpTable("smash", DP(DiscU32, Fighter_804D64FC->x10), fp->kind);
    dumpTable("edge", DP(DiscU32, Fighter_804D64FC->x1C), fp->kind);
}
