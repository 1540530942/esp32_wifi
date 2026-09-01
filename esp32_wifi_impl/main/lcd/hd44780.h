#pragma once
#ifdef __cplusplus
extern "C" {
#endif

void lcd_init(void);
void lcd_clear(void);
void lcd_set_cursor(int col, int row);
void lcd_print(const char *str);
void lcd_print_line(int row, const char *str);  // 自动补空格到 16 字符

#ifdef __cplusplus
}
#endif
