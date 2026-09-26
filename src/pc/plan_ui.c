/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native builds draw the pick panel in the game only (see plan_ui.h; the
 * browser's version is platforms/browser/plan_web.c). */
#include "pc/plan_ui.h"

bool pc_plan_ui(bool visible, const char* title, const char* sub, const char* const* labels,
                int count, int cursor) {
    (void)visible;
    (void)title;
    (void)sub;
    (void)labels;
    (void)count;
    (void)cursor;
    return false;
}

void pc_fighter_ui(int mode, int p1, int p2, const char* const* rows, int count) {
    (void)mode;
    (void)p1;
    (void)p2;
    (void)rows;
    (void)count;
}
