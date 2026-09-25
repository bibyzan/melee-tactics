#include "tactics.h"
#include "tacticsmode.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/gx/GXStruct.h>
#include <dolphin/pad.h>
#include <melee/ft/inlines.h>
#include <melee/ft/types.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A36.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/gmresult.h>
#include <melee/gm/gm_unsplit.h>
#include <melee/gm/gmvs.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/gm/gmonlinemode.h>
#include <melee/gr/forward.h>
#include <melee/lb/lblanguage.h>
#include <pc/link.h>
#include <pc/pc.h>
#include <pc/render_scale.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/hsd_3915.h>
#include <sysdolphin/baselib/random.h>
#include <sysdolphin/baselib/sislib.h>

/* Short, because the fighters are running in while it counts. */
#define PLAN_SETTLE 2
#define PLAN_AUTO_FRAMES 45
/* Both fighters free and idle this long opens a break. */
#define IDLE_BREAK 30
/* A scripted run with no break for this long has stalled. */
#define SCRIPT_STALL_FRAMES (20 * 60)
#define MENU_ROWS 4
#define OPT_CAP 8

static TacticsLoadout draft[2] = {
    { CKind_Fox, { TM_NONE, TM_NONE }, 0 },
    { CKind_Mars, { TM_NONE, TM_NONE }, 0 },
};

static int cursor, frames;
static bool auto_started;
static bool live, planning, auto_resume;
/* How long a scripted run holds each break; varying it checks that the
 * pause itself changes nothing. */
static int auto_frames = PLAN_AUTO_FRAMES;
/* A match against another machine (tacticsnet.c). net is set for the fight
 * once the lobby agreed on the seed and both fighters. */
static bool online, online_ready, online_agreed, after_online;
static u32 online_seed, brk_sum;
static int online_ckind[2];
static bool p2_cpu = true;
static int settle, plan_port, plan_frames, idle, since_plan;
/* The launched port during a mid-air break, or -1 at a normal break. */
static int react_port = -1;
static char result[96];

enum {
    LINE_TITLE,
    LINE_SUB,
    LINE_OPT,
    LINE_HINT = LINE_OPT + OPT_CAP,
    LINE_COUNT
};

/* One row in the break menu. b == TM_NONE is a single move. */
typedef struct Opt {
    u8 a, b;
} Opt;

enum {
    D_CLOSE = 1,
    D_MID = 2,
    D_FAR = 4,
    V_LEVEL = 1,
    V_ABOVE = 2,
    V_BELOW = 4,
    P_COMBO = 1,
    P_KILL = 2,
};

typedef struct Recipe {
    u8 a, b;
    u8 air;
    u8 dist;
    u8 vert;
    u8 pct;
} Recipe;

/* Preference order. The first recipes that match the freeze are the rows.
 * The run-in options (grab, aerial, dash attack, side B) sit near the top so
 * they survive the row cap next to the close-range combos. */
static const Recipe recipes[] = {
    { TM_DTILT, TM_UTILT, 0, D_CLOSE, V_LEVEL | V_ABOVE, P_COMBO },
    { TM_UTHROW, TM_NONE, 0, D_CLOSE, V_LEVEL | V_ABOVE, P_COMBO },
    { TM_NAIR, TM_FAIR, 0, D_CLOSE | D_MID, V_LEVEL, P_COMBO },
    { TM_DASH_ATTACK, TM_FTILT, 0, D_MID | D_FAR, V_LEVEL, P_COMBO },
    { TM_SIDE_B, TM_NONE, 0, D_MID | D_FAR, V_LEVEL, P_COMBO | P_KILL },
    { TM_JAB, TM_FTILT, 0, D_CLOSE, V_LEVEL, P_COMBO },
    { TM_FSMASH, TM_NONE, 0, D_CLOSE, V_LEVEL, P_KILL },
    { TM_USMASH, TM_NONE, 0, D_CLOSE, V_LEVEL | V_ABOVE, P_KILL },
    { TM_BTHROW, TM_NONE, 0, D_CLOSE, V_LEVEL, P_KILL },
    { TM_FAIR, TM_NONE, 0, D_CLOSE | D_MID, V_LEVEL, P_COMBO | P_KILL },
    { TM_DTILT, TM_UAIR, 0, D_CLOSE, V_LEVEL | V_ABOVE, P_COMBO },
    { TM_UTILT, TM_UAIR, 0, D_CLOSE, V_LEVEL | V_ABOVE, P_COMBO },
    { TM_BAIR, TM_NONE, 0, D_CLOSE | D_MID, V_LEVEL, P_KILL },
    { TM_UAIR, TM_NONE, 0, D_CLOSE, V_ABOVE, P_COMBO | P_KILL },
    { TM_DAIR, TM_NONE, 0, D_CLOSE, V_BELOW, P_COMBO | P_KILL },
    { TM_DASH_ATTACK, TM_NONE, 0, D_MID | D_FAR, V_LEVEL, P_COMBO | P_KILL },
    { TM_FTILT, TM_NONE, 0, D_CLOSE | D_MID, V_LEVEL, P_COMBO | P_KILL },
    { TM_NEUTRAL_B, TM_NONE, 0, D_MID | D_FAR, V_LEVEL | V_ABOVE | V_BELOW, P_COMBO | P_KILL },
    { TM_DOWN_B, TM_NONE, 0, D_CLOSE, V_LEVEL | V_BELOW, P_COMBO | P_KILL },
    { TM_NAIR, TM_NONE, 1, D_CLOSE | D_MID, V_LEVEL | V_ABOVE | V_BELOW, P_COMBO | P_KILL },
    { TM_NAIR, TM_FAIR, 1, D_CLOSE | D_MID, V_LEVEL, P_COMBO },
    { TM_FAIR, TM_NONE, 1, D_CLOSE | D_MID, V_LEVEL, P_COMBO | P_KILL },
    { TM_BAIR, TM_NONE, 1, D_CLOSE | D_MID, V_LEVEL, P_KILL },
    { TM_UAIR, TM_NONE, 1, D_CLOSE | D_MID, V_ABOVE, P_COMBO | P_KILL },
    { TM_DAIR, TM_NONE, 1, D_CLOSE | D_MID, V_BELOW | V_LEVEL, P_COMBO | P_KILL },
    { TM_NEUTRAL_B, TM_NONE, 1, D_MID | D_FAR, V_LEVEL, P_COMBO | P_KILL },
};

