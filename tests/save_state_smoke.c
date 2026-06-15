/* Save-state smoke test: exercises save_state_capture/apply, the POSIX
 * SaveStateStore, SaveMenu navigation, and the full AppShell SAVE_MENU
 * state machine (create / exit-to-save-menu / load / clear-all) against a
 * synthetic NROM ROM.  No graphics, no SDL — just the portable state
 * machines and the host POSIX store. */

#include "app_shell.h"
#include "input.h"
#include "nes.h"
#include "rom_menu.h"
#include "rom_source.h"
#include "save_state.h"
#include "save_state_store.h"
#include "save_state_store_posix.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures = 0;

#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (%s)\n", msg, #cond); ++g_failures; } \
} while (0)

/* Build a minimal 32KB NROM-256 ROM: iNES header + 32KB PRG of NOPs, with
 * NMI/RESET/IRQ vectors all pointing at $8000 (the first NOP). CHR banks=0
 * gives 8KB of CHR RAM automatically. */
static void build_synthetic_rom(uint8_t *rom, size_t total_size) {
    memset(rom, 0xEA /* NOP */, total_size);

    memcpy(rom, "NES\x1a", 4);
    rom[4] = 2; /* PRG banks: 2 * 16KB = 32KB */
    rom[5] = 0; /* CHR banks: 0 -> 8KB CHR RAM */
    rom[6] = 0; /* mapper low nibble = 0, horizontal mirroring */
    rom[7] = 0; /* mapper high nibble = 0 */
    for (size_t i = 8; i < 16; ++i) {
        rom[i] = 0;
    }

    size_t prg_off = 16;
    /* NMI ($FFFA/$FFFB), RESET ($FFFC/$FFFD), IRQ/BRK ($FFFE/$FFFF) all -> $8000. */
    rom[prg_off + 0x7FFA] = 0x00; rom[prg_off + 0x7FFB] = 0x80;
    rom[prg_off + 0x7FFC] = 0x00; rom[prg_off + 0x7FFD] = 0x80;
    rom[prg_off + 0x7FFE] = 0x00; rom[prg_off + 0x7FFF] = 0x80;
}

/* --- Fake RomSource: one entry backed by the synthetic ROM bytes. --- */

typedef struct {
    RomSourceEntry entries[1];
    size_t         count;
    const uint8_t *rom;
    size_t         rom_size;
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
    FakeSource *f = (FakeSource *)self->user;
    (void)err; (void)err_size;
    if (index >= f->count) return false;

    uint8_t *buf = (uint8_t *)malloc(f->rom_size);
    if (buf == NULL) return false;
    memcpy(buf, f->rom, f->rom_size);
    *out_buf = buf;
    *out_size = f->rom_size;
    return true;
}

static void fake_free_buf(RomSource *self, uint8_t *buf) {
    (void)self;
    free(buf);
}

static void fake_init(FakeSource *f, RomSource *src, const uint8_t *rom, size_t rom_size) {
    memset(f, 0, sizeof(*f));
    f->count = 1;
    snprintf(f->entries[0].name, sizeof(f->entries[0].name), "Test Rom");
    f->entries[0].mapper = 0;
    f->entries[0].supported = true;
    f->rom = rom;
    f->rom_size = rom_size;

    memset(src, 0, sizeof(*src));
    src->user     = f;
    src->count    = fake_count;
    src->entry    = fake_entry;
    src->load     = fake_load;
    src->free_buf = fake_free_buf;
}

/* Drive one frame of the AppShell with raw controller bits. */
static AppShellFrame shell_tick(AppShell *shell, uint8_t buttons) {
    NesControllerState in;
    in.buttons = buttons;
    return app_shell_begin_frame(shell, in);
}

/* Drive one frame of the SaveMenu, mirroring tick() in menu_smoke.c. */
static SaveMenuResult save_tick(SaveMenu *m, SaveStateStore *store,
                                uint8_t *prev, uint8_t curr, int *out_index) {
    SaveMenuResult r = save_menu_step(m, store, *prev, curr, out_index);
    *prev = curr;
    return r;
}

/* --- Section A: save_state_capture/apply roundtrip --- */

