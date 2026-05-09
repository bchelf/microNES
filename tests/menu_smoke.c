/* Drives the rom_menu state machine against a synthetic RomSource and
 * checks that navigation, selection, and launch-blocked behavior all do
 * what we expect.  No graphics, no NES core — just the state machine. */

#include "input.h"
#include "rom_menu.h"
#include "rom_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    RomSourceEntry entries[8];
    size_t         count;
} FakeSource;

static size_t fake_count(RomSource *self) {
    FakeSource *f = (FakeSource *)self->user;
    return f->count;
}

static const RomSourceEntry *fake_entry(RomSource *self, size_t index) {
    FakeSource *f = (FakeSource *)self->user;
    if (index >= f->count) return NULL;
    return &f->entries[index];
}

static bool fake_load(RomSource *self, size_t index,
                      uint8_t **out_buf, size_t *out_size,
                      char *err, size_t err_size) {
    (void)self; (void)index; (void)out_buf; (void)out_size;
    (void)err; (void)err_size;
    return false;
}

static void fake_free_buf(RomSource *self, uint8_t *buf) {
    (void)self; (void)buf;
}

static void fake_init(FakeSource *f, RomSource *src) {
    memset(f, 0, sizeof(*f));
    static const struct { const char *name; uint16_t mapper; bool ok; } seed[] = {
        { "Alpha",        0u, true  },
        { "Bravo",        1u, true  },
        { "Charlie",      4u, false },
        { "Delta",        2u, false },
        { "Echo",         0u, true  },
    };
    f->count = sizeof(seed) / sizeof(seed[0]);
    for (size_t i = 0; i < f->count; ++i) {
        snprintf(f->entries[i].name, sizeof(f->entries[i].name), "%s", seed[i].name);
        f->entries[i].mapper = seed[i].mapper;
        f->entries[i].supported = seed[i].ok;
    }

    memset(src, 0, sizeof(*src));
    src->user = f;
    src->count = fake_count;
    src->entry = fake_entry;
    src->load = fake_load;
    src->free_buf = fake_free_buf;
}

static int g_failures = 0;

#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (%s)\n", msg, #cond); ++g_failures; } \
} while (0)

/* Drive one frame of input.  Updates *prev to reflect curr after the step
 * so subsequent calls see the correct edge events. */
static RomMenuResult tick(RomMenu *m, RomSource *src, uint8_t *prev, uint8_t curr,
                          int *out_index) {
    RomMenuResult r = rom_menu_step(m, src, *prev, curr, out_index);
    *prev = curr;
    return r;
}

int main(void) {
    FakeSource fs;
    RomSource  src;
    RomMenu    menu;
    int idx = -1;

    fake_init(&fs, &src);
    rom_menu_init(&menu);

    /* Initial state: selected = 0. */
    EXPECT(menu.selected == 0, "initial selection is 0");

    uint8_t prev = 0;

    /* Press Down → selection moves to 1. */
    tick(&menu, &src, &prev, NES_BUTTON_DOWN, &idx);
    EXPECT(menu.selected == 1, "down moves to 1");

    /* Release Down. */
    tick(&menu, &src, &prev, 0, &idx);
    EXPECT(menu.selected == 1, "no change on release");

    /* Press Up → selection moves to 0. */
    tick(&menu, &src, &prev, NES_BUTTON_UP, &idx);
    EXPECT(menu.selected == 0, "up moves to 0");

    /* Wrap-around: Up at top wraps to last entry. */
    tick(&menu, &src, &prev, 0, &idx);
    tick(&menu, &src, &prev, NES_BUTTON_UP, &idx);
    EXPECT(menu.selected == (int)fs.count - 1, "up at top wraps to bottom");

    /* Wrap-around: Down at bottom wraps to top. */
    tick(&menu, &src, &prev, 0, &idx);
    tick(&menu, &src, &prev, NES_BUTTON_DOWN, &idx);
    EXPECT(menu.selected == 0, "down at bottom wraps to top");

    /* Press Start on supported entry → LAUNCH. */
    tick(&menu, &src, &prev, 0, &idx);
    RomMenuResult r = tick(&menu, &src, &prev, NES_BUTTON_START, &idx);
    EXPECT(r == ROM_MENU_RESULT_LAUNCH, "start on supported launches");
    EXPECT(idx == 0, "launch index matches selection");

    /* Move to unsupported entry (index 2: Charlie). */
    tick(&menu, &src, &prev, 0, &idx);
    tick(&menu, &src, &prev, NES_BUTTON_DOWN, &idx);
    tick(&menu, &src, &prev, 0, &idx);
    tick(&menu, &src, &prev, NES_BUTTON_DOWN, &idx);
    EXPECT(menu.selected == 2, "selection is unsupported entry");

    /* Press Start → BLOCKED. */
    tick(&menu, &src, &prev, 0, &idx);
    r = tick(&menu, &src, &prev, NES_BUTTON_START, &idx);
    EXPECT(r == ROM_MENU_RESULT_LAUNCH_BLOCKED, "start on unsupported is blocked");
    EXPECT(idx == 2, "blocked index matches selection");

    /* Press A on supported entry (move back to 0). */
    tick(&menu, &src, &prev, 0, &idx);
    for (int i = 0; i < (int)fs.count; ++i) {
        tick(&menu, &src, &prev, NES_BUTTON_UP, &idx);
        tick(&menu, &src, &prev, 0, &idx);
    }
    EXPECT(menu.selected == 2, "wrap-up cycle returns to 2");
    /* Move to 0 explicitly. */
    while (menu.selected != 0) {
        tick(&menu, &src, &prev, NES_BUTTON_UP, &idx);
        tick(&menu, &src, &prev, 0, &idx);
    }
    r = tick(&menu, &src, &prev, NES_BUTTON_A, &idx);
    EXPECT(r == ROM_MENU_RESULT_LAUNCH, "A also launches");

    /* Auto-repeat: hold Down for many frames and observe the selection
     * advances after the initial delay. */
    rom_menu_init(&menu);
    prev = 0;
    int prev_sel = 0;
    int advances = 0;
    for (int i = 0; i < 80; ++i) {
        tick(&menu, &src, &prev, NES_BUTTON_DOWN, &idx);
        if (menu.selected != prev_sel) ++advances;
        prev_sel = menu.selected;
    }
    EXPECT(advances >= 5, "auto-repeat advances multiple times while held");

    if (g_failures == 0) {
        printf("menu_smoke: OK\n");
        return 0;
    }
    fprintf(stderr, "menu_smoke: %d failure(s)\n", g_failures);
    return 1;
}