static Opt opts[2][OPT_CAP];
static int opt_n[2];
static int opt_cursor[2];
static bool opt_locked[2];
/* Which ports pick at this break. */
static bool choosing[2];

typedef struct PlanLine {
    int entry;
    char text[64];
} PlanLine;

static HSD_Text* plan_text;
static HSD_GObj* plan_panel;
static PlanLine plan_lines[LINE_COUNT];
static bool plan_ui;

static GXColor col_white = { 0xFF, 0xFF, 0xFF, 0xFF };
static GXColor col_dim = { 0xB0, 0xB0, 0xB0, 0xFF };
static GXColor col_gold = { 0xFF, 0xE0, 0x60, 0xFF };

static const char* moveName(int ckind, int move)
{
    const TacticsMoveInfo* info;

    if (move == TM_NONE) {
        return "pass";
    }
    info = tactics_GetMove(ckind, move);
    return info != NULL ? info->name : "pass";
}

static void optionLabel(char* buf, size_t n, int ckind, const Opt* opt)
{
    if (opt->a == TM_NONE) {
        snprintf(buf, n, "Wait");
        return;
    }
    if (opt->b == TM_NONE) {
        snprintf(buf, n, "%s", moveName(ckind, opt->a));
        return;
    }
    snprintf(buf, n, "%s > %s", moveName(ckind, opt->a), moveName(ckind, opt->b));
}

static bool sameOpt(const Opt* a, const Opt* b)
{
    return a->a == b->a && a->b == b->b;
}

static Fighter* portFighter(int which);

static int buildOptions(int which, Opt* out, int cap)
{
    Fighter* self = portFighter(which);
    Fighter* foe = portFighter(which ^ 1);
    int ckind = draft[which].ckind;
    int air, dist, vert, pct;
    int n = 0;
    int i;
    float dx, dy;

    if (self == NULL || foe == NULL) {
        out[0].a = TM_FTILT;
        out[0].b = TM_NONE;
        return 1;
    }
    dx = fabsf(foe->cur_pos.x - self->cur_pos.x);
    dy = foe->cur_pos.y - self->cur_pos.y;
    air = self->ground_or_air == GA_Air;
    /* The fighters are running at each other, so mid range soon is close. */
    dist = dx < 20.0f ? D_CLOSE : dx < 48.0f ? D_CLOSE | D_MID : D_FAR;
    vert = dy > 14.0f ? V_ABOVE : dy < -14.0f ? V_BELOW : V_LEVEL;
    pct = foe->dmg.x1830_percent >= 80.0f ? P_KILL : P_COMBO;
    for (i = 0; i < (int) (sizeof(recipes) / sizeof(recipes[0])) && n < cap; i++) {
        const Recipe* r = &recipes[i];
        Opt opt;
        int k;
        bool dup = false;

        if ((r->air != 0) != (air != 0)) {
            continue;
        }
        if ((r->dist & dist) == 0 || (r->vert & vert) == 0 || (r->pct & pct) == 0) {
            continue;
        }
        if (!tactics_MoveAllowed(ckind, r->a)) {
            continue;
        }
        if (r->b != TM_NONE && !tactics_MoveAllowed(ckind, r->b)) {
            continue;
        }
        opt.a = r->a;
        opt.b = r->b;
        for (k = 0; k < n; k++) {
            if (sameOpt(&out[k], &opt)) {
                dup = true;
            }
        }
        if (!dup) {
            out[n++] = opt;
        }
    }
    if (n == 0) {
        out[0].a = air ? TM_NAIR : TM_FTILT;
        out[0].b = TM_NONE;
        n = 1;
    }
    return n;
}

static int addOpts(int ckind, const Opt* list, int count, Opt* out, int n, int cap)
{
    int i;

    for (i = 0; i < count && n < cap; i++) {
        const Opt* o = &list[i];

        if (o->a == TM_NONE ||
            (tactics_MoveAllowed(ckind, o->a) &&
             (o->b == TM_NONE || tactics_MoveAllowed(ckind, o->b))))
        {
            out[n++] = *o;
        }
    }
    return n;
}

/* The launched fighter as the chaser closes in: get away, or swing. */
static int buildReactOptions(int which, Opt* out, int cap)
{
    static const Opt react[] = {
        { TM_AIRDODGE, TM_NONE }, { TM_JUMP, TM_NONE }, { TM_DRIFT, TM_NONE },
        { TM_NAIR, TM_NONE },     { TM_FAIR, TM_NONE }, { TM_BAIR, TM_NONE },
        { TM_DAIR, TM_NONE },     { TM_UAIR, TM_NONE },
    };
    Fighter* self = portFighter(which);
    int ckind = draft[which].ckind;

    if (self != NULL && self->x1968_jumpsUsed >= self->co_attrs.max_jumps) {
        return addOpts(ckind, react + 2, 6, out, addOpts(ckind, react, 1, out, 0, cap), cap);
    }
    return addOpts(ckind, react, 8, out, 0, cap);
}

/* The chaser about to reach a launched foe: a move or a combo, or wait for
 * the read. */
static int buildChaseOptions(int which, Opt* out, int cap)
{
    static const Opt ground[] = {
        { TM_UAIR, TM_NONE },   { TM_UAIR, TM_UAIR },  { TM_NAIR, TM_FAIR },
        { TM_FAIR, TM_NONE },   { TM_BAIR, TM_NONE },  { TM_UTILT, TM_UAIR },
        { TM_USMASH, TM_NONE }, { TM_NONE, TM_NONE },
    };
    static const Opt air[] = {
        { TM_UAIR, TM_NONE }, { TM_UAIR, TM_UAIR }, { TM_NAIR, TM_FAIR }, { TM_FAIR, TM_NONE },
        { TM_BAIR, TM_NONE }, { TM_DAIR, TM_NONE }, { TM_NONE, TM_NONE },
    };
    Fighter* self = portFighter(which);
    int ckind = draft[which].ckind;

    if (self != NULL && self->ground_or_air == GA_Air) {
        return addOpts(ckind, air, 7, out, 0, cap);
    }
    return addOpts(ckind, ground, 8, out, 0, cap);
}