static void test_capture_apply_roundtrip(uint8_t *rom, size_t rom_size) {
    Nes nes;
    nes_init(&nes);
    EXPECT(nes_load_cartridge_const_memory(&nes, rom, rom_size), "load synthetic rom");
    nes_reset(&nes);

    uint32_t rom_checksum   = save_state_crc32(rom, rom_size);
    uint32_t rom_image_size = (uint32_t)rom_size;

    for (int i = 0; i < 5; ++i) {
        EXPECT(nes_step_frame(&nes), "step frame before capture");
    }

    SaveStateBlob blob;
    save_state_capture(&nes, rom_checksum, rom_image_size, 1234u, &blob);
    EXPECT(blob.header.magic == SAVE_STATE_MAGIC, "blob magic set");
    EXPECT(blob.header.version == SAVE_STATE_VERSION, "blob version set");
    EXPECT(blob.header.elapsed_seconds == 1234u, "blob elapsed_seconds set");

    uint64_t frame_count_at_capture = nes_frame_count(&nes);
    Cpu6502 cpu_at_capture = nes.cpu;

    for (int i = 0; i < 5; ++i) {
        EXPECT(nes_step_frame(&nes), "step frame after capture");
    }
    EXPECT(nes_frame_count(&nes) != frame_count_at_capture, "state diverged after capture");

    EXPECT(save_state_apply(&nes, &blob, rom_checksum, rom_image_size), "apply roundtrip");
    EXPECT(nes_frame_count(&nes) == frame_count_at_capture, "frame count restored");
    EXPECT(nes.cpu.pc == cpu_at_capture.pc, "pc restored");
    EXPECT(nes.cpu.cycles == cpu_at_capture.cycles, "cycles restored");

    EXPECT(!save_state_apply(&nes, &blob, rom_checksum + 1u, rom_image_size),
           "checksum mismatch rejected");

    SaveStateBlob bad = blob;
    bad.crc32 ^= 0xFFFFFFFFu;
    EXPECT(!save_state_apply(&nes, &bad, rom_checksum, rom_image_size),
           "corrupted crc32 rejected");

    nes_destroy(&nes);
}

/* --- Section B: POSIX SaveStateStore roundtrip --- */

static void test_posix_store_roundtrip(void) {
    char tmpl[] = "/tmp/micrones_ss_b_XXXXXX";
    char *tmpdir = mkdtemp(tmpl);
    EXPECT(tmpdir != NULL, "mkdtemp for posix store test");
    if (tmpdir == NULL) return;

    SaveStateStore store;
    EXPECT(save_state_store_posix_init(&store, tmpdir), "posix store init");

    EXPECT(store.refresh(&store, "Test Rom") == 0, "no saves before any write");
    EXPECT(store.count(&store) == 0, "count is 0 before any write");

    SaveStateBlob blob_a;
    memset(&blob_a, 0, sizeof(blob_a));
    blob_a.header.magic          = SAVE_STATE_MAGIC;
    blob_a.header.version        = SAVE_STATE_VERSION;
    blob_a.header.elapsed_seconds = 100u;

    EXPECT(store.save(&store, &blob_a), "save elapsed=100");
    EXPECT(store.count(&store) == 1, "count == 1 after first save");

    SaveStateBlob blob_b = blob_a;
    blob_b.header.elapsed_seconds = 50u;
    EXPECT(store.save(&store, &blob_b), "save elapsed=50");
    EXPECT(store.count(&store) == 2, "count == 2 after second save");

    const SaveStateEntry *e0 = store.entry(&store, 0);
    const SaveStateEntry *e1 = store.entry(&store, 1);
    EXPECT(e0 != NULL && e0->elapsed_seconds == 100u, "entry 0 is newest (100)");
    EXPECT(e1 != NULL && e1->elapsed_seconds == 50u, "entry 1 is oldest (50)");
    EXPECT(store.entry(&store, 2) == NULL, "entry 2 out of range");

    SaveStateBlob loaded;
    EXPECT(store.load(&store, 0, &loaded), "load entry 0");
    EXPECT(loaded.header.elapsed_seconds == 100u, "loaded entry 0 has elapsed=100");

    /* A third save colliding with the existing elapsed=100 entry should be
     * written as elapsed=101, and save() must update the blob in place
     * (header.elapsed_seconds + crc32) so the caller highlights the entry
     * that was actually written, not the pre-collision time. */
    SaveStateBlob blob_c = blob_a;
    blob_c.header.elapsed_seconds = 100u;
    EXPECT(store.save(&store, &blob_c), "save colliding elapsed=100");
    EXPECT(blob_c.header.elapsed_seconds == 101u,
           "colliding save rewrites elapsed_seconds to 101");
    EXPECT(blob_c.crc32 == save_state_crc32((const uint8_t *)&blob_c,
                                             offsetof(SaveStateBlob, crc32)),
           "colliding save recomputes crc32");
    EXPECT(store.count(&store) == 3, "count == 3 after colliding save");

    const SaveStateEntry *c0 = store.entry(&store, 0);
    EXPECT(c0 != NULL && c0->elapsed_seconds == 101u,
           "entry 0 is now the colliding save (101)");

    /* delete_entry removes just the targeted entry and re-scans. */
    EXPECT(store.delete_entry(&store, 1), "delete_entry index 1 (elapsed=100)");
    EXPECT(store.count(&store) == 2, "count == 2 after delete_entry");
    const SaveStateEntry *d0 = store.entry(&store, 0);
    const SaveStateEntry *d1 = store.entry(&store, 1);
    EXPECT(d0 != NULL && d0->elapsed_seconds == 101u, "entry 0 still 101 after delete");
    EXPECT(d1 != NULL && d1->elapsed_seconds == 50u, "entry 1 is now 50 after delete");
    EXPECT(!store.delete_entry(&store, 5), "delete_entry out of range fails");

    EXPECT(store.clear_all(&store), "clear_all");
    EXPECT(store.count(&store) == 0, "count == 0 after clear_all");

    char dir_name[9];
    save_state_store_dir_name("Test Rom", dir_name);
    char rom_dir_path[1100];
    snprintf(rom_dir_path, sizeof(rom_dir_path), "%s/%s", tmpdir, dir_name);
    rmdir(rom_dir_path);
    rmdir(tmpdir);

    save_state_store_posix_destroy(&store);
}

