#include "rom_menu.h"

#include "font_5x7.h"
#include "input.h"

#include <stdio.h>
#include <string.h>

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

enum {
    MENU_HEADER_H        = 14,
    MENU_LIST_TOP_Y      = 18,
    MENU_ITEM_H          = 8,
    MENU_VISIBLE_ROWS    = 24,
    MENU_LIST_BOTTOM_Y   = MENU_LIST_TOP_Y + MENU_VISIBLE_ROWS * MENU_ITEM_H,
    MENU_STATUS_Y        = 214,
    MENU_FOOTER_Y        = 230,
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

static void move_selection(RomMenu *menu, RomSource *source, int dir) {
    int n = source != NULL ? (int)source->count(source) : 0;
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
    menu->selected = find_next_navigable(source, next, dir);

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
    int n = source != NULL ? (int)source->count(source) : 0;
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
        const RomSourceEntry *e = source->entry(source, (size_t)menu->selected);
        if (out_index != NULL) {
            *out_index = menu->selected;
        }
        if (e != NULL && e->supported) {
            return ROM_MENU_RESULT_LAUNCH;
        }
        return ROM_MENU_RESULT_LAUNCH_BLOCKED;
    }

    return ROM_MENU_RESULT_NONE;
}

static void format_mapper_tag(const RomSourceEntry *e, char *out, size_t out_size) {
    if (e == NULL) {
        if (out != NULL && out_size > 0) {
            out[0] = '\0';
        }
        return;
    }
    if (e->mapper == 0xFFFFu) {
        snprintf(out, out_size, "?");
        return;
    }
    if (e->supported) {
        snprintf(out, out_size, "M%u", (unsigned)e->mapper);
    } else {
        snprintf(out, out_size, "M%u*", (unsigned)e->mapper);
    }
}

static void draw_centered_text(NesFrameBuffer *fb, int y, const char *text, uint8_t color) {
    int w = font5x7_text_width(text);
    int x = (NES_FRAME_WIDTH - w) / 2;
    if (x < 0) x = 0;
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

    /* Header bar. */
    fill_rect(fb, 0, 0, NES_FRAME_WIDTH, MENU_HEADER_H, MENU_BAR);
    font5x7_draw_text(fb, 4, 4, "microNES", MENU_TEXT);

    int n = source != NULL ? (int)source->count(source) : 0;
    char count_buf[24];
    snprintf(count_buf, sizeof(count_buf), "%d ROM%s", n, n == 1 ? "" : "s");
    int cw = font5x7_text_width(count_buf);
    font5x7_draw_text(fb, NES_FRAME_WIDTH - cw - 4, 4, count_buf, MENU_TEXT);

    if (n <= 0) {
        draw_centered_text(fb, 100, "No ROMs found.", MENU_TEXT);
        draw_centered_text(fb, 112, "Add some .nes files to your directory.", MENU_TEXT_FAINT);
        /* Footer hint. */
        const char *footer = "Quit the host to retry.";
        draw_centered_text(fb, MENU_FOOTER_Y, footer, MENU_TEXT_FAINT);
        return;
    }

    int top = menu != NULL ? menu->top : 0;
    int selected = menu != NULL ? menu->selected : 0;
    int rows_to_draw = n - top;
    if (rows_to_draw > MENU_VISIBLE_ROWS) {
        rows_to_draw = MENU_VISIBLE_ROWS;
    }

    for (int row = 0; row < rows_to_draw; ++row) {
        int idx = top + row;
        const RomSourceEntry *e = source->entry(source, (size_t)idx);
        int y = MENU_LIST_TOP_Y + row * MENU_ITEM_H;
        bool is_selected = (idx == selected);
        bool supported = e != NULL && e->supported;

        if (is_selected) {
            fill_rect(fb, 0, y, NES_FRAME_WIDTH, MENU_ITEM_H, MENU_BAR);
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
        /* Truncate to fit in the available pixel width. */
        char trim[ROM_SOURCE_NAME_MAX];
        snprintf(trim, sizeof(trim), "%s", name);
        char tag[8];
        format_mapper_tag(e, tag, sizeof(tag));
        int tag_w = font5x7_text_width(tag);
        int max_name_px = NES_FRAME_WIDTH - 4 - tag_w - 8 - 4;
        int max_chars = max_name_px / FONT5X7_CELL_W;
        if (max_chars < 1) max_chars = 1;
        if ((int)strlen(trim) > max_chars) {
            if (max_chars >= 1) {
                trim[max_chars] = '\0';
            }
        }

        font5x7_draw_text(fb, 4, y + 1, trim, name_color);
        font5x7_draw_text(fb, NES_FRAME_WIDTH - tag_w - 4, y + 1, tag, info_color);
    }

    /* Scroll bar hint on the right when list is taller than window. */
    if (n > MENU_VISIBLE_ROWS) {
        int track_x = NES_FRAME_WIDTH - 2;
        fill_rect(fb, track_x, MENU_LIST_TOP_Y, 2,
                  MENU_VISIBLE_ROWS * MENU_ITEM_H, MENU_TEXT_DIM);
        int track_h = MENU_VISIBLE_ROWS * MENU_ITEM_H;
        int thumb_h = (MENU_VISIBLE_ROWS * track_h) / n;
        if (thumb_h < 4) thumb_h = 4;
        int thumb_y = MENU_LIST_TOP_Y + (top * (track_h - thumb_h)) /
                      (n > MENU_VISIBLE_ROWS ? (n - MENU_VISIBLE_ROWS) : 1);
        fill_rect(fb, track_x, thumb_y, 2, thumb_h, MENU_TEXT);
    }

    if (status != NULL && status[0] != '\0') {
        draw_centered_text(fb, MENU_STATUS_Y, status, MENU_TEXT_ERROR);
    }

    /* Footer hint. */
    const char *footer = "Up/Down move  Start/A run  Down+Start back";
    draw_centered_text(fb, MENU_FOOTER_Y, footer, MENU_TEXT_FAINT);
}
