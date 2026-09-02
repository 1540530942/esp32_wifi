#include "hd44780.h"
#include "audio_board_config.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

#define LCD_COLS 16

static bool s_lcd_ready = false;

static void pulse_enable(void) {
    gpio_set_level(LCD_E, 1);
    esp_rom_delay_us(1);
    gpio_set_level(LCD_E, 0);
    esp_rom_delay_us(50);
}

static void write_nibble(uint8_t nibble) {
    gpio_set_level(LCD_D4, (nibble >> 0) & 1);
    gpio_set_level(LCD_D5, (nibble >> 1) & 1);
    gpio_set_level(LCD_D6, (nibble >> 2) & 1);
    gpio_set_level(LCD_D7, (nibble >> 3) & 1);
    pulse_enable();
}

static void write_byte(uint8_t rs, uint8_t data) {
    gpio_set_level(LCD_RS, rs);
    write_nibble(data >> 4);
    write_nibble(data & 0x0F);
    esp_rom_delay_us(50);
}

static void send_cmd(uint8_t cmd) { write_byte(0, cmd); }
static void send_data(uint8_t ch)  { write_byte(1, ch); }

void lcd_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LCD_RS) | (1ULL << LCD_E) |
                        (1ULL << LCD_D4) | (1ULL << LCD_D5) |
                        (1ULL << LCD_D6) | (1ULL << LCD_D7),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    gpio_set_level(LCD_RS, 0);
    gpio_set_level(LCD_E,  0);

    vTaskDelay(pdMS_TO_TICKS(50));   // >40ms power-on delay

    // HD44780 4-bit init sequence
    write_nibble(0x03); vTaskDelay(pdMS_TO_TICKS(5));   // >4.1ms
    write_nibble(0x03); esp_rom_delay_us(150);           // >100µs
    write_nibble(0x03); esp_rom_delay_us(150);
    write_nibble(0x02);                                  // switch to 4-bit

    send_cmd(0x28);  // function set: 4-bit, 2 lines, 5x8
    send_cmd(0x0C);  // display on, cursor off, blink off
    send_cmd(0x06);  // entry mode: increment, no shift
    send_cmd(0x01);  // clear display
    vTaskDelay(pdMS_TO_TICKS(2));    // clear needs >1.52ms
    s_lcd_ready = true;
}

void lcd_clear(void) {
    if (!s_lcd_ready) return;
    send_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(2));
}

void lcd_set_cursor(int col, int row) {
    const uint8_t row_offsets[] = {0x00, 0x40};
    if (row < 0 || row > 1) row = 0;
    if (col < 0 || col > 15) col = 0;
    send_cmd(0x80 | (row_offsets[row] + col));
}

void lcd_print(const char *str) {
    while (*str) send_data((uint8_t)*str++);
}

void lcd_print_line(int row, const char *str) {
    if (!s_lcd_ready) return;
    char buf[LCD_COLS + 1];
    snprintf(buf, sizeof(buf), "%-*s", LCD_COLS, str ? str : "");
    lcd_set_cursor(0, row);
    lcd_print(buf);
}
