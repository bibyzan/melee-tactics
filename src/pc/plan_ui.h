/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PLAN_UI_H
#define PC_PLAN_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Melee Tactics' pick panel, mirrored for the host: on a touch screen the
 * browser page lays big buttons over it (count may be 0 while waiting), and a
 * tap comes back through tactics_PlanTap. Called whenever the panel is drawn;
 * cheap when nothing changed. Native builds ignore it. */
void pc_plan_ui(bool visible, const char* title, const char* sub, const char* const* labels,
                int count, int cursor);

#ifdef __cplusplus
}
#endif

#endif
