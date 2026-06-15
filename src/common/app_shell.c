#include "app_shell.h"

#include "save_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define EXIT_COMBO_MASK   (NES_BUTTON_DOWN | NES_BUTTON_START)
#define SAVE_COMBO_MASK   (NES_BUTTON_UP   | NES_BUTTON_START)

static void shell_set_status(AppShell *shell, const char *fmt, ...) {
    if (shell == NULL) {
        return;
    }
    if (fmt == NULL) {
        shell->status[0] = '\0';
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(shell->status, sizeof(shell->status), fmt, ap);
    va_end(ap);
}

static void shell_unload_running(AppShell *shell) {
    if (shell == NULL || shell->nes == NULL) {
        return;
    }
    /* Persist battery-backed WRAM if the entry supports it.  Best-effort:
     * a write failure is logged into status but does not block the
     * transition back to the menu. */
    if (shell->source != NULL && shell->running_index >= 0 &&
        shell->nes->cartridge.has_battery && shell->source->save_store != NULL) {
        if (!shell->source->save_store(shell->source, (size_t)shell->running_index,
                                       shell->nes->wram, sizeof(shell->nes->wram))) {
            shell_set_status(shell, "Save write failed");
        }
    }
    nes_destroy(shell->nes);
    nes_init(shell->nes);

    /* The cart was loaded zero-copy via nes_load_cartridge_const_memory, so
     * cart_unload (called by nes_destroy) did not free our buffer.  Release
     * it now that the cart no longer points into it. */
    if (shell->current_rom_buf != NULL && shell->source != NULL) {
        shell->source->free_buf(shell->source, shell->current_rom_buf);
    }
    shell->current_rom_buf = NULL;
    shell->current_rom_size = 0;
    shell->running_index = -1;
}

static bool shell_launch(AppShell *shell, int index) {
    if (shell == NULL || shell->source == NULL || shell->nes == NULL) {
        return false;
    }
    size_t n = shell->source->count(shell->source);
    if (index < 0 || (size_t)index >= n) {
        shell_set_status(shell, "Invalid selection");
        return false;
    }
    const RomSourceEntry *entry = shell->source->entry(shell->source, (size_t)index);

    uint8_t *buf = NULL;
    size_t   sz  = 0;
    char     err[160];
    err[0] = '\0';
    if (!shell->source->load(shell->source, (size_t)index, &buf, &sz, err, sizeof(err))) {
        shell_set_status(shell, "Load failed: %s", err[0] ? err : "unknown error");
        return false;
    }

    /* Fresh NES state for the new ROM. */
    nes_destroy(shell->nes);
    nes_init(shell->nes);

    /* Use the zero-copy cart loader so the cart points directly into our
     * buffer.  This saves a transient peak of ROM-size bytes during launch,
     * which matters on the Pico where SRAM is tight. */
    if (!nes_load_cartridge_const_memory(shell->nes, buf, sz)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s", nes_last_error(shell->nes));
        shell->source->free_buf(shell->source, buf);
        shell_set_status(shell, "Load failed: %s", msg[0] ? msg : "unknown error");
        return false;
    }

    /* Hold onto the source buffer until the cart is unloaded.  Zero-copy CHR
     * data still aliases this region, and some platforms may also keep PRG
     * there if the optional SRAM copy fails. */
    shell->current_rom_buf  = buf;
    shell->current_rom_size = sz;

    nes_reset(shell->nes);

    /* Restore battery-backed WRAM if available. */
    if (shell->nes->cartridge.has_battery && shell->source->save_load != NULL) {
        (void)shell->source->save_load(shell->source, (size_t)index,
                                       shell->nes->wram, sizeof(shell->nes->wram));
    }

    shell->running_index = index;
    shell->state = APP_SHELL_STATE_RUNNING;
    /* Latch the exit/save combos so the user pressing Start to launch and
     * still holding Down/Up doesn't immediately re-trigger. */
    shell->exit_combo_latched = true;
    shell->save_combo_latched = true;
    /* A fresh launch starts from the ROM's reset state, not a save. */
    shell->loaded_from_save = false;
    shell->loaded_save_elapsed_seconds = 0;
    shell_set_status(shell, "");
    return true;
}

/* CRC32 over the currently-loaded ROM image, for SaveStateHeader.rom_checksum.
 * Returns 0 if no ROM is loaded (current_rom_buf == NULL). */
static uint32_t shell_rom_checksum(const AppShell *shell) {
    if (shell == NULL || shell->current_rom_buf == NULL) {
        return 0;
    }
    return save_state_crc32(shell->current_rom_buf, shell->current_rom_size);
}

/* Capture and persist a new save state for the running ROM. */
static void shell_create_save_state(AppShell *shell) {
    if (shell == NULL || shell->nes == NULL || shell->save_store == NULL ||
        shell->source == NULL || shell->running_index < 0) {
        return;
    }
    const RomSourceEntry *entry = shell->source->entry(shell->source, (size_t)shell->running_index);
    if (entry == NULL) {
        return;
    }

    uint32_t rom_checksum   = shell_rom_checksum(shell);
    uint32_t rom_image_size = (uint32_t)shell->current_rom_size;
    uint32_t elapsed_seconds = (uint32_t)(nes_frame_count(shell->nes) / 60u);

    SaveStateBlob blob;
    save_state_capture(shell->nes, rom_checksum, rom_image_size, elapsed_seconds, &blob);

    shell->save_store->refresh(shell->save_store, entry->name);
    if (shell->save_store->save(shell->save_store, &blob)) {
        shell->loaded_from_save = true;
        shell->loaded_save_elapsed_seconds = elapsed_seconds;
        char label[12];
        save_state_format_elapsed(elapsed_seconds, label, sizeof(label));
        shell_set_status(shell, "Saved %s", label);
    } else {
        shell_set_status(shell, "Save failed");
    }
}

static void shell_decide_state(AppShell *shell) {
    if (shell->source != NULL && shell->source->count(shell->source) > 0) {
        shell->state = APP_SHELL_STATE_MENU;
    } else if (shell->import_source != NULL && shell->import_fn != NULL) {
        shell->state = APP_SHELL_STATE_IMPORT;
    } else {
        shell->state = APP_SHELL_STATE_MENU;
    }
}

static const char *shell_save_menu_rom_name(AppShell *shell) {
    if (shell->save_menu_rom_index < 0 || shell->source == NULL) {
        return "";
    }
    const RomSourceEntry *e = shell->source->entry(shell->source,
                                                    (size_t)shell->save_menu_rom_index);
    return e != NULL ? e->name : "";
}

static void shell_render_current(AppShell *shell) {
    if (shell->state == APP_SHELL_STATE_IMPORT) {
        rom_menu_render_import(&shell->menu, shell->import_source,
                               &shell->menu_fb, shell->status);
    } else if (shell->state == APP_SHELL_STATE_CONFIRM_ERASE) {
        rom_menu_render_confirm(&shell->menu_fb,
                                "Erase all ROMs from flash?",
                                "This cannot be undone.");
    } else if (shell->state == APP_SHELL_STATE_SAVE_MENU) {
        save_menu_render(&shell->save_menu, shell->save_store, &shell->menu_fb,
                         shell_save_menu_rom_name(shell), shell->status);
    } else if (shell->state == APP_SHELL_STATE_CONFIRM_CLEAR_SAVES) {
        rom_menu_render_confirm(&shell->menu_fb,
                                "Clear all save states?",
                                "This cannot be undone.");
    } else {
        rom_menu_render(&shell->menu, shell->source,
                        &shell->menu_fb, shell->status);
    }
}

static void shell_absorb_menu_direction(AppShell *shell, uint8_t buttons) {
    if (shell == NULL) {
        return;
    }

    bool up_held = (buttons & NES_BUTTON_UP) != 0u;
    bool down_held = (buttons & NES_BUTTON_DOWN) != 0u;
    if (up_held && !down_held) {
        shell->menu.hold_dir = -1;
        shell->menu.hold_frames = 1;
    } else if (down_held && !up_held) {
        shell->menu.hold_dir = +1;
        shell->menu.hold_frames = 1;
    } else {
        shell->menu.hold_dir = 0;
        shell->menu.hold_frames = 0;
    }
}

static void shell_return_to_menu(AppShell *shell, uint8_t buttons) {
    if (shell == NULL) {
        return;
    }

    int previous_index = shell->running_index;
    shell_unload_running(shell);
    shell_set_status(shell, "");
    shell_decide_state(shell);
    if (shell->state == APP_SHELL_STATE_MENU && previous_index >= 0) {
        rom_menu_select(&shell->menu, shell->source, previous_index);
        shell_absorb_menu_direction(shell, buttons);
    }
    shell_render_current(shell);
}

/* Exit the running ROM via the Down+Start combo.  If the ROM has save
 * states, go to the save-state list (defaulting to the entry the running
 * ROM was loaded from, if any) instead of the main ROM menu. */
static void shell_exit_running(AppShell *shell, uint8_t buttons) {
    if (shell == NULL) {
        return;
    }

    int previous_index = shell->running_index;
    char rom_name[ROM_SOURCE_NAME_MAX];
    rom_name[0] = '\0';
    if (previous_index >= 0 && shell->source != NULL) {
        const RomSourceEntry *e = shell->source->entry(shell->source, (size_t)previous_index);
        if (e != NULL) {
            snprintf(rom_name, sizeof(rom_name), "%s", e->name);
        }
    }

    bool     loaded_from_save  = shell->loaded_from_save;
    uint32_t loaded_elapsed    = shell->loaded_save_elapsed_seconds;

    size_t save_count = 0;
    if (shell->save_store != NULL && rom_name[0] != '\0') {
        save_count = shell->save_store->refresh(shell->save_store, rom_name);
    }

    if (save_count == 0) {
        shell_return_to_menu(shell, buttons);
        return;
    }

    shell_unload_running(shell);
    shell_set_status(shell, "");
    shell->save_menu_rom_index = previous_index;
    if (loaded_from_save) {
        save_menu_select_elapsed(&shell->save_menu, shell->save_store, loaded_elapsed);
    } else {
        save_menu_init(&shell->save_menu);
    }
    shell->state = APP_SHELL_STATE_SAVE_MENU;
    shell_render_current(shell);
}

void app_shell_init(AppShell *shell, RomSource *source, Nes *nes) {
    if (shell == NULL) {
        return;
    }
    memset(shell, 0, sizeof(*shell));
    shell->source = source;
    shell->nes = nes;
    shell->running_index = -1;
    shell->save_menu_rom_index = -1;
    shell->prev_buttons = 0xFFu;
    rom_menu_init(&shell->menu);
    if (source != NULL && source->refresh != NULL) {
        source->refresh(source);
    }
    shell_decide_state(shell);
    shell_render_current(shell);
}

void app_shell_set_import(AppShell *shell,
                          RomSource *import_source,
                          AppShellImportFn import_fn,
                          void *import_fn_user) {
    if (shell == NULL) return;
    shell->import_source  = import_source;
    shell->import_fn      = import_fn;
    shell->import_fn_user = import_fn_user;
    shell->menu.show_flash_actions =
        (import_fn != NULL || shell->erase_fn != NULL);
    shell_decide_state(shell);
    shell_render_current(shell);
}

void app_shell_set_erase(AppShell *shell,
                         AppShellEraseFn erase_fn,
                         void *erase_fn_user) {
    if (shell == NULL) return;
    shell->erase_fn      = erase_fn;
    shell->erase_fn_user = erase_fn_user;
    shell->menu.show_flash_actions =
        (shell->import_fn != NULL || erase_fn != NULL);
    shell_decide_state(shell);
    shell_render_current(shell);
}

void app_shell_set_save_store(AppShell *shell, SaveStateStore *save_store) {
    if (shell == NULL) return;
    shell->save_store = save_store;
}

void app_shell_destroy(AppShell *shell) {
    if (shell == NULL) {
        return;
    }
    if (shell->state == APP_SHELL_STATE_RUNNING) {
        shell_unload_running(shell);
    }
}

bool app_shell_in_menu(const AppShell *shell) {
    return shell != NULL && (shell->state == APP_SHELL_STATE_MENU ||
                             shell->state == APP_SHELL_STATE_IMPORT ||
                             shell->state == APP_SHELL_STATE_CONFIRM_ERASE ||
                             shell->state == APP_SHELL_STATE_SAVE_MENU ||
                             shell->state == APP_SHELL_STATE_CONFIRM_CLEAR_SAVES);
}

const NesFrameBuffer *app_shell_menu_framebuffer(const AppShell *shell) {
    return shell != NULL ? &shell->menu_fb : NULL;
}

const char *app_shell_status(const AppShell *shell) {
    return shell != NULL ? shell->status : "";
}

void app_shell_request_menu(AppShell *shell) {
    if (shell == NULL || shell->state == APP_SHELL_STATE_MENU ||
        shell->state == APP_SHELL_STATE_IMPORT ||
        shell->state == APP_SHELL_STATE_CONFIRM_ERASE) {
        return;
    }
    shell_return_to_menu(shell, 0u);
    shell->exit_combo_latched = false;
}

void app_shell_request_reset(AppShell *shell) {
    if (shell == NULL || shell->state != APP_SHELL_STATE_RUNNING ||
        shell->nes == NULL) {
        return;
    }
    nes_reset(shell->nes);
}

void app_shell_render_menu(AppShell *shell) {
    if (shell == NULL) {
        return;
    }
    shell_render_current(shell);
}

AppShellFrame app_shell_begin_frame(AppShell *shell, NesControllerState input) {
    AppShellFrame out;
    out.stepping_nes = false;
    out.just_entered_menu = false;
    out.just_entered_run = false;
    out.forwarded = input;

    if (shell == NULL) {
        return out;
    }

    uint8_t prev = shell->prev_buttons;
    uint8_t curr = input.buttons;

    if (shell->state == APP_SHELL_STATE_CONFIRM_ERASE) {
        uint8_t pressed = (uint8_t)(~prev & curr);
        if ((pressed & NES_BUTTON_A) != 0u && shell->erase_fn != NULL) {
            bool ok = shell->erase_fn(shell->source, shell->erase_fn_user);
            if (ok) {
                if (shell->source != NULL && shell->source->refresh != NULL)
                    shell->source->refresh(shell->source);
                shell_set_status(shell, "Flash erased");
                rom_menu_init(&shell->menu);
                shell->menu.show_flash_actions =
                    (shell->import_fn != NULL || shell->erase_fn != NULL);
                shell_decide_state(shell);
            } else {
                shell_set_status(shell, "Erase failed");
                shell->state = APP_SHELL_STATE_MENU;
            }
        } else if ((pressed & NES_BUTTON_B) != 0u) {
            shell_set_status(shell, "");
            shell->state = APP_SHELL_STATE_MENU;
        }
        shell_render_current(shell);
        shell->prev_buttons = curr;
        return out;
    }

    if (shell->state == APP_SHELL_STATE_CONFIRM_CLEAR_SAVES) {
        uint8_t pressed = (uint8_t)(~prev & curr);
        if ((pressed & NES_BUTTON_A) != 0u) {
            if (shell->save_store != NULL) {
                shell->save_store->clear_all(shell->save_store);
            }
            shell->loaded_from_save = false;
            shell_set_status(shell, "Save states cleared");
            shell_decide_state(shell);
            if (shell->state == APP_SHELL_STATE_MENU) {
                rom_menu_select(&shell->menu, shell->source, shell->save_menu_rom_index);
            }
        } else if ((pressed & NES_BUTTON_B) != 0u) {
            shell_set_status(shell, "");
            shell->state = APP_SHELL_STATE_SAVE_MENU;
        }
        shell_render_current(shell);
        shell->prev_buttons = curr;
        return out;
    }

    if (shell->state == APP_SHELL_STATE_SAVE_MENU) {
        int chosen = -1;
        SaveMenuResult r = save_menu_step(&shell->save_menu, shell->save_store,
                                          prev, curr, &chosen);
        if (r == SAVE_MENU_RESULT_LOAD) {
            SaveStateBlob blob;
            if (shell->save_store != NULL &&
                shell->save_store->load(shell->save_store, (size_t)chosen, &blob) &&
                shell_launch(shell, shell->save_menu_rom_index)) {
                uint32_t rom_checksum   = shell_rom_checksum(shell);
                uint32_t rom_image_size = (uint32_t)shell->current_rom_size;
                if (save_state_apply(shell->nes, &blob, rom_checksum, rom_image_size)) {
                    shell->loaded_from_save = true;
                    shell->loaded_save_elapsed_seconds = blob.header.elapsed_seconds;
                    out.just_entered_run = true;
                    out.stepping_nes = true;
                    NesControllerState forwarded = { 0 };
                    out.forwarded = forwarded;
                } else {
                    shell_unload_running(shell);
                    shell->state = APP_SHELL_STATE_SAVE_MENU;
                    shell_set_status(shell, "Load failed: bad save");
                }
            } else {
                shell_set_status(shell, "Load failed");
            }
        } else if (r == SAVE_MENU_RESULT_CLEAR_ALL) {
            shell->state = APP_SHELL_STATE_CONFIRM_CLEAR_SAVES;
        } else if (r == SAVE_MENU_RESULT_BACK) {
            shell_set_status(shell, "");
            shell_decide_state(shell);
            if (shell->state == APP_SHELL_STATE_MENU) {
                rom_menu_select(&shell->menu, shell->source, shell->save_menu_rom_index);
            }
        }
        shell_render_current(shell);
        shell->prev_buttons = curr;
        return out;
    }

    if (shell->state == APP_SHELL_STATE_IMPORT) {
        RomMenuResult r = rom_menu_step_import(&shell->menu,
                                               shell->import_source,
                                               prev, curr);
        if (r == ROM_MENU_RESULT_IMPORT && shell->import_fn != NULL) {
            bool ok = shell->import_fn(shell->source, shell->import_source,
                                       shell->import_fn_user);
            if (ok) {
                if (shell->source != NULL && shell->source->refresh != NULL)
                    shell->source->refresh(shell->source);
                shell_set_status(shell, "");
                rom_menu_init(&shell->menu);
                shell->menu.show_flash_actions =
                    (shell->import_fn != NULL || shell->erase_fn != NULL);
                shell_decide_state(shell);
            } else {
                shell_set_status(shell, "Copy failed");
            }
        }
        shell_render_current(shell);
        shell->prev_buttons = curr;
        return out;
    }

    if (shell->state == APP_SHELL_STATE_MENU) {
        int chosen = -1;
        RomMenuResult r = rom_menu_step(&shell->menu, shell->source,
                                        prev, curr, &chosen);
        if (r == ROM_MENU_RESULT_ERASE_FLASH) {
            shell->state = APP_SHELL_STATE_CONFIRM_ERASE;
            shell_render_current(shell);
            shell->prev_buttons = curr;
            return out;
        } else if (r == ROM_MENU_RESULT_IMPORT && shell->import_fn != NULL) {
            if (shell->import_source != NULL &&
                shell->import_source->refresh != NULL) {
                shell->import_source->refresh(shell->import_source);
            }
            bool ok = shell->import_fn(shell->source, shell->import_source,
                                       shell->import_fn_user);
            if (ok) {
                if (shell->source != NULL && shell->source->refresh != NULL)
                    shell->source->refresh(shell->source);
                shell_set_status(shell, "");
                rom_menu_init(&shell->menu);
                shell->menu.show_flash_actions =
                    (shell->import_fn != NULL || shell->erase_fn != NULL);
                shell_decide_state(shell);
            } else {
                shell_set_status(shell, "Copy failed");
            }
            shell_render_current(shell);
            shell->prev_buttons = curr;
            return out;
        } else if (r == ROM_MENU_RESULT_LAUNCH) {
            if (shell_launch(shell, chosen)) {
                out.just_entered_run = true;
                out.stepping_nes = true;
                /* Don't forward this frame's Start press into the game. */
                NesControllerState forwarded = { 0 };
                out.forwarded = forwarded;
            }
        } else if (r == ROM_MENU_RESULT_LAUNCH_BLOCKED) {
            const RomSourceEntry *e = (chosen >= 0 && shell->source != NULL)
                ? shell->source->entry(shell->source, (size_t)chosen)
                : NULL;
            if (e != NULL && e->mapper != 0xFFFFu) {
                shell_set_status(shell, "Mapper %u not supported", (unsigned)e->mapper);
            } else {
                shell_set_status(shell, "Could not parse iNES header");
            }
        } else {
            /* Clear stale status once the user navigates. */
            uint8_t pressed = (uint8_t)(~prev & curr);
            if ((pressed & (NES_BUTTON_UP | NES_BUTTON_DOWN |
                            NES_BUTTON_LEFT | NES_BUTTON_RIGHT)) != 0u) {
                shell->status[0] = '\0';
            }
        }
        rom_menu_render(&shell->menu, shell->source, &shell->menu_fb, shell->status);
        shell->prev_buttons = curr;
        return out;
    }

    /* RUNNING state: detect exit and save combos. */
    bool combo_now = (curr & EXIT_COMBO_MASK) == EXIT_COMBO_MASK;
    bool combo_prev = (prev & EXIT_COMBO_MASK) == EXIT_COMBO_MASK;

    if (shell->exit_combo_latched) {
        /* Wait until the combo is fully released at least once before we
         * accept a new exit press.  Avoids "I just launched and was still
         * holding Start". */
        if (!combo_now) {
            shell->exit_combo_latched = false;
        }
    } else if (combo_now && !combo_prev) {
        /* Edge-trigger the exit. */
        shell_exit_running(shell, curr);
        out.just_entered_menu = true;
        out.stepping_nes = false;
        NesControllerState forwarded = { 0 };
        out.forwarded = forwarded;
        shell->prev_buttons = curr;
        return out;
    }

    bool save_combo_now = (curr & SAVE_COMBO_MASK) == SAVE_COMBO_MASK;
    bool save_combo_prev = (prev & SAVE_COMBO_MASK) == SAVE_COMBO_MASK;

    if (shell->save_store != NULL) {
        if (shell->save_combo_latched) {
            /* Same "wait for release" latch as the exit combo. */
            if (!save_combo_now) {
                shell->save_combo_latched = false;
            }
        } else if (save_combo_now && !save_combo_prev) {
            /* Edge-trigger creating a save state. */
            shell_create_save_state(shell);
            shell->save_combo_latched = true;
            out.stepping_nes = true;
            out.forwarded.buttons = (uint8_t)(curr & ~SAVE_COMBO_MASK);
            shell->prev_buttons = curr;
            return out;
        }
    }

    out.stepping_nes = true;
    /* Mask out the combo bits while held so the game doesn't see a spurious
     * Down+Start or Up+Start. */
    if (combo_now) {
        out.forwarded.buttons = (uint8_t)(out.forwarded.buttons & ~EXIT_COMBO_MASK);
    }
    if (shell->save_store != NULL && save_combo_now) {
        out.forwarded.buttons = (uint8_t)(out.forwarded.buttons & ~SAVE_COMBO_MASK);
    }

    shell->prev_buttons = curr;
    return out;
}
