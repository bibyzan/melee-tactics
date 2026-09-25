/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_RENDER_SCALE_H
#define PC_RENDER_SCALE_H

#ifdef __cplusplus
extern "C" {
#endif

/* The internal resolution as a multiple of Melee's 640x480; 0 follows the
 * window. */
float pc_render_scale(void);
/* Apply now; the browser also remembers it for the next visit. */
void pc_set_render_scale(float scale);

#ifdef __cplusplus
}
#endif

#endif