static Fighter* portFighter(int which)
{
    HSD_GObj* g;

    for (g = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_FIGHTER]; g != NULL; g = g->next) {
        Fighter* f = GET_FIGHTER(g);
        if (f->player_id == which && !f->is_sub_fighter) {
            return f;
        }
    }
    return NULL;
}

static int portPercent(int which)
{
    Fighter* f = portFighter(which);
    if (f == NULL) {
        return 0;
    }
    return (int) f->dmg.x1830_percent;
}

static void setPlanLine(PlanLine* line, const char* text)
{
    if (strcmp(line->text, text) == 0) {
        return;
    }
    snprintf(line->text, sizeof(line->text), "%s", text);
    HSD_SisLib_803A70A0(plan_text, line->entry, "%s", line->text);
}

static void setPlanColor(PlanLine* line, GXColor* color)
{
    HSD_SisLib_803A74F0(plan_text, line->entry, color);
}

static void drawPlanPanel(HSD_GObj* gobj, int pass)
{
    static GXColor panel = { 0, 0, 0, 0xC0 };
    static GXColor rule = { 0xFF, 0xE0, 0x60, 0xA0 };

    (void) gobj;
    if (pass != 2 || !planning) {
        return;
    }
    hsd_80391A04(1.0f, 1.0f, 1);
    DrawRectangle(12.0f, -470.0f, 616.0f, 250.0f, &panel);
    DrawRectangle(24.0f, -228.0f, 592.0f, 2.0f, &rule);
}

static void addPlanLine(int index, float x, float y, float scale)
{
    PlanLine* line = &plan_lines[index];

    line->text[0] = '\0';
    line->entry = HSD_SisLib_803A6B98(plan_text, x, y, "%s", " ");
    HSD_SisLib_803A7548(plan_text, line->entry, scale, scale);
    HSD_SisLib_803A74F0(plan_text, line->entry, &col_white);
}

static void ensurePlanUi(void)
{
    int canvas;
    int i;
    float y;

    if (plan_ui) {
        return;
    }
    if (lbLang_IsSavedLanguageUS()) {
        HSD_SisLib_803A62A0(0, "SdMenu.usd", "SIS_MenuData");
    } else {
        HSD_SisLib_803A62A0(0, "SdMenu.dat", "SIS_MenuData");
    }
    canvas = HSD_SisLib_803A611C(0, NULL, 9, 0xD, 0, 0xE, 0, 25);
    plan_panel = GObj_Create(9, 0xD, 0);
    if (plan_panel != NULL) {
        GObj_SetupGXLink(plan_panel, drawPlanPanel, 0xE, 0);
    }
    plan_text = HSD_SisLib_803A6754(0, canvas);
    if (plan_text == NULL) {
        pc_log_line("tactics: planning overlay failed");
        return;
    }
    plan_text->default_kerning = 1;
    addPlanLine(LINE_TITLE, 36.0f, 232.0f, 0.5f);
    addPlanLine(LINE_SUB, 36.0f, 260.0f, 0.4f);
    for (i = 0; i < OPT_CAP; i++) {
        y = 292.0f + (float) i * 20.0f;
        addPlanLine(LINE_OPT + i, 36.0f, y, 0.42f);
    }
    addPlanLine(LINE_HINT, 36.0f, 458.0f, 0.36f);
    plan_ui = true;
}

static void destroyPlanUi(void)
{
    if (plan_text != NULL) {
        HSD_SisLib_803A5CC4(plan_text);
        plan_text = NULL;
    }
    if (plan_panel != NULL) {
        HSD_GObjFree(plan_panel);
        plan_panel = NULL;
    }
    plan_ui = false;
    memset(plan_lines, 0, sizeof(plan_lines));
}

static void showPlan(void)
{
    char buf[64];
    char label[64];
    int who = plan_port;
    int foe = who ^ 1;
    int i;

    ensurePlanUi();
    if (!plan_ui) {
        return;
    }
    /* Online, with nothing (left) to choose here: the other side is still
     * picking. */
    if (online && (who < 0 || opt_locked[who])) {
        who = tactics_NetLocalPort();
        snprintf(buf, sizeof(buf), "P%d %s  %d%%", who + 1,
                 tactics_FighterName(draft[who].ckind), portPercent(who));
        setPlanLine(&plan_lines[LINE_TITLE], buf);
        setPlanColor(&plan_lines[LINE_TITLE], &col_gold);
        setPlanLine(&plan_lines[LINE_SUB], tactics_NetClosed() ? tactics_NetStatus()
                                           : opt_locked[who] && choosing[who]
                                               ? "Locked in. Waiting for the other player..."
                                               : "The other player is choosing...");
        setPlanColor(&plan_lines[LINE_SUB], &col_white);
        for (i = 0; i < OPT_CAP; i++) {
            setPlanLine(&plan_lines[LINE_OPT + i], "");
        }
        setPlanLine(&plan_lines[LINE_HINT], "");
        return;
    }
    snprintf(buf, sizeof(buf), "P%d %s  %d%%", who + 1, tactics_FighterName(draft[who].ckind),
             portPercent(who));
    setPlanLine(&plan_lines[LINE_TITLE], buf);
    setPlanColor(&plan_lines[LINE_TITLE], &col_gold);
    /* Only the chooser's own list is shown. The other side's pick stays
     * hidden until the exchange plays. */
    if (react_port == who) {
        snprintf(buf, sizeof(buf), "Out of hitstun! React!");
    } else if (react_port == foe) {
        snprintf(buf, sizeof(buf), "Chasing P%d %s %d%%. Go for it!", foe + 1,
                 tactics_FighterName(draft[foe].ckind), portPercent(foe));
    } else {
        snprintf(buf, sizeof(buf), "vs P%d %s  %d%%%s", foe + 1,
                 tactics_FighterName(draft[foe].ckind), portPercent(foe),
                 !choosing[foe] ? ""
                 : online       ? (tactics_NetTheyCommitted() ? "  (locked in)" : "")
                 : !p2_cpu && opt_locked[foe] ? "  (locked in)" : "");
    }
    setPlanLine(&plan_lines[LINE_SUB], buf);
    setPlanColor(&plan_lines[LINE_SUB], &col_white);
    for (i = 0; i < OPT_CAP; i++) {
        PlanLine* line = &plan_lines[LINE_OPT + i];

        if (i < opt_n[who]) {
            optionLabel(label, sizeof(label), draft[who].ckind, &opts[who][i]);
            snprintf(buf, sizeof(buf), "%s%s", i == opt_cursor[who] ? "> " : "  ", label);
            setPlanLine(line, buf);
            setPlanColor(line, i == opt_cursor[who] ? &col_white : &col_dim);
        } else {
            setPlanLine(line, "");
        }
    }
    if (online) {
        setPlanLine(&plan_lines[LINE_HINT], "Up/Down choose    A or START lock in");
    } else if (p2_cpu || !choosing[foe]) {
        setPlanLine(&plan_lines[LINE_HINT], "Up/Down choose    A or START play");
    } else {
        snprintf(buf, sizeof(buf), "Up/Down choose    A or START lock in    P%d look away",
                 foe + 1);
        setPlanLine(&plan_lines[LINE_HINT], buf);
    }
    setPlanColor(&plan_lines[LINE_HINT], &col_dim);
}

