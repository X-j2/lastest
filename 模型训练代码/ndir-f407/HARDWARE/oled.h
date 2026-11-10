#ifndef __OLED_SSD1306_H
#define __OLED_SSD1306_H

#include "stm32f4xx.h"

#define OLED_WIDTH     128U
#define OLED_HEIGHT     64U

/* SSD1306 的 7 位 I2C 地址为 0x3C。 */
#define OLED_I2C_ADDRESS  0x3CU

void OLED_Init(void);
void OLED_Clear(void);
void OLED_Refresh(void);
void OLED_DrawPixel(uint8_t x, uint8_t y, uint8_t color);
void OLED_DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1);
void OLED_DrawChar(uint8_t x, uint8_t y, char character);
void OLED_DrawString(uint8_t x, uint8_t y, const char *text);
void OLED_DrawUInt(uint8_t x, uint8_t y, uint32_t value, uint8_t minimum_digits);

#endif
