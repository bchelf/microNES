#ifndef MICRONES_APP_SHELL_H
#define MICRONES_APP_SHELL_H

#include "framebuffer.h"
#include "input.h"
#include "nes.h"
#include "rom_menu.h"
#include "rom_source.h"

#include <stdbool.h>

typedef enum {
    APP_SHELL_STATE_MENU = 0,
    APP_SHELL_STATE_RUNNING,
    APP_SHELL_STATE_IMPORT,
    APP_SHELL_STATE_CONFIRM_ERASE,
    APP_SHELL_STATE_SAVE_MENU,
    APP_SHELL_STATE_CONFIRM_CLEAR_SAVES,
} AppShellState;

typedef bool (*AppShellImportFn)(RomSource *flash_source,
                                RomSource *sd_source,
                                void *user);
typedef bool (*AppShellEraseFn)(RomSource *flash_source, void *user);

typedef struct {
    AppShellState state;
    RomSource    *source;
    RomSource    *import_source;
    Nes          *nes;
    RomMenu       menu;
    NesFrameBuffer menu_fb;

    /* Edge-detection: previous frame's controller buttons. */
    uint8_t       prev_buttons;
    /* The exit combo (Down+Start) latches "wait for release" so we don't
     * immediately re-trigger when the user lets go after returning. */
    bool          exit_combo_latched;
    /* The create-save-state combo (Up+Start) latches the same way. */
    bool          save_combo_latched;

    /* Index of the running ROM, -1 when nothing is running. */
    int           running_index;

    /* Save-state storage (may be NULL to disable save states entirely). */
    SaveStateStore *save_store;
    /* Save-state list menu, shown in APP_SHELL_STATE_SAVE_MENU. */
    SaveMenu        save_menu;
    /* Index into `source` of the ROM whose save-state list is shown in
     * SAVE_MENU / CONFIRM_CLEAR_SAVES. */
    int             save_menu_rom_index;
    /* True if the running ROM was launched by loading a save state (vs. a
     * fresh launch).  loaded_save_elapsed_seconds is then the elapsed time
     * recorded in that save, used to re-select it if the user returns to
     * the save-state menu. */
    bool            loaded_from_save;
    uint32_t        loaded_save_elapsed_seconds;

    /* The cart was loaded zero-copy via nes_load_cartridge_const_memory and
     * the cart's prg_rom/chr_data pointers alias into this buffer.  Held
     * here until the cart is unloaded; freed via source->free_buf. */
    uint8_t      *current_rom_buf;
    size_t        current_rom_size;

    AppShellImportFn import_fn;
    void            *import_fn_user;
    AppShellEraseFn  erase_fn;
    void            *erase_fn_user;

    /* Transient text shown above the menu footer (e.g. error from a load
     * attempt).  Cleared on the next selection change. */
    char          status[160];
} AppShell;

/* Initialize the shell.  source must outlive the shell.  nes must be a
 * fully-initialized Nes that the shell will manage (load/reset/destroy as
 * the user picks ROMs).  Calls source->refresh() if non-NULL.
 *
 * import_source (may be NULL) is an SD-card source used when the primary
 * source has zero entries — the shell enters import mode, showing the SD
 * card contents greyed out and offering to copy them into flash.
 *
 * import_fn (may be NULL) is called when the user triggers the import.
 * It receives the primary source, import_source, and import_fn_user.  */
void app_shell_init(AppShell *shell, RomSource *source, Nes *nes);

void app_shell_set_import(AppShell *shell,
                          RomSource *import_source,
                          AppShellImportFn import_fn,
                          void *import_fn_user);

void app_shell_set_erase(AppShell *shell,
                         AppShellEraseFn erase_fn,
                         void *erase_fn_user);

/* Provide a save-state store.  May be left unset (NULL) to disable
 * save-state support: Down+Start then always returns straight to the ROM
 * menu (the pre-save-state behavior) and Up+Start is a no-op. */
void app_shell_set_save_store(AppShell *shell, SaveStateStore *save_store);

/* Tear down the shell, freeing any in-flight ROM buffer.  Does not destroy
 * the NES (caller owns it). */
void app_shell_destroy(AppShell *shell);

/* True when the shell wants the host to display its menu framebuffer
 * instead of the NES output. */
bool app_shell_in_menu(const AppShell *shell);

/* The framebuffer the menu has rendered into.  Valid whenever in_menu(). */
const NesFrameBuffer *app_shell_menu_framebuffer(const AppShell *shell);

/* Process input for the current frame.
 *
 * In MENU: drives navigation, may transition to RUNNING by loading a ROM
 *          and resetting the NES.  When transitioning to RUNNING this
 *          function returns the controller state to forward to the NES
 *          for the just-starting frame; otherwise the host should NOT
 *          step the NES this frame.
 *
 * In RUNNING: detects the exit combo (Down+Start, instant) and on hit,
 *             transitions back to MENU.  When staying in RUNNING returns
 *             the controller state the host should pass to the NES.
 *
 * The caller passes the raw controller state read from input devices.  The
 * shell strips Down+Start from the returned state on the frame the combo
 * fires so the game does not see the spurious press. */
typedef struct {
    bool stepping_nes;        /* host should step the NES this frame */
    bool just_entered_menu;   /* this frame transitioned RUNNING -> MENU */
    bool just_entered_run;    /* this frame transitioned MENU -> RUNNING */
    NesControllerState forwarded; /* controller state to pass to the NES */
} AppShellFrame;

AppShellFrame app_shell_begin_frame(AppShell *shell, NesControllerState input);

/* Render the menu into menu_fb.  Call only when in_menu().  Idempotent — the
 * shell does not auto-render, so the host can choose when to repaint. */
void app_shell_render_menu(AppShell *shell);

/* Force a return to the menu from any state (e.g. host received a hotkey).
 * No-op when already in the menu. */
void app_shell_request_menu(AppShell *shell);

/* Reset the currently running ROM without unloading it.  No-op in the menu. */
void app_shell_request_reset(AppShell *shell);

/* Last error/status text (for host logging or display). */
const char *app_shell_status(const AppShell *shell);

#endif
