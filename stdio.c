#include "stdio.h"

const unsigned SCREEN_WIDTH = 80;
const unsigned SCREEN_HEIGHT = 25;
const uint8_t DEFAULT_COLOR = 0x7;

uint8_t* g_ScreenBuffer = (uint8_t*) 0xb8000;
int g_ScreenX = 0;
int g_ScreenY = 0;

void putchr(int x, int y, char c)
{
    g_ScreenBuffer[2 * (y * SCREEN_WIDTH + x)] = c;
}

void putcolor(int x, int y, uint8_t color)
{
    g_ScreenBuffer[2 * (y * SCREEN_WIDTH + x) + 1] = color;
}

void setcursor(int x, int y)
{
    g_ScreenX = x;
    g_ScreenY = y;
}

void puts(char* str, uint8_t color)
{
    while(*str)
    {
        char c = *str;
        switch (c)
        {
        case '\n':
            g_ScreenX = 0;
            g_ScreenY++;
            break;
 
        case '\t':
            for (int i = 0; i < 4 - (g_ScreenX % 4); i++)
                puts((char*)' ', color);
            break;

        default:
            putchr(g_ScreenX, g_ScreenY, c);
            putcolor(g_ScreenX, g_ScreenY, color);
            g_ScreenX++;
            break;
        }
        str++;
    }
}

