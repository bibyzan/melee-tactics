#include "tactics.h"
#include "tacticsmode.h"

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

#define PLAN_ROWS 3
#define PLAN_SETTLE 10
#define PLAN_AUTO_FRAMES 45
#define MENU_ROWS 6

static TacticsLoadout draft[2] = {
    { CKind_Fox, { TM_DTILT, TM_UTILT, TM_NAIR }, 3 },
    { CKind_Mars, { TM_FTILT, TM_UTILT, TM_FAIR }, 3 },
};

static int cursor, port, frames;
static bool auto_started;
static bool live, planning, auto_resume;
static int settle, plan_row, plan_port, plan_frames;
static u8 plan_moves[2][TACTICS_SLOTS];
static char result[96] = "Queue three moves. The fight pauses between exchanges.";

enum {
    LINE_TITLE,
    LINE_LEFT_NAME,
    LINE_RIGHT_NAME,
    LINE_MOVES,
    LINE_HINT = LINE_MOVES + TACTICS_PORTS * TACTICS_SLOTS,
    LINE_COUNT
};

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

static bool selectable(int ckind, int move)
{
    return tactics_MoveAllowed(ckind, move) && move < TM_NEUTRAL_B;
}

static int cycleMove(int ckind, int move, int delta)
{
    int m = move;
    int i;

    if (m == TM_NONE) {
        m = delta > 0 ? -1 : TM_COUNT;
    }
    for (i = 0; i < TM_COUNT + 1; i++) {
        m += delta;
        if (m < 0 || m >= TM_COUNT) {
            return TM_NONE;
        }
        if (selectable(ckind, m)) {
            return m;
        }
    }
    return TM_NONE;
}

static const char* moveName(int ckind, int move)
{
    const TacticsMoveInfo* info;

    if (move == TM_NONE) {
        return "pass";
    }
    info = tactics_GetMove(ckind, move);
    return info != NULL ? info->name : "pass";
}

