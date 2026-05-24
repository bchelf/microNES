#include "rom_menu.h"

#include "font_5x7.h"
#include "input.h"

#include <stdio.h>
#include <string.h>

static size_t empty_count(RomSource *self) { (void)self; return 0; }
static const RomSourceEntry *empty_entry(RomSource *self, size_t i) {
    (void)self; (void)i; return NULL;
}
static bool empty_load(RomSource *self, size_t i, uint8_t **buf, size_t *sz,
                       char *err, size_t err_size) {
    (void)self; (void)i; (void)buf; (void)sz;
    if (err && err_size) snprintf(err, err_size, "no source");
    return false;
}
static void empty_free(RomSource *self, uint8_t *buf) { (void)self; (void)buf; }

void rom_source_make_empty(RomSource *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->count    = empty_count;
    out->entry    = empty_entry;
    out->load     = empty_load;
    out->free_buf = empty_free;
}

/* NES palette indices used for the menu.  These map through the host's
 * built-in 64-entry palette table:
 *   0x0F  pure black
 *   0x00  medium gray   (~0x7c)
 *   0x10  light gray    (~0xbc)
 *   0x12  blue
 *   0x16  red
 *   0x30  white
 */
enum {
    MENU_BG          = 0x0Fu,
    MENU_TEXT        = 0x30u,
    MENU_TEXT_DIM    = 0x00u,
    MENU_TEXT_FAINT  = 0x10u,
    MENU_BAR         = 0x12u,
    MENU_TEXT_ERROR  = 0x16u,
};

/* Overscan inset.  CRT TVs vary widely in how much of the 256x240 NES
 * frame actually reaches the visible glass.  We keep ~10 px on the
 * sides and top, and 20 px on the bottom — many CRTs lose more on the
 * bottom edge than the top, and a wider bottom margin gives the
 * footer hint visible breathing room from the safe-area boundary.
 * All menu drawing is relative to MENU_SAFE_LEFT/TOP/RIGHT/BOTTOM. */
enum {
    MENU_INSET_X            = 10,
    MENU_INSET_Y_TOP        = 10,
    MENU_INSET_Y_BOTTOM     = 20,
    MENU_SAFE_LEFT          = MENU_INSET_X,
    MENU_SAFE_TOP           = MENU_INSET_Y_TOP,
    MENU_SAFE_RIGHT         = NES_FRAME_WIDTH  - MENU_INSET_X,
    MENU_SAFE_BOTTOM        = NES_FRAME_HEIGHT - MENU_INSET_Y_BOTTOM,
    MENU_SAFE_WIDTH         = MENU_SAFE_RIGHT  - MENU_SAFE_LEFT,
    MENU_SAFE_HEIGHT        = MENU_SAFE_BOTTOM - MENU_SAFE_TOP,

    MENU_HEADER_H           = 14,
    MENU_HEADER_TOP_Y       = MENU_SAFE_TOP,
    MENU_COL_HDR_Y          = MENU_HEADER_TOP_Y + MENU_HEADER_H + 3,
    MENU_LIST_TOP_Y         = MENU_COL_HDR_Y + 9,
    MENU_ITEM_H             = 8,
    MENU_VISIBLE_ROWS       = 21,
    MENU_LIST_BOTTOM_Y      = MENU_LIST_TOP_Y + MENU_VISIBLE_ROWS * MENU_ITEM_H,
    MENU_STATUS_Y           = MENU_LIST_BOTTOM_Y + 3,
    MENU_FOOTER_Y           = MENU_SAFE_BOTTOM - 7,
    MENU_HOLD_INITIAL_DELAY = 18,
    MENU_HOLD_REPEAT_RATE   = 4,
};

static void fill_rect(NesFrameBuffer *fb, int x, int y, int w, int h, uint8_t color) {
    if (fb == NULL) {
        return;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > NES_FRAME_WIDTH)  { w = NES_FRAME_WIDTH  - x; }
    if (y + h > NES_FRAME_HEIGHT) { h = NES_FRAME_HEIGHT - y; }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int row = 0; row < h; ++row) {
        uint8_t *line = nes_framebuffer_scanline(fb, (uint16_t)(y + row));
        memset(&line[x], color, (size_t)w);
    }
}

