/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PLAN_UI_H
#define PC_PLAN_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Melee Tactics' pick panel, mirrored for the host: on a touch screen the
 * browser page shows it as big buttons in the space beside or below the game
 * (count may be 0 while waiting), and a tap comes back through
 * tactics_PlanTap. Called whenever the panel is drawn; cheap when nothing
 * changed. True when the host shows the panel itself, so the game need not
 * draw its own over the fight. Native builds ignore it. */
bool pc_plan_ui(bool visible, const char* title, const char* sub, const char* const* labels,
                int count, int cursor);

#ifdef __cplusplus
}
#endif

#endif
