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

/* The fighter menus, mirrored the same way as a grid of fighters to tap.
 * mode: 0 none, 1 both P1 and P2 (against the CPU), 2 the player's own only
 * (online). p1 and p2 are the chosen character kinds, -1 for random. A tap
 * comes back through tactics_FighterTap. rows are the screen's other menu
 * rows, shown as buttons since the grid covers the D-pad: a tap on rows[i]
 * comes back as tactics_MenuTap(i + 1), menu row i + 1 chosen with A.
 * Native builds ignore it. */
void pc_fighter_ui(int mode, int p1, int p2, const char* const* rows, int count);

#ifdef __cplusplus
}
#endif

#endif
