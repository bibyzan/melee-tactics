/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The internal resolution, for the game's own menus: Melee's 640x480 frame
 * rendered at a multiple, then fitted to the window. In the browser the page
 * picks the starting value (Module.renderScale) and remembers a change in
 * localStorage; natively the launcher's render scale stands until changed
 * here. */
#include "pc/render_scale.h"

#include <dolphin/vi.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static float s_scale = -1.0f;

float pc_render_scale(void) {
#ifdef __EMSCRIPTEN__
    if (s_scale < 0.0f) {
        s_scale = (float)EM_ASM_DOUBLE({ return Module.renderScale || 0; });
    }
#endif
    return s_scale < 0.0f ? 0.0f : s_scale;
}

void pc_set_render_scale(float scale) {
    s_scale = scale;
#ifdef __EMSCRIPTEN__
    /* In the browser a change after startup shows only a corner of the new
     * frame (it is not refitted to the canvas), so remember it and reload:
     * the page starts at the new scale, straight back into the menus. */
    EM_ASM(
        {
            Module.renderScale = $0;
            try {
                localStorage.setItem('melee-render-scale', String($0));
            } catch (e) {
            }
            location.reload();
        },
        scale);
#else
    VISetFrameBufferScale(scale);
#endif
}