static void hidePlanText(void)
{
    int i;

    if (!plan_ui) {
        return;
    }
    for (i = 0; i < LINE_COUNT; i++) {
        setPlanLine(&plan_lines[i], "");
    }
}

static void closeFight(void)
{
    live = false;
    planning = false;
    react_port = -1;
    settle = 0;
    destroyPlanUi();
}

static void queueOption(int which, const Opt* opt)
{
    TacticsLoadout next = draft[which];
    char label[64];

    next.moves[0] = opt->a;
    next.moves[1] = opt->b;
    next.count = opt->a == TM_NONE ? 0 : opt->b == TM_NONE ? 1 : 2;
    draft[which] = next;
    tactics_SetLoadout(which, &next);
    optionLabel(label, sizeof(label), next.ckind, opt);
    pc_log_line("tactics: choose P%d %s", which + 1, label);
}

/* The CPU decides only when the exchange starts, so its row is never on
 * screen. The lower of two rolls leans toward the top rows, which are the
 * preferred ones. */
static int cpuPick(int n)
{
    int a = (int) (tactics_SyncRand() % (u32) n);
    int b = (int) (tactics_SyncRand() % (u32) n);

    return a < b ? a : b;
}

/* Only the choosers' queues change. A fighter left out of the break keeps
 * playing what it was doing. */
static void commitPlan(void)
{
    int p;

    if (p2_cpu && choosing[1] && opt_n[1] > 0) {
        opt_cursor[1] = cpuPick(opt_n[1]);
    }
    /* A scripted run exercises the whole list, not just the top row. Online,
     * each side already chose its own. */
    if (!online && auto_resume && choosing[0] && opt_n[0] > 0) {
        opt_cursor[0] = cpuPick(opt_n[0]);
    }
    for (p = 0; p < 2; p++) {
        int pick = opt_cursor[p];

        if (!choosing[p] || opt_n[p] <= 0) {
            continue;
        }
        if (pick < 0 || pick >= opt_n[p]) {
            pick = 0;
        }
        queueOption(p, &opts[p][pick]);
        tactics_RestartQueue(p);
    }
    planning = false;
    react_port = -1;
    hidePlanText();
}

/* The next human chooser still to lock in, or -1. */
static int nextChooser(void)
{
    int p;

    for (p = 0; p < 2; p++) {
        if (choosing[p] && !opt_locked[p]) {
            return p;
        }
    }
    return -1;
}

/* launched: the launched port at an air break, or -1 for a normal break.
 * picks: which ports choose. With no human choosing there is no pause at
 * all; the CPU picks and play goes on. */
static void openPlan(int launched, const bool* picks)
{
    int p;

    react_port = launched;
    for (p = 0; p < 2; p++) {
        choosing[p] = picks[p];
        opt_n[p] = 0;
        opt_cursor[p] = 0;
        /* Online, the other side's port is chosen over there. */
        opt_locked[p] = !picks[p] || (p == 1 && p2_cpu) ||
                        (online && p != tactics_NetLocalPort());
        if (!picks[p]) {
            continue;
        }
        if (launched < 0) {
            opt_n[p] = buildOptions(p, opts[p], OPT_CAP);
        } else if (p == launched) {
            opt_n[p] = buildReactOptions(p, opts[p], OPT_CAP);
        } else {
            opt_n[p] = buildChaseOptions(p, opts[p], OPT_CAP);
        }
    }
    plan_frames = 0;
    settle = 0;
    idle = 0;
    since_plan = 0;
    {
        Fighter* a = portFighter(0);
        Fighter* b = portFighter(1);

        pc_log_line("tactics: planning p1=%d p2=%d picks=%d%d launched=%d gap=%.0f,%.0f",
                    portPercent(0), portPercent(1), picks[0], picks[1], launched,
                    a && b ? b->cur_pos.x - a->cur_pos.x : 0.0f,
                    a && b ? b->cur_pos.y - a->cur_pos.y : 0.0f);
    }
    brk_sum = tactics_SyncChecksum();
    p = tactics_SyncBreak();
    plan_port = nextChooser();
    /* Online, a break always waits for the other side's reveal, even when
     * there is nothing to choose here. */
    if (online) {
        tactics_NetBreak(p);
        if (plan_port < 0) {
            tactics_NetSendPick(-1, brk_sum);
        }
        planning = true;
        showPlan();
        return;
    }
    if (plan_port < 0) {
        commitPlan();
        return;
    }
    planning = true;
    showPlan();
}

