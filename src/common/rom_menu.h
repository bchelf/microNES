#ifndef MICRONES_ROM_MENU_H
#define MICRONES_ROM_MENU_H

#include "framebuffer.h"
#include "rom_source.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int     selected;
    int     top;
    int     hold_dir;       /* -1 = up, 0 = none, +1 = down */
    uint32_t hold_frames;   /* frames the dir has been held */
} RomMenu;

typedef enum {
    ROM_MENU_RESULT_NONE = 0,
    ROM_MENU_RESULT_LAUNCH,         /* user picked a supported entry */
    ROM_MENU_RESULT_LAUNCH_BLOCKED, /* user picked an unsupported entry */
} RomMenuResult;

void rom_menu_init(RomMenu *menu);

/* Process one frame of input.  Edge-detects button-down events using
 * prev_buttons vs curr_buttons.  Up/Down auto-repeat after a brief hold so
 * navigating long lists feels responsive.  Returns whether a launch was
 * requested; on launch, *out_index receives the selected entry index. */
RomMenuResult rom_menu_step(RomMenu *menu,
                            RomSource *source,
                            uint8_t prev_buttons,
                            uint8_t curr_buttons,
                            int *out_index);

/* Paint the menu into fb.  status (may be NULL) is shown above the footer
 * and is intended for transient messages like "mapper 4 not supported". */
void rom_menu_render(const RomMenu *menu,
                     RomSource *source,
                     NesFrameBuffer *fb,
                     const char *status);

/* Draw a modal loading-bar overlay on top of the current framebuffer.
 * pct is 0-100.  name (may be NULL) is displayed above the bar. */
void rom_menu_render_loading(NesFrameBuffer *fb, const char *name, int pct);

#endif
