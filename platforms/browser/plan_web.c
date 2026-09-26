/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The pick panel for the page (src/pc/plan_ui.h): on a touch screen the page
 * lays big buttons over the game's own list (touch.mjs), and a tap comes back
 * through browser_plan_pick. Only a change reaches the page. */
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#include "pc/plan_ui.h"
/* src/melee/tactics/tacticsmode.c */
void tactics_PlanTap(int index);

// clang-format off
EM_JS(int, plan_web_active, (void), {
  return Module.onPlan ? 1 : 0;
});
EM_JS(void, plan_web_show, (int visible, const char* text, int cursor), {
  if (Module.onPlan) Module.onPlan(visible ? UTF8ToString(text).split('\x1f') : null, cursor);
});
// clang-format on

bool pc_plan_ui(bool visible, const char* title, const char* sub, const char* const* labels,
                int count, int cursor) {
    static char last[1024];
    static int last_cursor = -2;
    char text[1024];
    size_t len;
    int i;

    /* title, sub, then the labels, separated by \x1f */
    len = (size_t)snprintf(text, sizeof text, "%s\x1f%s", visible ? title : "", visible ? sub : "");
    for (i = 0; visible && i < count && len < sizeof text; i++) {
        len += (size_t)snprintf(text + len, sizeof text - len, "\x1f%s", labels[i]);
    }
    if (!plan_web_active()) {
        return false;
    }
    if (strcmp(text, last) == 0 && cursor == last_cursor) {
        return true;
    }
    snprintf(last, sizeof last, "%s", text);
    last_cursor = cursor;
    plan_web_show(visible, text, cursor);
    return true;
}

/* A tapped row: the game picks it, as if chosen and confirmed. */
EMSCRIPTEN_KEEPALIVE void browser_plan_pick(int index) {
    tactics_PlanTap(index);
}

/* The fighter menus as a tap grid (touch.mjs createFighterPicker). */
void tactics_FighterTap(int slot, int ckind);

// clang-format off
EM_JS(void, fighter_web_show, (int mode, int p1, int p2), {
  if (Module.onFighters) Module.onFighters(mode, p1, p2);
});
// clang-format on

void pc_fighter_ui(int mode, int p1, int p2) {
    static int last[3] = { -2, -2, -2 };

    if (mode == last[0] && p1 == last[1] && p2 == last[2]) {
        return;
    }
    last[0] = mode;
    last[1] = p1;
    last[2] = p2;
    fighter_web_show(mode, p1, p2);
}

/* slot 0 or 1 (P1, P2 against the CPU; online, the player's own), and the
 * character kind, -1 for random. */
EMSCRIPTEN_KEEPALIVE void browser_pick_fighter(int slot, int ckind) {
    tactics_FighterTap(slot, ckind);
}
