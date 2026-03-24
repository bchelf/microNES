#pragma once

// ============================================================
// GPIO pin definitions for Waveshare ESP32-S3-Touch-AMOLED-1.91
// Verify against your specific board revision's schematic.
// ============================================================

// --- QSPI Display (SH8601 AMOLED) ---
// Pin assignments from official Waveshare schematic
#define BOARD_LCD_QSPI_CS     6   // QSPI chip select
#define BOARD_LCD_QSPI_SCLK  47   // QSPI clock
#define BOARD_LCD_QSPI_D0    18   // QSPI D0
#define BOARD_LCD_QSPI_D1     7   // QSPI D1
#define BOARD_LCD_QSPI_D2    48   // QSPI D2
#define BOARD_LCD_QSPI_D3     5   // QSPI D3
#define BOARD_LCD_RST        17   // AMOLED reset (active low)
#define BOARD_LCD_TE         16   // Tearing-effect pin (NC by default)

// --- I2C bus (shared by Touch + IMU) ---
#define BOARD_I2C_PORT        0   // I2C peripheral number
#define BOARD_I2C_SDA        40   // GP40 per board pinout
#define BOARD_I2C_SCL        39   // GP39 per board pinout
#define BOARD_I2C_FREQ_HZ   400000

// --- Touch controller (FT3168) ---
#define BOARD_TP_I2C_ADDR  0x38   // 7-bit I2C address
#define BOARD_TP_RST         17   // Shared with AMOLED reset (GPIO17)
#define BOARD_TP_INT         41   // TP interrupt pin

// --- IMU (QMI8658) – same I2C bus, not used by emulator ---
#define BOARD_IMU_I2C_ADDR 0x6B   // AD0 pulled high; use 0x6A if AD0 low

// --- Audio output ---
// The board has a passive buzzer on GPIO46 suitable for PWM audio.
// Connect a 1 kΩ resistor + 10 nF RC filter for better fidelity.
#define BOARD_AUDIO_PIN      46
#define BOARD_AUDIO_LEDC_TIMER     LEDC_TIMER_0
#define BOARD_AUDIO_LEDC_CHANNEL   LEDC_CHANNEL_0
#define BOARD_AUDIO_LEDC_SPEED     LEDC_LOW_SPEED_MODE

// --- Power / battery ADC ---
#define BOARD_BAT_ADC_CHANNEL  ADC_CHANNEL_0   // GPIO1

// ============================================================
// Display geometry
// ============================================================

// Native portrait resolution
#define BOARD_LCD_NATIVE_W   240
#define BOARD_LCD_NATIVE_H   536

// After 90° CW rotation (landscape) used for NES layout
#define DISPLAY_W            536   // landscape width  = native height
#define DISPLAY_H            240   // landscape height = native width

// NES framebuffer: 256×240, placed in the centre of the landscape display
//   left zone  [  0 .. 139] × [0 .. 239]  – D-pad
//   NES region [140 .. 395] × [0 .. 239]  – emulator output
//   right zone [396 .. 535] × [0 .. 239]  – A/B/Start/Select
#define NES_DISPLAY_X        140
#define NES_DISPLAY_Y          0
#define NES_DISPLAY_W        256   // == NES_FRAME_WIDTH
#define NES_DISPLAY_H        240   // == NES_FRAME_HEIGHT

#define UI_LEFT_X              0
#define UI_LEFT_W            140
#define UI_RIGHT_X           396
#define UI_RIGHT_W           140

// SPI bus configuration
#define BOARD_LCD_SPI_HOST   SPI2_HOST
#define BOARD_LCD_SPI_CLK_HZ (40 * 1000 * 1000)  // 40 MHz (conservative; try 80 MHz if stable)
