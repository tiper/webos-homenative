/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <SDL2/SDL.h>
enum { IMG_INIT_JPG = 1, IMG_INIT_PNG = 2 };
SDL_Surface *IMG_Load(const char *); int IMG_Init(int);