/* --- Section C: SaveMenu navigation against a populated posix store --- */

static void test_save_menu_navigation(void) {
    char tmpl[] = "/tmp/micrones_ss_c_XXXXXX";
    char *tmpdir = mkdtemp(tmpl);
    EXPECT(tmpdir != NULL, "mkdtemp for save menu test");
    if (tmpdir == NULL) return;

    SaveStateStore store;
    EXPECT(save_state_store_posix_init(&store, tmpdir), "posix store init (menu test)");
    store.refresh(&store, "Test Rom");

    /* Three saves: elapsed 100, 200, 300 -> newest-first order 300,200,100. */
    SaveStateBlob blob;
    memset(&blob, 0, sizeof(blob));
    blob.header.magic   = SAVE_STATE_MAGIC;
    blob.header.version = SAVE_STATE_VERSION;

    blob.header.elapsed_seconds = 100u;
    EXPECT(store.save(&store, &blob), "save elapsed=100");
    blob.header.elapsed_seconds = 200u;
    EXPECT(store.save(&store, &blob), "save elapsed=200");
    blob.header.elapsed_seconds = 300u;
    EXPECT(store.save(&store, &blob), "save elapsed=300");

    EXPECT(store.count(&store) == 3, "three saves present");
    const SaveStateEntry *e0 = store.entry(&store, 0);
    const SaveStateEntry *e1 = store.entry(&store, 1);
    const SaveStateEntry *e2 = store.entry(&store, 2);
    EXPECT(e0 != NULL && e0->elapsed_seconds == 300u, "entry 0 == 300 (newest)");
    EXPECT(e1 != NULL && e1->elapsed_seconds == 200u, "entry 1 == 200");
    EXPECT(e2 != NULL && e2->elapsed_seconds == 100u, "entry 2 == 100 (oldest)");

    /* total items = 3 saves + "Clear all" + "Back" == 5 (indices 0..4). */
    SaveMenu menu;
    save_menu_init(&menu);
    EXPECT(menu.selected == 0, "save menu starts at 0");

    uint8_t prev = 0;
    int idx = -1;

    save_tick(&menu, &store, &prev, NES_BUTTON_DOWN, &idx);
    EXPECT(menu.selected == 1, "down -> 1");
    save_tick(&menu, &store, &prev, 0, &idx);

    save_tick(&menu, &store, &prev, NES_BUTTON_DOWN, &idx);
    EXPECT(menu.selected == 2, "down -> 2");
    save_tick(&menu, &store, &prev, 0, &idx);

    save_tick(&menu, &store, &prev, NES_BUTTON_DOWN, &idx);
    EXPECT(menu.selected == 3, "down -> 3 (Clear all save states)");
    save_tick(&menu, &store, &prev, 0, &idx);

    save_tick(&menu, &store, &prev, NES_BUTTON_DOWN, &idx);
    EXPECT(menu.selected == 4, "down -> 4 (Back to ROM menu)");
    save_tick(&menu, &store, &prev, 0, &idx);

    save_tick(&menu, &store, &prev, NES_BUTTON_DOWN, &idx);
    EXPECT(menu.selected == 0, "down wraps from 4 back to 0");
    save_tick(&menu, &store, &prev, 0, &idx);

    /* Start on entry 0 -> LOAD with out_index == 0. */
    SaveMenuResult r = save_tick(&menu, &store, &prev, NES_BUTTON_START, &idx);
    EXPECT(r == SAVE_MENU_RESULT_LOAD, "start on save entry loads");
    EXPECT(idx == 0, "load index == 0");
    save_tick(&menu, &store, &prev, 0, &idx);

    /* Navigate to "Clear all save states" (index 3) and press A. */
    for (int i = 0; i < 3; ++i) {
        save_tick(&menu, &store, &prev, NES_BUTTON_DOWN, &idx);
        save_tick(&menu, &store, &prev, 0, &idx);
    }
    EXPECT(menu.selected == 3, "navigated to Clear all save states");
    r = save_tick(&menu, &store, &prev, NES_BUTTON_A, &idx);
    EXPECT(r == SAVE_MENU_RESULT_CLEAR_ALL, "A on Clear all returns CLEAR_ALL");
    save_tick(&menu, &store, &prev, 0, &idx);

    /* B returns BACK regardless of selection. */
    r = save_tick(&menu, &store, &prev, NES_BUTTON_B, &idx);
    EXPECT(r == SAVE_MENU_RESULT_BACK, "B returns BACK");

    /* save_menu_select_elapsed locates the entry with elapsed == 200. */
    save_menu_select_elapsed(&menu, &store, 200u);
    EXPECT(menu.selected == 1, "select_elapsed(200) selects entry 1");

    /* Fall back to 0 when no entry matches. */
    save_menu_select_elapsed(&menu, &store, 999999u);
    EXPECT(menu.selected == 0, "select_elapsed falls back to 0 on no match");

    save_tick(&menu, &store, &prev, 0, &idx);

    /* Select on a save-state entry (selected == 0) returns DELETE with
     * out_index set. */
    idx = -1;
    r = save_tick(&menu, &store, &prev, NES_BUTTON_SELECT, &idx);
    EXPECT(r == SAVE_MENU_RESULT_DELETE, "select on save entry returns DELETE");
    EXPECT(idx == 0, "delete index == 0");
    save_tick(&menu, &store, &prev, 0, &idx);

    /* Navigate to "Clear all save states" (index 3); Select there is a
     * no-op since it's not a save-state entry. */
    for (int i = 0; i < 3; ++i) {
        save_tick(&menu, &store, &prev, NES_BUTTON_DOWN, &idx);
        save_tick(&menu, &store, &prev, 0, &idx);
    }
    EXPECT(menu.selected == 3, "navigated to Clear all save states");
    idx = -1;
    r = save_tick(&menu, &store, &prev, NES_BUTTON_SELECT, &idx);
    EXPECT(r == SAVE_MENU_RESULT_NONE, "select on Clear all is a no-op");
    EXPECT(idx == -1, "out_index untouched for non-DELETE result");
    save_tick(&menu, &store, &prev, 0, &idx);

    EXPECT(store.clear_all(&store), "clear_all (menu test)");

    char dir_name[9];
    save_state_store_dir_name("Test Rom", dir_name);
    char rom_dir_path[1100];
    snprintf(rom_dir_path, sizeof(rom_dir_path), "%s/%s", tmpdir, dir_name);
    rmdir(rom_dir_path);
    rmdir(tmpdir);

    save_state_store_posix_destroy(&store);
}

