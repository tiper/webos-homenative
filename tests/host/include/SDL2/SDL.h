/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The SDL2 the launcher uses, faked: see fake.c. */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint8_t Uint8; typedef uint16_t Uint16; typedef uint32_t Uint32; typedef uint64_t Uint64;
typedef enum { SDL_FALSE, SDL_TRUE } SDL_bool;
typedef struct { Uint8 r, g, b, a; } SDL_Color;
typedef struct { Uint32 format; } SDL_PixelFormat;
typedef struct { SDL_PixelFormat *format; int w, h, pitch; void *pixels; } SDL_Surface;
typedef struct SDL_Window SDL_Window; typedef void *SDL_GLContext; typedef struct SDL_mutex SDL_mutex; typedef struct SDL_cond SDL_cond; typedef struct SDL_Thread SDL_Thread;
typedef struct { int w, h, refresh_rate; } SDL_DisplayMode;
enum { SDL_PIXELFORMAT_ABGR8888 = 1, SDL_PIXELFORMAT_ARGB8888 = 2 };
enum { SDL_INIT_VIDEO = 1, SDL_WINDOW_OPENGL = 1, SDL_WINDOW_SHOWN = 2, SDL_BUTTON_LEFT = 1 };
#define SDL_HINT_VIDEO_ALLOW_SCREENSAVER "x"
enum { SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_MAJOR_VERSION, SDL_GL_CONTEXT_MINOR_VERSION, SDL_GL_RED_SIZE, SDL_GL_GREEN_SIZE, SDL_GL_BLUE_SIZE, SDL_GL_ALPHA_SIZE, SDL_GL_DEPTH_SIZE, SDL_GL_STENCIL_SIZE, SDL_GL_CONTEXT_PROFILE_ES };
enum { SDL_QUIT = 1, SDL_KEYDOWN, SDL_KEYUP, SDL_MOUSEMOTION, SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP, SDL_MOUSEWHEEL, SDL_WINDOWEVENT };
enum { SDL_WINDOWEVENT_HIDDEN = 1, SDL_WINDOWEVENT_MINIMIZED, SDL_WINDOWEVENT_SHOWN, SDL_WINDOWEVENT_RESTORED, SDL_WINDOWEVENT_EXPOSED, SDL_WINDOWEVENT_FOCUS_GAINED, SDL_WINDOWEVENT_FOCUS_LOST };
typedef int SDL_Keycode; typedef int SDL_Scancode;
enum { SDLK_RETURN = 13, SDLK_ESCAPE = 27, SDLK_LEFT = 1000, SDLK_RIGHT, SDLK_UP, SDLK_DOWN, SDLK_KP_ENTER, SDL_SCANCODE_AC_BACK = 270 };
typedef union {
    Uint32 type;
    struct { Uint32 type; int repeat; struct { SDL_Keycode sym; SDL_Scancode scancode; } keysym; } key;
    struct { Uint32 type; int x, y, xrel, yrel; } motion;
    struct { Uint32 type; int x, y, button; } button;
    struct { Uint32 type; int y; } wheel;
    struct { Uint32 type; Uint8 event; } window;
} SDL_Event;
SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32, int, int, int, Uint32); SDL_Surface *SDL_ConvertSurfaceFormat(SDL_Surface *, Uint32, Uint32); void SDL_FreeSurface(SDL_Surface *);
int SDL_LockSurface(SDL_Surface *); void SDL_UnlockSurface(SDL_Surface *);
int SDL_SetError(const char *, ...); const char *SDL_GetError(void); Uint64 SDL_GetPerformanceCounter(void); Uint64 SDL_GetPerformanceFrequency(void);
int SDL_Init(Uint32); void SDL_Quit(void); SDL_bool SDL_SetHint(const char *, const char *); int SDL_GL_SetAttribute(int, int); int SDL_GL_SetSwapInterval(int);
SDL_Window *SDL_CreateWindow(const char *, int, int, int, int, Uint32); SDL_GLContext SDL_GL_CreateContext(SDL_Window *); void SDL_GL_DeleteContext(SDL_GLContext); void SDL_DestroyWindow(SDL_Window *);
void SDL_GL_GetDrawableSize(SDL_Window *, int *, int *); void SDL_GetWindowSize(SDL_Window *, int *, int *); void SDL_RaiseWindow(SDL_Window *); void SDL_GL_SwapWindow(SDL_Window *);
int SDL_GetCurrentDisplayMode(int, SDL_DisplayMode *); int SDL_PollEvent(SDL_Event *); int SDL_WaitEventTimeout(SDL_Event *, int);
SDL_mutex *SDL_CreateMutex(void); int SDL_LockMutex(SDL_mutex *); int SDL_UnlockMutex(SDL_mutex *); SDL_cond *SDL_CreateCond(void); int SDL_CondWait(SDL_cond *, SDL_mutex *); int SDL_CondSignal(SDL_cond *);
SDL_Thread *SDL_CreateThread(int (*)(void *), const char *, void *);
void SDL_Delay(Uint32);
