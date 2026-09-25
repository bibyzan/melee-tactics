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
#include <pc/pc.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/hsd_3915.h>
#include <sysdolphin/baselib/sislib.h>

#define PLAN_SETTLE 10
#define PLAN_AUTO_FRAMES 45
#define MENU_ROWS 4
#define OPT_CAP 8

static TacticsLoadout draft[2] = {
    { CKind_Fox, { TM_NONE, TM_NONE }, 0 },
    { CKind_Mars, { TM_NONE, TM_NONE }, 0 },
};

static int cursor, frames;
static bool auto_started;
static bool live, planning, auto_resume;
static bool p2_cpu = true;
static int settle, plan_port, plan_frames;
static char result[96] = "Pick a 2-move exchange when the fight pauses. P2 can play itself.";

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

/* Preference order. The first recipes that match the freeze are the rows. */
static const Recipe recipes[] = {
    { TM_DTILT, TM_UTILT, 0, D_CLOSE, V_LEVEL | V_ABOVE, P_COMBO },
    { TM_DTILT, TM_UAIR, 0, D_CLOSE, V_LEVEL | V_ABOVE, P_COMBO },
    { TM_UTILT, TM_UAIR, 0, D_CLOSE, V_LEVEL | V_ABOVE, P_COMBO },
    { TM_JAB, TM_FTILT, 0, D_CLOSE, V_LEVEL, P_COMBO },
    { TM_UTHROW, TM_NONE, 0, D_CLOSE, V_LEVEL | V_ABOVE, P_COMBO },
    { TM_NAIR, TM_FAIR, 0, D_CLOSE | D_MID, V_LEVEL, P_COMBO },
    { TM_FSMASH, TM_NONE, 0, D_CLOSE, V_LEVEL, P_KILL },
    { TM_USMASH, TM_NONE, 0, D_CLOSE, V_LEVEL | V_ABOVE, P_KILL },
    { TM_BTHROW, TM_NONE, 0, D_CLOSE, V_LEVEL, P_KILL },
    { TM_FAIR, TM_NONE, 0, D_CLOSE | D_MID, V_LEVEL, P_COMBO | P_KILL },
    { TM_BAIR, TM_NONE, 0, D_CLOSE | D_MID, V_LEVEL, P_KILL },
    { TM_UAIR, TM_NONE, 0, D_CLOSE, V_ABOVE, P_COMBO | P_KILL },
    { TM_DAIR, TM_NONE, 0, D_CLOSE, V_BELOW, P_COMBO | P_KILL },
    { TM_DASH_ATTACK, TM_FTILT, 0, D_MID | D_FAR, V_LEVEL, P_COMBO },
    { TM_DASH_ATTACK, TM_NONE, 0, D_MID | D_FAR, V_LEVEL, P_COMBO | P_KILL },
    { TM_FTILT, TM_NONE, 0, D_CLOSE | D_MID, V_LEVEL, P_COMBO | P_KILL },
    { TM_NEUTRAL_B, TM_NONE, 0, D_MID | D_FAR, V_LEVEL | V_ABOVE | V_BELOW, P_COMBO | P_KILL },
    { TM_SIDE_B, TM_NONE, 0, D_MID | D_FAR, V_LEVEL, P_COMBO | P_KILL },
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
    dist = dx < 20.0f ? D_CLOSE : dx < 48.0f ? D_MID : D_FAR;
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
    int who = p2_cpu ? 0 : plan_port;
    int i;

    ensurePlanUi();
    if (!plan_ui) {
        return;
    }
    snprintf(buf, sizeof(buf), "P1 %s  %d%%", tactics_FighterName(draft[0].ckind),
             portPercent(0));
    setPlanLine(&plan_lines[LINE_TITLE], buf);
    setPlanColor(&plan_lines[LINE_TITLE], &col_gold);
    if (p2_cpu && opt_n[1] > 0) {
        optionLabel(label, sizeof(label), draft[1].ckind, &opts[1][opt_cursor[1]]);
        snprintf(buf, sizeof(buf), "P2 %s CPU: %s", tactics_FighterName(draft[1].ckind),
                 label);
    } else if (!p2_cpu) {
        snprintf(buf, sizeof(buf), "Choosing for P%d %s  %d%%", who + 1,
                 tactics_FighterName(draft[who].ckind), portPercent(who));
    } else {
        snprintf(buf, sizeof(buf), "P2 %s CPU", tactics_FighterName(draft[1].ckind));
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
    setPlanLine(&plan_lines[LINE_HINT], p2_cpu
                    ? "Up/Down choose    A or START play"
                    : "Up/Down choose    X other fighter    A or START lock");
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
    settle = 0;
    destroyPlanUi();
}

static void queueOption(int which, const Opt* opt)
{
    TacticsLoadout next = draft[which];
    char label[64];

    next.moves[0] = opt->a;
    next.moves[1] = opt->b;
    next.count = opt->b == TM_NONE ? 1 : 2;
    draft[which] = next;
    tactics_SetLoadout(which, &next);
    optionLabel(label, sizeof(label), next.ckind, opt);
    pc_log_line("tactics: choose P%d %s", which + 1, label);
}

static void commitPlan(void)
{
    int p;

    for (p = 0; p < 2; p++) {
        int pick = opt_cursor[p];

        if (opt_n[p] <= 0) {
            continue;
        }
        if (pick < 0 || pick >= opt_n[p]) {
            pick = 0;
        }
        queueOption(p, &opts[p][pick]);
    }
    tactics_RestartQueues();
    planning = false;
    hidePlanText();
    pc_log_line("tactics: resume");
}

static void openPlan(void)
{
    opt_n[0] = buildOptions(0, opts[0], OPT_CAP);
    opt_n[1] = buildOptions(1, opts[1], OPT_CAP);
    opt_cursor[0] = 0;
    opt_cursor[1] = 0;
    opt_locked[0] = false;
    opt_locked[1] = p2_cpu;
    plan_port = 0;
    plan_frames = 0;
    settle = 0;
    planning = true;
    pc_log_line("tactics: planning p1=%d p2=%d options=%d", portPercent(0),
                portPercent(1), opt_n[0]);
    showPlan();
}

bool tactics_IsPlanning(void)
{
    return planning;
}

void tactics_MatchFrame(void)
{
    u64 keys;
    int who;

    if (!live) {
        return;
    }
    /* A knockout or a scripted time limit ends the match. Drop the menu so
     * the victory sequence can play. */
    if (gm_GetMatchOutcome() != OUTCOME_NONE) {
        if (planning) {
            planning = false;
            hidePlanText();
        }
        settle = 0;
        return;
    }
    if (!planning) {
        if (tactics_BreakInAction()) {
            if (++settle >= PLAN_SETTLE) {
                openPlan();
            }
        } else {
            settle = 0;
        }
        return;
    }

    plan_frames++;
    who = p2_cpu ? 0 : plan_port;
    keys = gm_GetButtonsTriggered(4);
    if (opt_n[who] > 0 && (keys & PAD_ANY_UP)) {
        opt_cursor[who] = (opt_cursor[who] + opt_n[who] - 1) % opt_n[who];
    }
    if (opt_n[who] > 0 && (keys & PAD_ANY_DOWN)) {
        opt_cursor[who] = (opt_cursor[who] + 1) % opt_n[who];
    }
    if (!p2_cpu && (keys & PAD_BUTTON_X)) {
        plan_port = 1 - plan_port;
    }
    if (auto_resume && plan_frames >= PLAN_AUTO_FRAMES) {
        commitPlan();
        return;
    }
    if (keys & (PAD_CONFIRM | PAD_BUTTON_START)) {
        opt_locked[who] = true;
        if (p2_cpu || (opt_locked[0] && opt_locked[1])) {
            commitPlan();
            return;
        }
        plan_port = 1 - plan_port;
    }
    showPlan();
}

static void enterBattle(GameModeState* state)
{
    StartMeleeData* s = gm_GetGameModeStateEnterData(state);
    bool scripted = getenv("MELEE_TACTICS_AUTOSTART") != NULL;
    int i;

    gm_SetupRulesDefaults(&s->rules);
    s->rules.stkind = St_Kind_Last;
    s->rules.match_kind = MatchKind_Stock;
    s->rules.is_stock = true;
    /* A scripted run still needs the stock to end on its own. Play continues
     * until someone is knocked out. */
    s->rules.timer_enabled = scripted;
    s->rules.timer_counts_up = false;
    s->rules.time_limit = scripted ? 40 : 0;
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
    planning = false;
    settle = 0;
    auto_resume = scripted;
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
    closeFight();
    tactics_ClearLoadouts();
    gm_SetNextGameModeStateId(0);
}

GameModeState gm_Mode_Tactics_States[] = {
    { 0, 3, 0, NULL, NULL, { GS_TACTICS_DRAFT, NULL, NULL } },
    { 1, 2, 0, enterBattle, exitBattle, { GS_VS, &gmVsMelee_StartData, &gmVsMelee_VsExitInfo } },
    { GM_GAMEMODESTATE_TERMINATE }
};

void tactics_DraftEnter(void* unused)
{
    (void) unused;
    frames = 0;
    live = false;
    planning = false;
    mnOnlineLobby_Create();
    pc_log_line("tactics: draft opened");
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

void tactics_DraftFrame(void)
{
    u64 keys = gm_GetButtonsTriggered(4);
    OnlineLobbyView view = { 0 };
    int delta;

    frames++;
    if (keys & PAD_ANY_UP) {
        cursor = (cursor + MENU_ROWS - 1) % MENU_ROWS;
    }
    if (keys & PAD_ANY_DOWN) {
        cursor = (cursor + 1) % MENU_ROWS;
    }
    delta = (keys & PAD_ANY_RIGHT) ? 1 : (keys & PAD_ANY_LEFT) ? -1 : 0;
    if (delta != 0 && (cursor == 0 || cursor == 1)) {
        cycleFighter(cursor, delta);
    }
    if ((delta != 0 || (keys & PAD_BUTTON_X)) && cursor == 2) {
        p2_cpu = !p2_cpu;
    }
    if (keys & PAD_CANCEL) {
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        gm_801A4B60();
        return;
    }
    if (((keys & PAD_CONFIRM) && cursor == 3) || (keys & PAD_BUTTON_START) ||
        (frames == 120 && !auto_started && getenv("MELEE_TACTICS_AUTOSTART")))
    {
        auto_started = true;
        gm_801A4B60();
        return;
    }
    view.title = "MELEE TACTICS";
    view.screen = LOBBY_SCREEN_MENU;
    view.cursor = cursor;
    view.menu_count = MENU_ROWS;
    snprintf(view.subtitle, sizeof(view.subtitle), "1 stock, until a knockout");
    snprintf(view.menu[0], sizeof(view.menu[0]), "P1: %s",
             tactics_FighterName(draft[0].ckind));
    snprintf(view.menu[1], sizeof(view.menu[1]), "P2: %s",
             tactics_FighterName(draft[1].ckind));
    snprintf(view.menu[2], sizeof(view.menu[2]), "P2 plays: %s", p2_cpu ? "CPU" : "Human");
    snprintf(view.menu[3], sizeof(view.menu[3]), "FIGHT");
    snprintf(view.message, sizeof(view.message), "%s", result);
    view.hint = "Up/Down row   Left/Right change   START fight";
    mnOnlineLobby_Update(&view);
}