static bool anyQueued(void)
{
    int p, i;

    for (p = 0; p < 2; p++) {
        for (i = 0; i < TACTICS_SLOTS; i++) {
            if (plan_moves[p][i] != TM_NONE) {
                return true;
            }
        }
    }
    return false;
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
    DrawRectangle(12.0f, -468.0f, 616.0f, 164.0f, &panel);
    DrawRectangle(24.0f, -312.0f, 592.0f, 2.0f, &rule);
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
    int p, i;
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
    addPlanLine(LINE_TITLE, 36.0f, 308.0f, 0.55f);
    addPlanLine(LINE_LEFT_NAME, 36.0f, 338.0f, 0.42f);
    addPlanLine(LINE_RIGHT_NAME, 340.0f, 338.0f, 0.42f);
    for (p = 0; p < TACTICS_PORTS; p++) {
        for (i = 0; i < TACTICS_SLOTS; i++) {
            y = 366.0f + (float) i * 26.0f;
            addPlanLine(LINE_MOVES + p * TACTICS_SLOTS + i, p == 0 ? 36.0f : 340.0f, y,
                        0.42f);
        }
    }
    addPlanLine(LINE_HINT, 36.0f, 452.0f, 0.38f);
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
    int p, i;

    ensurePlanUi();
    if (!plan_ui) {
        return;
    }
    setPlanLine(&plan_lines[LINE_TITLE], "BREAK");
    setPlanColor(&plan_lines[LINE_TITLE], &col_gold);
    for (p = 0; p < 2; p++) {
        PlanLine* name = &plan_lines[p == 0 ? LINE_LEFT_NAME : LINE_RIGHT_NAME];

        snprintf(buf, sizeof(buf), "P%d %s  %d%%", p + 1,
                 tactics_FighterName(draft[p].ckind), portPercent(p));
        setPlanLine(name, buf);
        setPlanColor(name, p == plan_port ? &col_gold : &col_white);
        for (i = 0; i < TACTICS_SLOTS; i++) {
            PlanLine* line = &plan_lines[LINE_MOVES + p * TACTICS_SLOTS + i];
            bool here = p == plan_port && i == plan_row;

            snprintf(buf, sizeof(buf), "%s%d. %s", here ? "> " : "  ", i + 1,
                     moveName(draft[p].ckind, plan_moves[p][i]));
            setPlanLine(line, buf);
            setPlanColor(line, here ? &col_white : &col_dim);
        }
    }
    setPlanLine(&plan_lines[LINE_HINT],
                "Up/Down move   Left/Right change   X side   START resume");
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

static void commitPlan(void)
{
    int p, i;

    if (!anyQueued()) {
        return;
    }
    for (p = 0; p < 2; p++) {
        for (i = 0; i < TACTICS_SLOTS; i++) {
            draft[p].moves[i] = plan_moves[p][i];
        }
        draft[p].count = TACTICS_SLOTS;
        tactics_SetLoadout(p, &draft[p]);
    }
    tactics_RestartQueues();
    planning = false;
    hidePlanText();
    pc_log_line("tactics: resume");
}

static void openPlan(void)
{
    int p, i;

    for (p = 0; p < 2; p++) {
        for (i = 0; i < TACTICS_SLOTS; i++) {
            plan_moves[p][i] = draft[p].moves[i];
        }
    }
    plan_row = 0;
    plan_frames = 0;
    settle = 0;
    planning = true;
    pc_log_line("tactics: planning p1=%d p2=%d", portPercent(0), portPercent(1));
    showPlan();
}

bool tactics_IsPlanning(void)
{
    return planning;
}

void tactics_MatchFrame(void)
{
    u64 keys;
    int delta;

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
    keys = gm_GetButtonsTriggered(4);
    if (keys & PAD_ANY_UP) {
        plan_row = (plan_row + PLAN_ROWS - 1) % PLAN_ROWS;
    }
    if (keys & PAD_ANY_DOWN) {
        plan_row = (plan_row + 1) % PLAN_ROWS;
    }
    if (keys & PAD_BUTTON_X) {
        plan_port = 1 - plan_port;
    }
    delta = (keys & PAD_ANY_RIGHT) ? 1 : (keys & PAD_ANY_LEFT) ? -1 : 0;
    if (delta != 0) {
        plan_moves[plan_port][plan_row] =
            (u8) cycleMove(draft[plan_port].ckind, plan_moves[plan_port][plan_row], delta);
    }
    if (anyQueued() &&
        ((keys & (PAD_CONFIRM | PAD_BUTTON_START)) != 0 ||
         (auto_resume && plan_frames >= PLAN_AUTO_FRAMES)))
    {
        commitPlan();
        return;
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
            draft[i].count = TACTICS_SLOTS;
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
        snprintf(result, sizeof(result), "Battle cancelled. Edit the queue and try again.");
    } else if (end->n_winners == 1 && end->winners[0] < 2) {
        snprintf(result, sizeof(result), "P%d %s wins! Edit the queue or fight again.",
                 end->winners[0] + 1, tactics_FighterName(draft[end->winners[0]].ckind));
    } else {
        snprintf(result, sizeof(result), "Draw! Edit the queue or fight again.");
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

void tactics_DraftFrame(void)
{
    u64 keys = gm_GetButtonsTriggered(4);
    OnlineLobbyView view = { 0 };
    int delta;
    int i;

    frames++;
    if (keys & PAD_ANY_UP) {
        cursor = (cursor + MENU_ROWS - 1) % MENU_ROWS;
    }
    if (keys & PAD_ANY_DOWN) {
        cursor = (cursor + 1) % MENU_ROWS;
    }
    if (keys & PAD_BUTTON_X) {
        port = 1 - port;
    }
    delta = (keys & PAD_ANY_RIGHT) ? 1 : (keys & PAD_ANY_LEFT) ? -1 : 0;
    if (delta != 0 && cursor == 0) {
        do {
            draft[port].ckind =
                (draft[port].ckind + CKind_Playable_Count + delta) % CKind_Playable_Count;
        } while (draft[port].ckind == CKind_PopoNana);
    }
    if (delta != 0 && cursor >= 1 && cursor <= TACTICS_SLOTS) {
        draft[port].moves[cursor - 1] =
            (u8) cycleMove(draft[port].ckind, draft[port].moves[cursor - 1], delta);
    }
    if (keys & PAD_CANCEL) {
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        gm_801A4B60();
        return;
    }
    if (((keys & PAD_CONFIRM) && cursor == 4) || (keys & PAD_BUTTON_START) ||
        (frames == 120 && !auto_started && getenv("MELEE_TACTICS_AUTOSTART")))
    {
        auto_started = true;
        gm_801A4B60();
        return;
    }
    if ((keys & PAD_CONFIRM) && cursor == 5) {
        port = 1 - port;
    }
    view.title = "MELEE TACTICS";
    view.screen = LOBBY_SCREEN_MENU;
    view.cursor = cursor;
    view.menu_count = MENU_ROWS;
    snprintf(view.subtitle, sizeof(view.subtitle), "Editing P%d", port + 1);
    snprintf(view.menu[0], sizeof(view.menu[0]), "Fighter: %s",
             tactics_FighterName(draft[port].ckind));
    for (i = 0; i < TACTICS_SLOTS; i++) {
        snprintf(view.menu[i + 1], sizeof(view.menu[i + 1]), "%d. %s", i + 1,
                 moveName(draft[port].ckind, draft[port].moves[i]));
    }
    snprintf(view.menu[4], sizeof(view.menu[4]), "FIGHT - 1 stock, until a knockout");
    snprintf(view.menu[5], sizeof(view.menu[5]), "Inspect / edit P%d: %s", 2 - port,
             tactics_FighterName(draft[1 - port].ckind));
    snprintf(view.message, sizeof(view.message), "%s", result);
    view.hint = "Up/Down: row   Left/Right: edit   X: side   START: fight";
    mnOnlineLobby_Update(&view);
}