/* --- Section D: full AppShell SAVE_MENU integration --- */

static void test_app_shell_save_flow(uint8_t *rom, size_t rom_size) {
    char tmpl[] = "/tmp/micrones_ss_d_XXXXXX";
    char *tmpdir = mkdtemp(tmpl);
    EXPECT(tmpdir != NULL, "mkdtemp for app shell test");
    if (tmpdir == NULL) return;

    SaveStateStore store;
    EXPECT(save_state_store_posix_init(&store, tmpdir), "posix store init (shell test)");

    FakeSource src;
    RomSource  rom_source;
    fake_init(&src, &rom_source, rom, rom_size);

    Nes nes;
    nes_init(&nes);

    AppShell shell;
    app_shell_init(&shell, &rom_source, &nes);
    app_shell_set_save_store(&shell, &store);

    EXPECT(shell.state == APP_SHELL_STATE_MENU, "shell starts in MENU");

    /* Frame 1: release everything so prev_buttons becomes 0. */
    shell_tick(&shell, 0);

    /* Frame 2: Start launches the (only, supported) entry. */
    AppShellFrame f = shell_tick(&shell, NES_BUTTON_START);
    EXPECT(f.just_entered_run, "start launches rom");
    EXPECT(shell.state == APP_SHELL_STATE_RUNNING, "shell now RUNNING");

    /* Advance a few frames so the running NES has diverged from reset. */
    for (int i = 0; i < 3; ++i) {
        EXPECT(nes_step_frame(shell.nes), "step running nes");
    }
    uint64_t frame_count_before_save = nes_frame_count(shell.nes);

    /* Frame 3: release everything (unlatches exit/save combos). */
    shell_tick(&shell, 0);

    /* Frame 4: Up+Start creates a save state. */
    f = shell_tick(&shell, (uint8_t)(NES_BUTTON_UP | NES_BUTTON_START));
    EXPECT(shell.loaded_from_save, "save state created (loaded_from_save)");
    EXPECT(store.refresh(&store, "Test Rom") == 1, "one save state on disk");

    /* Frame 5: release everything (unlatches save combo). */
    shell_tick(&shell, 0);

    /* Frame 6: Down+Start exits to the save-state menu (save_count > 0). */
    f = shell_tick(&shell, (uint8_t)(NES_BUTTON_DOWN | NES_BUTTON_START));
    EXPECT(f.just_entered_menu, "exit combo fires");
    EXPECT(shell.state == APP_SHELL_STATE_SAVE_MENU, "shell now SAVE_MENU");
    EXPECT(shell.save_menu.selected == 0, "save menu defaults to the just-made save");

    /* Frame 7: release everything. */
    shell_tick(&shell, 0);

    /* Frame 8: Start loads the selected save state. */
    f = shell_tick(&shell, NES_BUTTON_START);
    EXPECT(f.just_entered_run, "load re-enters RUNNING");
    EXPECT(shell.state == APP_SHELL_STATE_RUNNING, "shell back in RUNNING after load");
    EXPECT(shell.loaded_from_save, "loaded_from_save after load");
    EXPECT(nes_frame_count(shell.nes) == frame_count_before_save,
           "frame count restored from save state");

    /* Frame 9: release everything. */
    shell_tick(&shell, 0);

    /* Frame 10: exit again -> SAVE_MENU (still one save). */
    f = shell_tick(&shell, (uint8_t)(NES_BUTTON_DOWN | NES_BUTTON_START));
    EXPECT(shell.state == APP_SHELL_STATE_SAVE_MENU, "back in SAVE_MENU");

    /* Frame 11: release. */
    shell_tick(&shell, 0);

    /* Frame 12: Down moves selection to "Clear all save states" (index 1
     * with one save state present: 0=save, 1=clear-all, 2=back). */
    shell_tick(&shell, NES_BUTTON_DOWN);
    EXPECT(shell.save_menu.selected == 1, "selection moved to Clear all save states");

    /* Frame 13: release. */
    shell_tick(&shell, 0);

    /* Frame 14: Start on "Clear all save states" -> confirmation modal. */
    shell_tick(&shell, NES_BUTTON_START);
    EXPECT(shell.state == APP_SHELL_STATE_CONFIRM_CLEAR_SAVES, "confirm-clear modal shown");

    /* Frame 15: release. */
    shell_tick(&shell, 0);

    /* Frame 16: B cancels back to SAVE_MENU without clearing. */
    shell_tick(&shell, NES_BUTTON_B);
    EXPECT(shell.state == APP_SHELL_STATE_SAVE_MENU, "B cancels back to SAVE_MENU");
    EXPECT(store.refresh(&store, "Test Rom") == 1, "save state survives cancel");

    /* Frame 17: release. */
    shell_tick(&shell, 0);

    /* Frame 18: Start on "Clear all save states" again -> confirm modal. */
    shell_tick(&shell, NES_BUTTON_START);
    EXPECT(shell.state == APP_SHELL_STATE_CONFIRM_CLEAR_SAVES, "confirm-clear modal shown again");

    /* Frame 19: release. */
    shell_tick(&shell, 0);

    /* Frame 20: A confirms -> clears all saves and returns to the ROM menu. */
    shell_tick(&shell, NES_BUTTON_A);
    EXPECT(shell.state == APP_SHELL_STATE_MENU, "confirm-clear returns to MENU");
    EXPECT(!shell.loaded_from_save, "loaded_from_save cleared after Clear all");
    EXPECT(store.refresh(&store, "Test Rom") == 0, "no save states remain");

    app_shell_destroy(&shell);
    nes_destroy(&nes);
    save_state_store_posix_destroy(&store);

    char dir_name[9];
    save_state_store_dir_name("Test Rom", dir_name);
    char rom_dir_path[1100];
    snprintf(rom_dir_path, sizeof(rom_dir_path), "%s/%s", tmpdir, dir_name);
    rmdir(rom_dir_path);
    rmdir(tmpdir);
}

