/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <SDL2/SDL.h>
typedef struct TTF_Font TTF_Font;
int TTF_GlyphIsProvided(TTF_Font *, Uint16);
TTF_Font *TTF_OpenFont(const char *, int); int TTF_Init(void); int TTF_SizeUTF8(TTF_Font *, const char *, int *, int *);
SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *, const char *, SDL_Color); const char *TTF_GetError(void);
