#include "nes_hw_controller.h"
#include "board.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us – cycle-accurate busy-wait

// ── GPIO init ────────────────────────────────────────────────────────────

void nes_hw_controller_init(void)
{
    // CLOCK and LATCH are push-pull outputs, idle low.
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << BOARD_NES_CTL_CLOCK) | (1ULL << BOARD_NES_CTL_LATCH),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);
    gpio_set_level(BOARD_NES_CTL_CLOCK, 0);
    gpio_set_level(BOARD_NES_CTL_LATCH, 0);

    // DATA is an input.  The 4021 Q8 output is actively driven, but enabling
    // the internal pull-up protects against a floating line when no controller
    // is connected (all reads return 0xFF = no buttons pressed).
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << BOARD_NES_CTL_DATA),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);
}

// ── Read cycle ───────────────────────────────────────────────────────────

NesControllerState nes_hw_controller_read(void)
{
    // Step 1: latch all eight button states into the 4021 shift register.
    gpio_set_level(BOARD_NES_CTL_LATCH, 1);
    esp_rom_delay_us(NES_HW_CTL_LATCH_US);
    gpio_set_level(BOARD_NES_CTL_LATCH, 0);
    esp_rom_delay_us(NES_HW_CTL_CLOCK_US);   // settle before first sample

    // Step 2: shift out 8 bits.
    // DATA is active-low: 0 = button pressed, 1 = released.
    // Button order matches the NES 4021 wiring (A first, RIGHT last).
    static const uint8_t k_map[8] = {
        NES_BUTTON_A,      NES_BUTTON_B,    NES_BUTTON_SELECT, NES_BUTTON_START,
        NES_BUTTON_UP,     NES_BUTTON_DOWN, NES_BUTTON_LEFT,   NES_BUTTON_RIGHT,
    };

    uint8_t buttons = 0;
    for (int i = 0; i < 8; ++i) {
        if (!gpio_get_level(BOARD_NES_CTL_DATA)) {
            buttons |= k_map[i];
        }
        // Rising edge shifts the 4021 to present the next button on DATA.
        gpio_set_level(BOARD_NES_CTL_CLOCK, 1);
        esp_rom_delay_us(NES_HW_CTL_CLOCK_US);
        gpio_set_level(BOARD_NES_CTL_CLOCK, 0);
        esp_rom_delay_us(NES_HW_CTL_CLOCK_US);
    }

    return (NesControllerState){ .buttons = buttons };
}