/* --- Section E: AppShell Select-to-delete confirmation flow --- */

static void test_app_shell_delete_save_state(uint8_t *rom, size_t rom_size) {
    char tmpl[] = "/tmp/micrones_ss_e_XXXXXX";
    char *tmpdir = mkdtemp(tmpl);
    EXPECT(tmpdir != NULL, "mkdtemp for delete-confirm test");
    if (tmpdir == NULL) return;

    SaveStateStore store;
    EXPECT(save_state_store_posix_init(&store, tmpdir), "posix store init (delete test)");

    FakeSource src;
    RomSource  rom_source;
    fake_init(&src, &rom_source, rom, rom_size);

    Nes nes;
    nes_init(&nes);

    AppShell shell;
    app_shell_init(&shell, &rom_source, &nes);
    app_shell_set_save_store(&shell, &store);

    /* Frame 1: release everything so prev_buttons becomes 0. */
    shell_tick(&shell, 0);

    /* Frame 2: Start launches the (only, supported) entry. */
    AppShellFrame f = shell_tick(&shell, NES_BUTTON_START);
    EXPECT(f.just_entered_run, "start launches rom");

    for (int i = 0; i < 3; ++i) {
        EXPECT(nes_step_frame(shell.nes), "step running nes");
    }

    /* Frame 3: release (unlatches exit/save combos). */
    shell_tick(&shell, 0);

    /* Frame 4: Up+Start creates the first save state (elapsed == 0). */
    shell_tick(&shell, (uint8_t)(NES_BUTTON_UP | NES_BUTTON_START));

    /* Frame 5: release (unlatches save combo). */
    shell_tick(&shell, 0);

    /* Frame 6: Up+Start creates a second save state.  It collides with
     * elapsed == 0 and is rewritten to elapsed == 1. */
    shell_tick(&shell, (uint8_t)(NES_BUTTON_UP | NES_BUTTON_START));
    EXPECT(shell.loaded_save_elapsed_seconds == 1u, "second save resolves to elapsed=1");

    /* Frame 7: release (unlatches save combo). */
    shell_tick(&shell, 0);

    /* Frame 8: Down+Start exits to the save-state menu (two saves present). */
    f = shell_tick(&shell, (uint8_t)(NES_BUTTON_DOWN | NES_BUTTON_START));
    EXPECT(f.just_entered_menu, "exit combo fires");
    EXPECT(shell.state == APP_SHELL_STATE_SAVE_MENU, "shell now SAVE_MENU");
    EXPECT(store.refresh(&store, "Test Rom") == 2, "two save states on disk");
    EXPECT(shell.save_menu.selected == 0, "save menu defaults to the just-made save (elapsed=1)");

    /* Frame 9: release. */
    shell_tick(&shell, 0);

    /* Frame 10: Select on entry 0 -> confirm-delete modal. */
    shell_tick(&shell, NES_BUTTON_SELECT);
    EXPECT(shell.state == APP_SHELL_STATE_CONFIRM_DELETE_SAVE, "confirm-delete modal shown");
    EXPECT(shell.pending_delete_index == 0, "pending delete index == 0");

    /* Frame 11: release. */
    shell_tick(&shell, 0);

    /* Frame 12: B cancels back to SAVE_MENU without deleting. */
    shell_tick(&shell, NES_BUTTON_B);
    EXPECT(shell.state == APP_SHELL_STATE_SAVE_MENU, "B cancels back to SAVE_MENU");
    EXPECT(store.refresh(&store, "Test Rom") == 2, "both saves survive cancel");

    /* Frame 13: release. */
    shell_tick(&shell, 0);

    /* Frame 14: Select on entry 0 again -> confirm-delete modal. */
    shell_tick(&shell, NES_BUTTON_SELECT);
    EXPECT(shell.state == APP_SHELL_STATE_CONFIRM_DELETE_SAVE, "confirm-delete modal shown again");

    /* Frame 15: release. */
    shell_tick(&shell, 0);

    /* Frame 16: A confirms -> deletes entry 0 (elapsed=1) and returns to SAVE_MENU. */
    shell_tick(&shell, NES_BUTTON_A);
    EXPECT(shell.state == APP_SHELL_STATE_SAVE_MENU, "A returns to SAVE_MENU");
    EXPECT(store.refresh(&store, "Test Rom") == 1, "one save state remains after delete");
    {
        const SaveStateEntry *e0 = store.entry(&store, 0);
        EXPECT(e0 != NULL && e0->elapsed_seconds == 0u, "remaining save is elapsed=0");
    }

    app_shell_destroy(&shell);
    nes_destroy(&nes);
    EXPECT(store.clear_all(&store), "clear_all (delete test)");
    save_state_store_posix_destroy(&store);

    char dir_name[9];
    save_state_store_dir_name("Test Rom", dir_name);
    char rom_dir_path[1100];
    snprintf(rom_dir_path, sizeof(rom_dir_path), "%s/%s", tmpdir, dir_name);
    rmdir(rom_dir_path);
    rmdir(tmpdir);
}

