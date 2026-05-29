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
    bool    show_flash_actions; /* append erase/copy actions after ROM list */
} RomMenu;

typedef enum {
    ROM_MENU_RESULT_NONE = 0,
    ROM_MENU_RESULT_LAUNCH,         /* user picked a supported entry */
    ROM_MENU_RESULT_LAUNCH_BLOCKED, /* user picked an unsupported entry */
    ROM_MENU_RESULT_IMPORT,         /* user triggered SD-to-flash import */
    ROM_MENU_RESULT_ERASE_FLASH,    /* user triggered flash erase */
} RomMenuResult;

void rom_menu_init(RomMenu *menu);

/* Move the cursor to a ROM entry and keep it visible.  Intended for callers
 * returning from a running ROM to the menu. */
void rom_menu_select(RomMenu *menu, RomSource *source, int index);

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

/* Render the import-mode menu: a "Copy SD Card to Flash" action followed by
 * greyed-out entries from import_source.  Pressing Start/A returns
 * ROM_MENU_RESULT_IMPORT from rom_menu_step_import. */
RomMenuResult rom_menu_step_import(RomMenu *menu,
                                   RomSource *import_source,
                                   uint8_t prev_buttons,
                                   uint8_t curr_buttons);
void rom_menu_render_import(const RomMenu *menu,
                            RomSource *import_source,
                            NesFrameBuffer *fb,
                            const char *status);

/* Draw a modal confirmation dialog.  Returns true on A, false on B. */
void rom_menu_render_confirm(NesFrameBuffer *fb,
                             const char *title,
                             const char *detail);

/* Draw a modal loading-bar overlay on top of the current framebuffer.
 * pct is 0-100.  name (may be NULL) is displayed above the bar. */
void rom_menu_render_loading(NesFrameBuffer *fb, const char *name, int pct);

#endif