/* A break online: this side picks for its own port, then both wait for the
 * other side's reveal (tacticsnet.c). */
static void netPlanFrame(void)
{
    int local = tactics_NetLocalPort();
    int theirs;

    if (tactics_NetClosed()) {
        if (auto_resume) {
            pc_log_line("tactics: scripted run lost the other player, exiting");
            exit(3);
        }
        showPlan();
        return;
    }
    if (plan_port == local && !opt_locked[local]) {
        u64 keys = gm_GetButtonsTriggered(4);
        bool lock = (keys & (PAD_CONFIRM | PAD_BUTTON_START)) != 0;

        if (opt_n[local] > 0 && (keys & PAD_ANY_UP)) {
            opt_cursor[local] = (opt_cursor[local] + opt_n[local] - 1) % opt_n[local];
        }
        if (opt_n[local] > 0 && (keys & PAD_ANY_DOWN)) {
            opt_cursor[local] = (opt_cursor[local] + 1) % opt_n[local];
        }
        /* A scripted run picks at random, from this machine's own rand():
         * the shared generator must advance the same on both sides. */
        if (auto_resume && plan_frames >= auto_frames && opt_n[local] > 0) {
            opt_cursor[local] = rand() % opt_n[local];
            lock = true;
        }
        if (lock) {
            opt_locked[local] = true;
            tactics_NetSendPick(opt_cursor[local], brk_sum);
        }
    }
    if (tactics_NetTheirPick(&theirs)) {
        int remote = local ^ 1;

        if (choosing[remote]) {
            opt_cursor[remote] = theirs >= 0 && theirs < opt_n[remote] ? theirs : 0;
        }
        commitPlan();
        return;
    }
    showPlan();
}

bool tactics_IsPlanning(void)
{
    return planning;
}

static void matchFrame(void);

void tactics_MatchFrame(void)
{
    s32 t;

    if (!live) {
        return;
    }
    matchFrame();
    /* Last, so the reseed is what the fighters' procs draw from this tick. */
    tactics_SyncFrame(planning);
    t = tactics_SyncTick();
    if (t >= 0 && t < 400 && !planning && getenv("MELEE_TACTICS_DEBUG") != NULL) {
        Fighter* a = portFighter(0);
        Fighter* b = portFighter(1);

        pc_log_line("tactics: tick=%d sum=%08X p1=%d,%.2f p2=%d,%.2f", t,
                    tactics_SyncChecksum(), a ? a->motion_id : -1, a ? a->cur_pos.x : 0.0f,
                    b ? b->motion_id : -1, b ? b->cur_pos.x : 0.0f);
    }
}

static void matchFrame(void)
{
    u64 keys;
    int who;

    /* A knockout ends the match. Drop the menu so
     * the victory sequence can play. */
    if (gm_GetMatchOutcome() != OUTCOME_NONE) {
        if (planning) {
            planning = false;
            react_port = -1;
            hidePlanText();
        }
        settle = 0;
        return;
    }
    if (!planning) {
        bool picks[2] = { false, false };
        int launched = tactics_AirBreak(picks);

        /* A scripted run that stops making decisions is a bug: say where it
         * stuck and quit rather than leave a window sitting there. */
        if (auto_resume && ++since_plan >= SCRIPT_STALL_FRAMES) {
            Fighter* a = portFighter(0);
            Fighter* b = portFighter(1);

            pc_log_line("tactics: scripted run stalled: P1 state=%d x=%.1f y=%.1f  "
                        "P2 state=%d x=%.1f y=%.1f",
                        a ? a->motion_id : -1, a ? a->cur_pos.x : 0.0f, a ? a->cur_pos.y : 0.0f,
                        b ? b->motion_id : -1, b ? b->cur_pos.x : 0.0f, b ? b->cur_pos.y : 0.0f);
            exit(2);
        }
        if (launched >= 0) {
            openPlan(launched, picks);
            return;
        }
        /* Nobody should ever stand around: both free with nothing queued
         * for a moment is a break, whatever the spacing. */
        idle = tactics_BothIdle() ? idle + 1 : 0;
        if (idle >= IDLE_BREAK) {
            picks[0] = picks[1] = true;
            openPlan(-1, picks);
            return;
        }
        if (tactics_BreakInAction()) {
            if (++settle >= PLAN_SETTLE) {
                picks[0] = picks[1] = true;
                openPlan(-1, picks);
            }
        } else {
            settle = 0;
        }
        return;
    }

    plan_frames++;
    if (online) {
        netPlanFrame();
        return;
    }
    who = plan_port;
    keys = gm_GetButtonsTriggered(4);
    if (opt_n[who] > 0 && (keys & PAD_ANY_UP)) {
        opt_cursor[who] = (opt_cursor[who] + opt_n[who] - 1) % opt_n[who];
    }
    if (opt_n[who] > 0 && (keys & PAD_ANY_DOWN)) {
        opt_cursor[who] = (opt_cursor[who] + 1) % opt_n[who];
    }
    if (auto_resume && plan_frames >= auto_frames) {
        commitPlan();
        return;
    }
    if (keys & (PAD_CONFIRM | PAD_BUTTON_START)) {
        opt_locked[who] = true;
        plan_port = nextChooser();
        if (plan_port < 0) {
            commitPlan();
            return;
        }
    }
    showPlan();
}

