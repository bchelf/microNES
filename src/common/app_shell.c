#include "app_shell.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define EXIT_COMBO_MASK (NES_BUTTON_DOWN | NES_BUTTON_START)

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
    /* Latch the exit combo so the user pressing Start to launch and still
     * holding Down doesn't immediately bounce back to the menu. */
    shell->exit_combo_latched = true;
    shell_set_status(shell, "");
    return true;
}

void app_shell_init(AppShell *shell, RomSource *source, Nes *nes) {
    if (shell == NULL) {
        return;
    }
    memset(shell, 0, sizeof(*shell));
    shell->source = source;
    shell->nes = nes;
    shell->running_index = -1;
    shell->state = APP_SHELL_STATE_MENU;
    /* Treat the very first controller read as already-pressed for every
     * button, so no spurious "press event" fires before the user has
     * touched anything.  Without this, a flaky controller bus that
     * reads 0xFF (all-pressed) on first poll — which is exactly what
     * we see at boot when the 4021 hasn't fully stabilised — would
     * edge-trigger Start and instant-launch the first ROM. */
    shell->prev_buttons = 0xFFu;
    rom_menu_init(&shell->menu);
    if (source != NULL && source->refresh != NULL) {
        source->refresh(source);
    }
    /* Pre-render so the first present has content. */
    rom_menu_render(&shell->menu, shell->source, &shell->menu_fb, shell->status);
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
    return shell != NULL && shell->state == APP_SHELL_STATE_MENU;
}

const NesFrameBuffer *app_shell_menu_framebuffer(const AppShell *shell) {
    return shell != NULL ? &shell->menu_fb : NULL;
}

const char *app_shell_status(const AppShell *shell) {
    return shell != NULL ? shell->status : "";
}

void app_shell_request_menu(AppShell *shell) {
    if (shell == NULL || shell->state == APP_SHELL_STATE_MENU) {
        return;
    }
    shell_unload_running(shell);
    shell->state = APP_SHELL_STATE_MENU;
    shell->exit_combo_latched = false;
    rom_menu_render(&shell->menu, shell->source, &shell->menu_fb, shell->status);
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
    rom_menu_render(&shell->menu, shell->source, &shell->menu_fb, shell->status);
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

    if (shell->state == APP_SHELL_STATE_MENU) {
        int chosen = -1;
        RomMenuResult r = rom_menu_step(&shell->menu, shell->source,
                                        prev, curr, &chosen);
        if (r == ROM_MENU_RESULT_LAUNCH) {
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

    /* RUNNING state: detect exit combo. */
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
        shell_unload_running(shell);
        shell->state = APP_SHELL_STATE_MENU;
        shell_set_status(shell, "");
        rom_menu_render(&shell->menu, shell->source, &shell->menu_fb, shell->status);
        out.just_entered_menu = true;
        out.stepping_nes = false;
        NesControllerState forwarded = { 0 };
        out.forwarded = forwarded;
        shell->prev_buttons = curr;
        return out;
    }

    out.stepping_nes = true;
    /* Mask out the combo bits while it's held so the game doesn't see a
     * spurious Down+Start. */
    if (combo_now) {
        out.forwarded.buttons = (uint8_t)(curr & ~EXIT_COMBO_MASK);
    }

    shell->prev_buttons = curr;
    return out;
}