static void clear_fb(NesFrameBuffer *fb, uint8_t color) {
    if (fb == NULL) {
        return;
    }
    memset(fb->pixels, color, sizeof(fb->pixels));
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void rom_menu_init(RomMenu *menu) {
    if (menu == NULL) {
        return;
    }
    memset(menu, 0, sizeof(*menu));
}

static int find_next_navigable(RomSource *source, int from, int dir) {
    /* All entries are navigable (we let the user move over unsupported ROMs
     * to see them); just clamp.  This helper exists so the rule can change
     * without touching the input handler. */
    (void)dir;
    if (source == NULL) {
        return 0;
    }
    int n = (int)source->count(source);
    if (n <= 0) {
        return 0;
    }
    return clamp_int(from, 0, n - 1);
}

static int menu_total_items(RomMenu *menu, RomSource *source) {
    int n = source != NULL ? (int)source->count(source) : 0;
    if (menu->show_flash_actions && n > 0)
        n += 2;
    return n;
}

static void move_selection(RomMenu *menu, RomSource *source, int dir) {
    int n = menu_total_items(menu, source);
    if (n <= 0) {
        menu->selected = 0;
        menu->top = 0;
        return;
    }
    int next = menu->selected + dir;
    if (next < 0) {
        next = n - 1;             /* wrap to bottom */
    } else if (next >= n) {
        next = 0;                 /* wrap to top */
    }
    int rom_count = source != NULL ? (int)source->count(source) : 0;
    if (next < rom_count) {
        menu->selected = find_next_navigable(source, next, dir);
    } else {
        menu->selected = next;
    }

    /* Keep selection visible. */
    if (menu->selected < menu->top) {
        menu->top = menu->selected;
    } else if (menu->selected >= menu->top + MENU_VISIBLE_ROWS) {
        menu->top = menu->selected - (MENU_VISIBLE_ROWS - 1);
    }
    int max_top = n > MENU_VISIBLE_ROWS ? n - MENU_VISIBLE_ROWS : 0;
    menu->top = clamp_int(menu->top, 0, max_top);
}

RomMenuResult rom_menu_step(RomMenu *menu,
                            RomSource *source,
                            uint8_t prev_buttons,
                            uint8_t curr_buttons,
                            int *out_index) {
    if (menu == NULL) {
        return ROM_MENU_RESULT_NONE;
    }

    uint8_t pressed = (uint8_t)(~prev_buttons & curr_buttons);

    bool up_held   = (curr_buttons & NES_BUTTON_UP)   != 0;
    bool down_held = (curr_buttons & NES_BUTTON_DOWN) != 0;
    int  dir = 0;
    if (up_held && !down_held)        dir = -1;
    else if (down_held && !up_held)   dir = +1;

    if (dir == 0) {
        menu->hold_dir = 0;
        menu->hold_frames = 0;
    } else if (dir != menu->hold_dir) {
        /* Fresh press: act now, start the hold timer. */
        move_selection(menu, source, dir);
        menu->hold_dir = dir;
        menu->hold_frames = 1;
    } else {
        ++menu->hold_frames;
        if (menu->hold_frames >= (uint32_t)MENU_HOLD_INITIAL_DELAY) {
            uint32_t since = menu->hold_frames - (uint32_t)MENU_HOLD_INITIAL_DELAY;
            if ((since % (uint32_t)MENU_HOLD_REPEAT_RATE) == 0u) {
                move_selection(menu, source, dir);
            }
        }
    }

    /* Clamp on every step in case the source mutated under us. */
    int n = menu_total_items(menu, source);
    int rom_count = source != NULL ? (int)source->count(source) : 0;
    if (n <= 0) {
        menu->selected = 0;
        menu->top = 0;
    } else {
        menu->selected = clamp_int(menu->selected, 0, n - 1);
        int max_top = n > MENU_VISIBLE_ROWS ? n - MENU_VISIBLE_ROWS : 0;
        menu->top = clamp_int(menu->top, 0, max_top);
    }

    /* Launch on Start or A. */
    if ((pressed & (NES_BUTTON_START | NES_BUTTON_A)) != 0u && n > 0) {
        if (out_index != NULL) {
            *out_index = menu->selected;
        }
        if (menu->show_flash_actions && menu->selected == rom_count) {
            return ROM_MENU_RESULT_ERASE_FLASH;
        }
        if (menu->show_flash_actions && menu->selected == rom_count + 1) {
            return ROM_MENU_RESULT_IMPORT;
        }
        const RomSourceEntry *e = source->entry(source, (size_t)menu->selected);
        if (e != NULL && e->supported) {
            return ROM_MENU_RESULT_LAUNCH;
        }
        return ROM_MENU_RESULT_LAUNCH_BLOCKED;
    }

    return ROM_MENU_RESULT_NONE;
}

static void format_info_tag(const RomSourceEntry *e, char *out, size_t out_size) {
    if (e == NULL) {
        if (out != NULL && out_size > 0) out[0] = '\0';
        return;
    }
    if (e->mapper == 0xFFFFu) {
        snprintf(out, out_size, "?");
        return;
    }
    if (!e->supported) {
        snprintf(out, out_size, "M%u", (unsigned)e->mapper);
        return;
    }
    snprintf(out, out_size, "M%u", (unsigned)e->mapper);
}

static void draw_centered_text(NesFrameBuffer *fb, int y, const char *text, uint8_t color) {
    int w = font5x7_text_width(text);
    int x = MENU_SAFE_LEFT + (MENU_SAFE_WIDTH - w) / 2;
    if (x < MENU_SAFE_LEFT) x = MENU_SAFE_LEFT;
    font5x7_draw_text(fb, x, y, text, color);
}

void rom_menu_render(const RomMenu *menu,
                     RomSource *source,
                     NesFrameBuffer *fb,
                     const char *status) {
    if (fb == NULL) {
        return;
    }

    clear_fb(fb, MENU_BG);

    /* Header bar — confined to the safe area so a CRT bezel doesn't
     * eat the title or the ROM count. */
    fill_rect(fb, MENU_SAFE_LEFT, MENU_HEADER_TOP_Y,
              MENU_SAFE_WIDTH, MENU_HEADER_H, MENU_BAR);
    font5x7_draw_text(fb, MENU_SAFE_LEFT + 4, MENU_HEADER_TOP_Y + 4,
                      "microNES", MENU_TEXT);

    int n = source != NULL ? (int)source->count(source) : 0;
    char count_buf[24];
    snprintf(count_buf, sizeof(count_buf), "%d ROM%s", n, n == 1 ? "" : "s");
    int cw = font5x7_text_width(count_buf);
    font5x7_draw_text(fb, MENU_SAFE_RIGHT - cw - 4, MENU_HEADER_TOP_Y + 4,
                      count_buf, MENU_TEXT);

    if (n <= 0) {
        int empty_msg_y    = MENU_LIST_TOP_Y + 70;
        int empty_detail_y = empty_msg_y + 12;
        draw_centered_text(fb, empty_msg_y, "No ROMs found.", MENU_TEXT);
        draw_centered_text(fb, empty_detail_y,
                           "Add some .nes files to your directory.",
                           MENU_TEXT_FAINT);
        const char *footer = "Quit the host to retry.";
        draw_centered_text(fb, MENU_FOOTER_Y, footer, MENU_TEXT_FAINT);
        return;
    }

    /* Column headers. */
    font5x7_draw_text(fb, MENU_SAFE_LEFT + 4, MENU_COL_HDR_Y, "Name", MENU_TEXT_FAINT);
    {
        const char *rt_hdr = "Mapper";
        int rt_w = font5x7_text_width(rt_hdr);
        font5x7_draw_text(fb, MENU_SAFE_RIGHT - rt_w - 4, MENU_COL_HDR_Y, rt_hdr, MENU_TEXT_FAINT);
    }

    int rom_count = n;
    bool has_actions = menu != NULL && menu->show_flash_actions && rom_count > 0;
    int total_items = has_actions ? rom_count + 2 : rom_count;

    int top = menu != NULL ? menu->top : 0;
    int selected = menu != NULL ? menu->selected : 0;
    int rows_to_draw = total_items - top;
    if (rows_to_draw > MENU_VISIBLE_ROWS) {
        rows_to_draw = MENU_VISIBLE_ROWS;
    }

    enum { MENU_ACTION_H = MENU_ITEM_H + 2 };

    for (int row = 0; row < rows_to_draw; ++row) {
        int idx = top + row;
        int y = MENU_LIST_TOP_Y;
        for (int r = 0; r < row; ++r) {
            int ri = top + r;
            y += (has_actions && ri >= rom_count) ? MENU_ACTION_H : MENU_ITEM_H;
        }
        bool is_selected = (idx == selected);
        int item_h = (has_actions && idx >= rom_count) ? MENU_ACTION_H : MENU_ITEM_H;

        if (idx >= rom_count && has_actions) {
            if (idx == rom_count) {
                fill_rect(fb, MENU_SAFE_LEFT + 4, y, MENU_SAFE_WIDTH - 8, 1, MENU_TEXT_DIM);
                y += 2;
            }
            if (is_selected) {
                fill_rect(fb, MENU_SAFE_LEFT, y,
                          MENU_SAFE_WIDTH, item_h, MENU_BAR);
            }
            const char *label;
            uint8_t color;
            if (idx == rom_count) {
                label = "Erase Flash";
                color = is_selected ? MENU_TEXT : MENU_TEXT_ERROR;
            } else {
                label = "Copy from SD Card";
                color = is_selected ? MENU_TEXT : MENU_TEXT_FAINT;
            }
            font5x7_draw_text(fb, MENU_SAFE_LEFT + 4, y + 2, label, color);
            continue;
        }

        const RomSourceEntry *e = source->entry(source, (size_t)idx);
        bool supported = e != NULL && e->supported;

        if (is_selected) {
            fill_rect(fb, MENU_SAFE_LEFT, y,
                      MENU_SAFE_WIDTH, MENU_ITEM_H, MENU_BAR);
        }

        uint8_t name_color;
        uint8_t info_color;
        if (!supported) {
            name_color = MENU_TEXT_DIM;
            info_color = MENU_TEXT_DIM;
        } else if (is_selected) {
            name_color = MENU_TEXT;
            info_color = MENU_TEXT_FAINT;
        } else {
            name_color = MENU_TEXT;
            info_color = MENU_TEXT_FAINT;
        }

        const char *name = e != NULL ? e->name : "";
        char trim[ROM_SOURCE_NAME_MAX];
        snprintf(trim, sizeof(trim), "%s", name);
        char tag[16];
        format_info_tag(e, tag, sizeof(tag));
        int tag_w = font5x7_text_width(tag);
        int max_name_px = MENU_SAFE_WIDTH - 4 - tag_w - 8 - 4;
        int max_chars = max_name_px / FONT5X7_CELL_W;
        if (max_chars < 1) max_chars = 1;
        if ((int)strlen(trim) > max_chars) {
            if (max_chars >= 1) {
                trim[max_chars] = '\0';
            }
        }

        font5x7_draw_text(fb, MENU_SAFE_LEFT + 4, y + 1, trim, name_color);
        font5x7_draw_text(fb, MENU_SAFE_RIGHT - tag_w - 4, y + 1, tag, info_color);
    }

    /* Scroll bar hint on the right when list is taller than window. */
    if (total_items > MENU_VISIBLE_ROWS) {
        int track_x = MENU_SAFE_RIGHT - 2;
        fill_rect(fb, track_x, MENU_LIST_TOP_Y, 2,
                  MENU_VISIBLE_ROWS * MENU_ITEM_H, MENU_TEXT_DIM);
        int track_h = MENU_VISIBLE_ROWS * MENU_ITEM_H;
        int thumb_h = (MENU_VISIBLE_ROWS * track_h) / total_items;
        if (thumb_h < 4) thumb_h = 4;
        int thumb_y = MENU_LIST_TOP_Y + (top * (track_h - thumb_h)) /
                      (total_items > MENU_VISIBLE_ROWS ? (total_items - MENU_VISIBLE_ROWS) : 1);
        fill_rect(fb, track_x, thumb_y, 2, thumb_h, MENU_TEXT);
    }

    if (status != NULL && status[0] != '\0') {
        draw_centered_text(fb, MENU_STATUS_Y, status, MENU_TEXT_ERROR);
    }

    /* Footer hint. */
    const char *footer = "Up/Down move  Start/A run  Down+Start back";
    draw_centered_text(fb, MENU_FOOTER_Y, footer, MENU_TEXT_FAINT);
}

RomMenuResult rom_menu_step_import(RomMenu *menu,
                                   RomSource *import_source,
                                   uint8_t prev_buttons,
                                   uint8_t curr_buttons) {
    if (menu == NULL) return ROM_MENU_RESULT_NONE;

    uint8_t pressed = (uint8_t)(~prev_buttons & curr_buttons);

    int n = import_source != NULL ? (int)import_source->count(import_source) : 0;
    if (n <= 0) {
        menu->selected = 0;
        menu->top = 0;
        return ROM_MENU_RESULT_NONE;
    }

    if ((pressed & (NES_BUTTON_START | NES_BUTTON_A)) != 0u) {
        return ROM_MENU_RESULT_IMPORT;
    }

    return ROM_MENU_RESULT_NONE;
}

void rom_menu_render_import(const RomMenu *menu,
                            RomSource *import_source,
                            NesFrameBuffer *fb,
                            const char *status) {
    if (fb == NULL) return;
    (void)menu;

    clear_fb(fb, MENU_BG);

    fill_rect(fb, MENU_SAFE_LEFT, MENU_HEADER_TOP_Y,
              MENU_SAFE_WIDTH, MENU_HEADER_H, MENU_BAR);
    font5x7_draw_text(fb, MENU_SAFE_LEFT + 4, MENU_HEADER_TOP_Y + 4,
                      "microNES", MENU_TEXT);

    int n = import_source != NULL ? (int)import_source->count(import_source) : 0;

    if (n <= 0) {
        int empty_y = MENU_LIST_TOP_Y + 70;
        draw_centered_text(fb, empty_y, "No SD card or no ROMs found.", MENU_TEXT);
        draw_centered_text(fb, empty_y + 12,
                           "Insert an SD card with .nes files.",
                           MENU_TEXT_FAINT);
        return;
    }

    {
        const char *action = "> Copy SD Card to Flash";
        int aw = font5x7_text_width(action);
        int ax = MENU_SAFE_LEFT + (MENU_SAFE_WIDTH - aw) / 2;
        if (ax < MENU_SAFE_LEFT) ax = MENU_SAFE_LEFT;
        fill_rect(fb, MENU_SAFE_LEFT, MENU_COL_HDR_Y,
                  MENU_SAFE_WIDTH, 10, MENU_BAR);
        font5x7_draw_text(fb, ax, MENU_COL_HDR_Y + 1, action, MENU_TEXT);
    }

    int list_y = MENU_LIST_TOP_Y;
    int rows_to_draw = n;
    if (rows_to_draw > MENU_VISIBLE_ROWS) rows_to_draw = MENU_VISIBLE_ROWS;

    for (int row = 0; row < rows_to_draw; ++row) {
        const RomSourceEntry *e = import_source->entry(import_source, (size_t)row);
        int y = list_y + row * MENU_ITEM_H;
        const char *name = e != NULL ? e->name : "";
        char trim[ROM_SOURCE_NAME_MAX];
        snprintf(trim, sizeof(trim), "%s", name);
        int max_chars = (MENU_SAFE_WIDTH - 8) / FONT5X7_CELL_W;
        if (max_chars < 1) max_chars = 1;
        if ((int)strlen(trim) > max_chars) trim[max_chars] = '\0';
        font5x7_draw_text(fb, MENU_SAFE_LEFT + 4, y + 1, trim, MENU_TEXT_DIM);
    }

    if (status != NULL && status[0] != '\0') {
        draw_centered_text(fb, MENU_STATUS_Y, status, MENU_TEXT_ERROR);
    }

    draw_centered_text(fb, MENU_FOOTER_Y,
                       "Press Start/A to copy ROMs to flash",
                       MENU_TEXT_FAINT);
}

void rom_menu_render_confirm(NesFrameBuffer *fb,
                             const char *title,
                             const char *detail) {
    if (fb == NULL) return;

    clear_fb(fb, MENU_BG);

    enum {
        BOX_W  = 200,
        BOX_H  = 56,
        BOX_X  = (NES_FRAME_WIDTH  - BOX_W) / 2,
        BOX_Y  = (NES_FRAME_HEIGHT - BOX_H) / 2 - 10,
    };

    fill_rect(fb, BOX_X, BOX_Y, BOX_W, BOX_H, MENU_BG);
    fill_rect(fb, BOX_X, BOX_Y, BOX_W, 1, MENU_TEXT_DIM);
    fill_rect(fb, BOX_X, BOX_Y + BOX_H - 1, BOX_W, 1, MENU_TEXT_DIM);
    fill_rect(fb, BOX_X, BOX_Y, 1, BOX_H, MENU_TEXT_DIM);
    fill_rect(fb, BOX_X + BOX_W - 1, BOX_Y, 1, BOX_H, MENU_TEXT_DIM);

    if (title != NULL) {
        draw_centered_text(fb, BOX_Y + 8, title, MENU_TEXT_ERROR);
    }
    if (detail != NULL) {
        draw_centered_text(fb, BOX_Y + 22, detail, MENU_TEXT);
    }

    draw_centered_text(fb, BOX_Y + 38, "A = confirm   B = cancel", MENU_TEXT_FAINT);
}

void rom_menu_render_loading(NesFrameBuffer *fb, const char *name, int pct) {
    if (fb == NULL) {
        return;
    }
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    enum {
        BOX_W  = 180,
        BOX_H  = 40,
        BOX_X  = (NES_FRAME_WIDTH  - BOX_W) / 2,
        BOX_Y  = (NES_FRAME_HEIGHT - BOX_H) / 2 - 10,
        BAR_X  = BOX_X + 10,
        BAR_Y  = BOX_Y + 22,
        BAR_W  = BOX_W - 20,
        BAR_H  = 8,
    };

    fill_rect(fb, BOX_X - 2, BOX_Y - 2, BOX_W + 4, BOX_H + 4, MENU_BG);
    fill_rect(fb, BOX_X, BOX_Y, BOX_W, BOX_H, MENU_BG);
    fill_rect(fb, BOX_X, BOX_Y, BOX_W, 1, MENU_TEXT_DIM);
    fill_rect(fb, BOX_X, BOX_Y + BOX_H - 1, BOX_W, 1, MENU_TEXT_DIM);
    fill_rect(fb, BOX_X, BOX_Y, 1, BOX_H, MENU_TEXT_DIM);
    fill_rect(fb, BOX_X + BOX_W - 1, BOX_Y, 1, BOX_H, MENU_TEXT_DIM);

    char label[64];
    if (name != NULL && name[0] != '\0') {
        char trimmed[24];
        snprintf(trimmed, sizeof(trimmed), "%s", name);
        snprintf(label, sizeof(label), "Loading %s...", trimmed);
    } else {
        snprintf(label, sizeof(label), "Loading...");
    }
    draw_centered_text(fb, BOX_Y + 6, label, MENU_TEXT);

    fill_rect(fb, BAR_X, BAR_Y, BAR_W, BAR_H, MENU_TEXT_DIM);
    int fill_w = (BAR_W * pct) / 100;
    if (fill_w > 0) {
        fill_rect(fb, BAR_X, BAR_Y, fill_w, BAR_H, MENU_BAR);
    }

    char pct_text[8];
    snprintf(pct_text, sizeof(pct_text), "%d%%", pct);
    int pw = font5x7_text_width(pct_text);
    font5x7_draw_text(fb, BOX_X + (BOX_W - pw) / 2, BAR_Y + BAR_H + 3,
                      pct_text, MENU_TEXT_FAINT);
}