/* Reproduces "while in a save state, returning to the menu selects the next
 * entry instead of the one currently loaded".  The Down+Start exit combo
 * leaves Down held on the frame the save menu appears; save_menu_step()
 * must not mistake that held Down for a fresh "move down" press and bump
 * the selection away from the entry that was just re-highlighted. */
static void test_exit_combo_keeps_loaded_entry_selected(uint8_t *rom, size_t rom_size) {
    char tmpl[] = "/tmp/micrones_ss_repro_XXXXXX";
    char *tmpdir = mkdtemp(tmpl);
    EXPECT(tmpdir != NULL, "mkdtemp for exit-combo test");
    if (tmpdir == NULL) return;

    SaveStateStore store;
    EXPECT(save_state_store_posix_init(&store, tmpdir), "posix store init (exit-combo test)");
    store.refresh(&store, "Test Rom");

    {
        Nes cap_nes;
        nes_init(&cap_nes);
        EXPECT(nes_load_cartridge_const_memory(&cap_nes, rom, rom_size), "load rom for exit-combo captures");
        nes_reset(&cap_nes);
        uint32_t rom_checksum   = save_state_crc32(rom, rom_size);
        uint32_t rom_image_size = (uint32_t)rom_size;

        SaveStateBlob blob;
        save_state_capture(&cap_nes, rom_checksum, rom_image_size, 10u, &blob);
        EXPECT(store.save(&store, &blob), "save elapsed=10");
        save_state_capture(&cap_nes, rom_checksum, rom_image_size, 20u, &blob);
        EXPECT(store.save(&store, &blob), "save elapsed=20");
        save_state_capture(&cap_nes, rom_checksum, rom_image_size, 30u, &blob);
        EXPECT(store.save(&store, &blob), "save elapsed=30");
        nes_destroy(&cap_nes);
    }
    EXPECT(store.count(&store) == 3, "three saves present (exit-combo test)");

    FakeSource src;
    RomSource  rom_source;
    fake_init(&src, &rom_source, rom, rom_size);

    Nes nes;
    nes_init(&nes);

    AppShell shell;
    app_shell_init(&shell, &rom_source, &nes);
    app_shell_set_save_store(&shell, &store);

    shell_tick(&shell, 0);
    AppShellFrame f = shell_tick(&shell, NES_BUTTON_START);
    EXPECT(f.just_entered_run, "start launches rom (exit-combo test)");

    shell_tick(&shell, 0);
    f = shell_tick(&shell, (uint8_t)(NES_BUTTON_DOWN | NES_BUTTON_START));
    EXPECT(shell.state == APP_SHELL_STATE_SAVE_MENU, "shell now SAVE_MENU (exit-combo test)");
    EXPECT(shell.save_menu.selected == 0, "save menu defaults to 0 (exit-combo test)");

    shell_tick(&shell, 0);

    /* Move down to entry index 1 (elapsed=20, "Save 2"). */
    shell_tick(&shell, NES_BUTTON_DOWN);
    EXPECT(shell.save_menu.selected == 1, "moved to entry 1 (exit-combo test)");
    shell_tick(&shell, 0);

    /* Start loads entry 1 (elapsed=20). */
    f = shell_tick(&shell, NES_BUTTON_START);
    EXPECT(f.just_entered_run, "load re-enters RUNNING (exit-combo test)");
    EXPECT(shell.loaded_save_elapsed_seconds == 20u, "loaded elapsed == 20 (exit-combo test)");

    shell_tick(&shell, 0);

    /* Exit again -> SAVE_MENU should highlight the loaded entry (elapsed=20, index 1). */
    f = shell_tick(&shell, (uint8_t)(NES_BUTTON_DOWN | NES_BUTTON_START));
    EXPECT(shell.state == APP_SHELL_STATE_SAVE_MENU, "back in SAVE_MENU (exit-combo test)");
    EXPECT(shell.save_menu.selected == 1, "save menu re-selects entry 1 (elapsed=20) (exit-combo test)");

    /* The exit combo is typically still held for at least one more frame.
     * That held Down must not be read as a fresh "move down" press. */
    shell_tick(&shell, (uint8_t)(NES_BUTTON_DOWN | NES_BUTTON_START));
    EXPECT(shell.save_menu.selected == 1, "selection unchanged while combo still held (exit-combo test)");

    /* Once the combo is released, the selection should still be entry 1. */
    shell_tick(&shell, 0);
    EXPECT(shell.save_menu.selected == 1, "selection unchanged after combo released (exit-combo test)");

    app_shell_destroy(&shell);
    nes_destroy(&nes);
    EXPECT(store.clear_all(&store), "clear_all (exit-combo test)");
    save_state_store_posix_destroy(&store);

    char dir_name[9];
    save_state_store_dir_name("Test Rom", dir_name);
    char rom_dir_path[1100];
    snprintf(rom_dir_path, sizeof(rom_dir_path), "%s/%s", tmpdir, dir_name);
    rmdir(rom_dir_path);
    rmdir(tmpdir);
}

int main(void) {
    static uint8_t rom[16 + 32768];
    build_synthetic_rom(rom, sizeof(rom));

    test_capture_apply_roundtrip(rom, sizeof(rom));
    test_posix_store_roundtrip();
    test_save_menu_navigation();
    test_app_shell_save_flow(rom, sizeof(rom));
    test_app_shell_delete_save_state(rom, sizeof(rom));
    test_exit_combo_keeps_loaded_entry_selected(rom, sizeof(rom));

    if (g_failures == 0) {
        printf("save_state_smoke: OK\n");
        return 0;
    }
    fprintf(stderr, "save_state_smoke: %d failure(s)\n", g_failures);
    return 1;
}
