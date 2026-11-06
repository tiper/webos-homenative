/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
typedef void *FT_Library; typedef unsigned FT_UInt;
typedef struct { unsigned width, rows; int pitch; unsigned char *buffer; } FT_Bitmap;
typedef struct { FT_Bitmap bitmap; } FT_Slot; typedef struct { FT_Slot *glyph; } *FT_Face;
enum { FT_LOAD_RENDER = 4 };
int FT_Init_FreeType(FT_Library *); int FT_New_Face(FT_Library, const char *, long, FT_Face *);
int FT_Set_Pixel_Sizes(FT_Face, FT_UInt, FT_UInt); int FT_Load_Char(FT_Face, unsigned long, int);
