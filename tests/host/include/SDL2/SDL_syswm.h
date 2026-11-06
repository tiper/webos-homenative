/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <SDL2/SDL.h>
typedef struct { Uint8 major, minor, patch; } SDL_version;
enum { SDL_SYSWM_UNKNOWN, SDL_SYSWM_WAYLAND };
struct wl_display;
typedef struct { SDL_version version; int subsystem; union { struct { struct wl_display *display; } wl; } info; } SDL_SysWMinfo;
void SDL_GetVersion(SDL_version *); SDL_bool SDL_GetWindowWMInfo(SDL_Window *, SDL_SysWMinfo *);
