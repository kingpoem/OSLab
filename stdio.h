/* 
 * Screen print functions.
 */

#pragma once

typedef signed char int8_t;
typedef unsigned char uint8_t;

void setcursor(int x, int y);
void puts(char* str, uint8_t color);