static void enterBattle(GameModeState* state)
{
    StartMeleeData* s = gm_GetGameModeStateEnterData(state);
    bool scripted = getenv("MELEE_TACTICS_AUTOSTART") != NULL;
    int i;

    /* Online, the lobby settled the fighters and the seed. */
    online = online_agreed;
    if (online) {
        draft[0].ckind = online_ckind[0];
        draft[1].ckind = online_ckind[1];
        p2_cpu = false;
    }
    /* A scripted run can pick its fighters by character kind number. */
    for (i = 0; i < 2 && !online; i++) {
        const char* pick = getenv(i == 0 ? "MELEE_TACTICS_P1" : "MELEE_TACTICS_P2");
        int ckind = pick != NULL ? atoi(pick) : -1;

        if (pick != NULL && ckind >= 0 && ckind < CKind_Playable_Count &&
            ckind != CKind_PopoNana)
        {
            draft[i].ckind = ckind;
        }
    }
    gm_SetupRulesDefaults(&s->rules);
    s->rules.stkind = St_Kind_Last;
    s->rules.match_kind = MatchKind_Stock;
    s->rules.is_stock = true;
    /* No clock, scripted or not: play continues until someone is knocked
     * out. */
    s->rules.timer_enabled = false;
    s->rules.timer_counts_up = false;
    s->rules.time_limit = 0;
    s->rules.item_freq = -1;
    s->rules.is_teams = false;
    s->rules.disable_pausing = true;
    for (i = 0; i < GM_MAX_PLAYERS; i++) {
        gm_SetupPlayerDefaults(&s->players[i]);
        s->players[i].slot_type = i < 2 ? Gm_PKind_Cpu : Gm_PKind_NA;
        if (i < 2) {
            s->players[i].ckind = draft[i].ckind;
            s->players[i].stocks = 1;
            s->players[i].cpu_kind = 4;
            s->players[i].cpu_level = 9;
            draft[i].count = 0;
            tactics_SetLoadout(i, &draft[i]);
        }
    }
    live = true;
    idle = 0;
    since_plan = 0;
    planning = false;
    settle = 0;
    auto_resume = scripted;
    if (getenv("MELEE_TACTICS_AUTO_FRAMES") != NULL) {
        auto_frames = atoi(getenv("MELEE_TACTICS_AUTO_FRAMES"));
    }
    /* Online the match is synced from the lobby's seed. MELEE_TACTICS_SEED
     * plays an offline match the same way, for testing; otherwise offline play
     * keeps the machine-seeded retail behaviour. */
    if (online) {
        tactics_SyncBegin(online_seed, true);
    } else {
        tactics_SyncBegin(getenv("MELEE_TACTICS_SEED") != NULL
                              ? (u32) strtoul(getenv("MELEE_TACTICS_SEED"), NULL, 0)
                              : 0,
                          getenv("MELEE_TACTICS_SEED") != NULL);
    }
    tactics_BeginMatch();
    gm_LoadAnnouncer();
    pc_log_line("tactics: battle %s vs %s", tactics_FighterName(draft[0].ckind),
                tactics_FighterName(draft[1].ckind));
}

static void exitBattle(GameModeState* state)
{
    MatchEnd* end = &((MatchExitInfo*) gm_GetGameModeStateExitData(state))->match_end;

    if (gm_WasMatchCanceled(end->outcome)) {
        snprintf(result, sizeof(result), "Battle cancelled.");
    } else if (end->n_winners == 1 && end->winners[0] < 2) {
        snprintf(result, sizeof(result), "P%d %s wins!", end->winners[0] + 1,
                 tactics_FighterName(draft[end->winners[0]].ckind));
    } else {
        snprintf(result, sizeof(result), "Draw!");
    }
    pc_log_line("tactics: result %s frames=%u", result, end->frame_count);
    tactics_SyncEnd();
    /* An online match is one session: back in the menus it has ended. */
    if (online) {
        tactics_NetLeave();
        after_online = true;
    }
    online = online_agreed = online_ready = false;
    closeFight();
    tactics_ClearLoadouts();
    gm_SetNextGameModeStateId(0);
}

GameModeState gm_Mode_Tactics_States[] = {
    { 0, 3, 0, NULL, NULL, { GS_TACTICS_DRAFT, NULL, NULL } },
    { 1, 2, 0, enterBattle, exitBattle, { GS_VS, &gmVsMelee_StartData, &gmVsMelee_VsExitInfo } },
    { GM_GAMEMODESTATE_TERMINATE }
};

/* ---- the menus ---------------------------------------------------------
 * Everything before a fight happens here, in game: the main menu, the draft
 * against the CPU, and online play (host a lobby, or pick one from the list
 * of open lobbies, then both players ready up). All of it is drawn with the
 * native lobby view.
 *
 * A scripted run (MELEE_TACTICS_AUTOSTART) drives the same menus: with
 * MELEE_TACTICS_ONLINE=host it opens a lobby, with =join it joins the first
 * one listed, otherwise it fights the CPU. MELEE_TACTICS_P1 is its fighter and
 * MELEE_TACTICS_READY_FRAME when it readies in a lobby. */

typedef enum MenuScreen {
    SCR_MAIN,
    SCR_CPU,
    SCR_ONLINE,
    SCR_FIND,
    SCR_LOBBY,
} MenuScreen;

enum {
    MAIN_ROWS = 2,
    ONLINE_ROWS = 3,
    LOBBY_LIST_MAX = 7,
    LOBBY_REFRESH_FRAMES = 90,
};

static MenuScreen screen;
static int screen_frames;
static int my_ckind = CKind_Ganon;
static PcLinkLobby found[LOBBY_LIST_MAX];
static int found_n = -1;
static char menu_note[96];

static void goScreen(MenuScreen next)
{
    screen = next;
    screen_frames = 0;
    cursor = 0;
}

static const char* scripted(void)
{
    return getenv("MELEE_TACTICS_AUTOSTART");
}

static const char* scriptedOnline(void)
{
    return scripted() ? getenv("MELEE_TACTICS_ONLINE") : NULL;
}

void tactics_DraftEnter(void* unused)
{
    const char* mine = getenv("MELEE_TACTICS_P1");

    (void) unused;
    /* A scripted run is one match. Back here, it is over; the exit handlers
     * shut the port down. */
    if (auto_started && scripted() != NULL) {
        pc_log_line("tactics: scripted match done, exiting");
        exit(0);
    }
    if (mine != NULL && atoi(mine) >= 0 && atoi(mine) < CKind_Playable_Count &&
        atoi(mine) != CKind_PopoNana)
    {
        my_ckind = atoi(mine);
    }
    frames = 0;
    live = false;
    planning = false;
    /* After an online match, the online menu, so another is one step away. */
    goScreen(after_online ? SCR_ONLINE : SCR_MAIN);
    if (after_online) {
        snprintf(menu_note, sizeof menu_note, "%s", result);
    }
    after_online = false;
    mnOnlineLobby_Create();
    pc_log_line("tactics: menus opened");
}

void tactics_DraftExit(void* unused)
{
    (void) unused;
    mnOnlineLobby_Destroy();
}

static void cycleFighter(int which, int delta)
{
    do {
        draft[which].ckind =
            (draft[which].ckind + CKind_Playable_Count + delta) % CKind_Playable_Count;
    } while (draft[which].ckind == CKind_PopoNana);
}

static void cycleMine(int delta)
{
    do {
        my_ckind = (my_ckind + CKind_Playable_Count + delta) % CKind_Playable_Count;
    } while (my_ckind == CKind_PopoNana);
}

static void moveCursor(u64 keys, int rows)
{
    if (rows <= 0) {
        cursor = 0;
        return;
    }
    if (keys & PAD_ANY_UP) {
        cursor = (cursor + rows - 1) % rows;
    }
    if (keys & PAD_ANY_DOWN) {
        cursor = (cursor + 1) % rows;
    }
}

static void startFight(void)
{
    auto_started = true;
    gm_801A4B60();
}

/* The internal resolution steps the main menu offers; 0 is "match the
 * window" (native only, where the launcher can leave it there). */
static const float render_steps[] = { 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
#define RENDER_STEPS ((int) (sizeof render_steps / sizeof render_steps[0]))

static void stepRenderScale(int delta)
{
    float now = pc_render_scale();
    int i, at = 0;

    for (i = 0; i < RENDER_STEPS; i++) {
        if (render_steps[i] <= now + 0.01f) {
            at = i;
        }
    }
    at = (at + delta + RENDER_STEPS) % RENDER_STEPS;
    pc_set_render_scale(render_steps[at]);
    pc_log_line("tactics: resolution %.1fx", render_steps[at]);
}

static void mainMenu(u64 keys, OnlineLobbyView* view)
{
    bool has_online = tactics_NetAvailable();
    int res_row = has_online ? 2 : 1;
    int rows = res_row + 1;
    int delta = (keys & PAD_ANY_RIGHT) ? 1 : (keys & PAD_ANY_LEFT) ? -1 : 0;
    bool pick = (keys & (PAD_CONFIRM | PAD_BUTTON_START)) != 0;
    float scale;

    moveCursor(keys, rows);
    if (scripted() != NULL && screen_frames == 30) {
        cursor = scriptedOnline() != NULL ? 1 : 0;
        pick = true;
    }
    /* The resolution row changes with Left/Right (or A), and stays here. */
    if (cursor == res_row && (delta != 0 || pick)) {
        stepRenderScale(delta != 0 ? delta : 1);
        pick = false;
    }
    if (pick) {
        menu_note[0] = '\0';
        goScreen(cursor == 0 ? SCR_CPU : SCR_ONLINE);
        return;
    }
    if (keys & PAD_CANCEL) {
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        gm_801A4B60();
        return;
    }
    scale = pc_render_scale();
    view->title = "MELEE TACTICS";
    snprintf(view->subtitle, sizeof view->subtitle, "1 stock, until a knockout");
    snprintf(view->menu[0], sizeof view->menu[0], "VS CPU");
    snprintf(view->menu[1], sizeof view->menu[1], "ONLINE");
    if (scale <= 0.0f) {
        snprintf(view->menu[res_row], sizeof view->menu[res_row], "RESOLUTION: Auto");
    } else {
        snprintf(view->menu[res_row], sizeof view->menu[res_row], "RESOLUTION: %gx (%dx%d)", scale,
                 (int) (640 * scale), (int) (480 * scale));
    }
    view->menu_count = rows;
    snprintf(view->message, sizeof view->message, "%s",
             cursor == res_row ? "Lower it if the game runs choppy." : result);
    view->hint = cursor == res_row ? "Left/Right change   B back" : "Up/Down choose   A select";
}

/* The draft against the CPU, or against a second player on this machine. */
static void cpuMenu(u64 keys, OnlineLobbyView* view)
{
    int delta = (keys & PAD_ANY_RIGHT) ? 1 : (keys & PAD_ANY_LEFT) ? -1 : 0;

    moveCursor(keys, MENU_ROWS);
    if (delta != 0 && (cursor == 0 || cursor == 1)) {
        cycleFighter(cursor, delta);
    }
    if ((delta != 0 || (keys & PAD_BUTTON_X)) && cursor == 2) {
        p2_cpu = !p2_cpu;
    }
    if (keys & PAD_CANCEL) {
        goScreen(SCR_MAIN);
        return;
    }
    if (((keys & PAD_CONFIRM) && cursor == 3) || (keys & PAD_BUTTON_START) ||
        (scripted() != NULL && screen_frames == 90))
    {
        startFight();
        return;
    }
    view->title = "VS CPU";
    snprintf(view->subtitle, sizeof view->subtitle, "1 stock, until a knockout");
    snprintf(view->menu[0], sizeof view->menu[0], "P1: %s", tactics_FighterName(draft[0].ckind));
    snprintf(view->menu[1], sizeof view->menu[1], "P2: %s", tactics_FighterName(draft[1].ckind));
    snprintf(view->menu[2], sizeof view->menu[2], "P2 plays: %s", p2_cpu ? "CPU" : "Human");
    snprintf(view->menu[3], sizeof view->menu[3], "FIGHT");
    view->menu_count = MENU_ROWS;
    snprintf(view->message, sizeof view->message, "%s", result);
    view->hint = "Left/Right change   START fight   B back";
}

static void onlineMenu(u64 keys, OnlineLobbyView* view)
{
    int delta = (keys & PAD_ANY_RIGHT) ? 1 : (keys & PAD_ANY_LEFT) ? -1 : 0;
    bool pick = (keys & (PAD_CONFIRM | PAD_BUTTON_START)) != 0;

    moveCursor(keys, ONLINE_ROWS);
    if (delta != 0 && cursor == 0) {
        cycleMine(delta);
    }
    if (scriptedOnline() != NULL && screen_frames == 30) {
        cursor = strcmp(scriptedOnline(), "host") == 0 ? 1 : 2;
        pick = true;
    }
    if (keys & PAD_CANCEL) {
        goScreen(SCR_MAIN);
        return;
    }
    if (pick && cursor == 1) {
        char name[PC_LINK_NAME_LEN];

        snprintf(name, sizeof name, "%s", tactics_FighterName(my_ckind));
        online_ready = false;
        tactics_NetHost(name);
        goScreen(SCR_LOBBY);
        return;
    }
    if (pick && cursor == 2) {
        found_n = -1;
        pc_link_refresh();
        goScreen(SCR_FIND);
        return;
    }
    view->title = "ONLINE";
    snprintf(view->subtitle, sizeof view->subtitle, "Play against another player");
    snprintf(view->menu[0], sizeof view->menu[0], "Your fighter: %s", tactics_FighterName(my_ckind));
    snprintf(view->menu[1], sizeof view->menu[1], "CREATE LOBBY");
    snprintf(view->menu[2], sizeof view->menu[2], "FIND A LOBBY");
    view->menu_count = ONLINE_ROWS;
    snprintf(view->message, sizeof view->message, "%s", menu_note);
    view->hint = "Left/Right fighter   A select   B back";
}

/* The open lobbies, from the page server; refreshed while this is up. */
static void findMenu(u64 keys, OnlineLobbyView* view)
{
    int i;

    if (screen_frames % LOBBY_REFRESH_FRAMES == 0) {
        pc_link_refresh();
    }
    found_n = pc_link_lobbies(found, LOBBY_LIST_MAX);
    moveCursor(keys, found_n);
    if (keys & PAD_CANCEL) {
        goScreen(SCR_ONLINE);
        return;
    }
    if (found_n > 0 && ((keys & (PAD_CONFIRM | PAD_BUTTON_START)) || scriptedOnline() != NULL)) {
        if (cursor >= found_n) {
            cursor = 0;
        }
        online_ready = false;
        tactics_NetJoin(found[cursor].room);
        goScreen(SCR_LOBBY);
        return;
    }
    view->title = "FIND A LOBBY";
    snprintf(view->subtitle, sizeof view->subtitle, "Your fighter: %s", tactics_FighterName(my_ckind));
    for (i = 0; i < found_n && i < LOBBY_LIST_MAX; i++) {
        snprintf(view->menu[i], sizeof view->menu[i], "%s", found[i].name);
    }
    view->menu_count = found_n > 0 ? found_n : 0;
    snprintf(view->message, sizeof view->message, "%s",
             found_n < 0    ? "Looking for lobbies..."
             : found_n == 0 ? "No open lobbies yet. Create one, or wait here."
                            : "");
    view->hint = "Up/Down choose   A join   B back";
}

/* A lobby, hosted or joined: pick a fighter, READY, and the match starts
 * once both players are ready. */
static void lobbyMenu(u64 keys, OnlineLobbyView* view)
{
    int delta = (keys & PAD_ANY_RIGHT) ? 1 : (keys & PAD_ANY_LEFT) ? -1 : 0;
    const char* ready_at = getenv("MELEE_TACTICS_READY_FRAME");

    if (keys & PAD_CANCEL) {
        pc_log_line("tactics: left the lobby");
        tactics_NetLeave();
        online_ready = false;
        goScreen(SCR_ONLINE);
        return;
    }
    if (!online_ready) {
        bool ready;

        moveCursor(keys, 2);
        if (delta != 0 && cursor == 0) {
            cycleMine(delta);
        }
        ready = ((keys & PAD_CONFIRM) && cursor == 1) || (keys & PAD_BUTTON_START);
        /* A scripted run readies at MELEE_TACTICS_READY_FRAME, or as soon as
         * the other player is here. */
        if (scriptedOnline() != NULL) {
            ready = ready_at != NULL ? screen_frames >= atoi(ready_at) : true;
        }
        if (ready && tactics_NetConnected()) {
            online_ready = true;
        }
    }
    if (online_ready) {
        int c1, c2;

        if (tactics_NetLobby(my_ckind, &online_seed, &c1, &c2)) {
            online_ckind[0] = c1;
            online_ckind[1] = c2;
            online_agreed = true;
            startFight();
            return;
        }
    } else {
        tactics_NetPoll();
    }
    view->title = tactics_NetLocalPort() == 0 ? "YOUR LOBBY" : "LOBBY";
    snprintf(view->subtitle, sizeof view->subtitle, "You are P%d", tactics_NetLocalPort() + 1);
    snprintf(view->menu[0], sizeof view->menu[0], "Your fighter: %s", tactics_FighterName(my_ckind));
    snprintf(view->menu[1], sizeof view->menu[1], "%s",
             online_ready             ? "READY - waiting for the other player"
             : tactics_NetConnected() ? "READY"
                                      : "READY (once someone joins)");
    view->menu_count = 2;
    if (online_ready) {
        view->cursor = -1;
    }
    snprintf(view->message, sizeof view->message, "%s", tactics_NetStatus());
    view->hint = "Left/Right fighter   A ready   B leave";
}

void tactics_DraftFrame(void)
{
    u64 keys = gm_GetButtonsTriggered(4);
    OnlineLobbyView view = { 0 };

    frames++;
    screen_frames++;
    view.screen = LOBBY_SCREEN_MENU;
    switch (screen) {
    case SCR_MAIN:
        mainMenu(keys, &view);
        break;
    case SCR_CPU:
        cpuMenu(keys, &view);
        break;
    case SCR_ONLINE:
        onlineMenu(keys, &view);
        break;
    case SCR_FIND:
        findMenu(keys, &view);
        break;
    case SCR_LOBBY:
        lobbyMenu(keys, &view);
        break;
    }
    if (view.title == NULL) {
        return; /* the screen changed or the scene is leaving */
    }
    if (view.cursor != -1) {
        view.cursor = cursor;
    }
    mnOnlineLobby_Update(&view);
}
