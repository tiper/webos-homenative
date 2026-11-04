/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A TV in software, for tests/host/run.sh. The real launcher talks to these fakes: a GPU that
 * catches drawing with a freed texture, luna services that answer with random, sometimes hostile
 * replies, and a remote that presses random keys. Virtual time runs 16 ms per loop. The arguments
 * are a seed and a number of frames. */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <GLES2/gl2.h>
#include <luna-service2/lunaservice.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"

static Uint64 clock_us = 1000000;
static long frames, max_frames = 20000;
static unsigned rng = 1;
static unsigned rnd(unsigned n) { rng = rng * 1103515245u + 12345u; return ((rng >> 16) & 0x7fff) % n; }
static int chance(unsigned pct) { return rnd(100) < pct; }
#define die(...) do { fprintf(stdout, "FAIL (frame %ld): ", frames); fprintf(stdout, __VA_ARGS__); fputc('\n', stdout); abort(); } while (0)

/* ---------- gpu: every texture tracked ---------- */
#define MAXTEX 200000
static unsigned char alive[MAXTEX]; static long tex_bytes[MAXTEX];
static GLuint next_tex = 1, bound; static long live_bytes, peak_bytes; static int live_tex, peak_tex;
void glGenTextures(GLsizei n, GLuint *o) { for (int i = 0; i < n; i++) { if (next_tex >= MAXTEX) die("too many textures"); o[i] = next_tex; alive[next_tex++] = 1; live_tex++; if (live_tex > peak_tex) peak_tex = live_tex; } }
void glDeleteTextures(GLsizei n, const GLuint *o) { for (int i = 0; i < n; i++) { if (!o[i]) continue; if (!alive[o[i]]) die("double free of texture %u", o[i]); alive[o[i]] = 0; live_tex--; live_bytes -= tex_bytes[o[i]]; tex_bytes[o[i]] = 0; if (bound == o[i]) bound = 0; } }
void glBindTexture(GLenum t, GLuint id) { (void)t; if (id && !alive[id]) die("binding freed texture %u", id); bound = id; }
void glTexImage2D(GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLint b, GLenum a, GLenum c, const void *px) {
    (void)t; (void)l; (void)f; (void)b; (void)a; (void)c;
    if (!bound) die("upload with no texture"); if (w <= 0 || h <= 0 || w > 8192 || h > 8192) die("texture %dx%d", w, h);
    volatile unsigned char sink = ((const unsigned char *)px)[(size_t)w * h * 4 - 1]; (void)sink; /* ASan: the buffer is really that big */
    live_bytes += (long)w * h * 4 - tex_bytes[bound]; tex_bytes[bound] = (long)w * h * 4; if (live_bytes > peak_bytes) peak_bytes = live_bytes;
}
void glDrawElements(GLenum m, GLsizei n, GLenum t, const void *p) { (void)m; (void)n; (void)t; (void)p; if (bound && !alive[bound]) die("drawing freed texture"); }
void glDrawArrays(GLenum m, GLint a, GLsizei n) { (void)m; (void)a; (void)n; if (bound && !alive[bound]) die("drawing freed texture"); }
GLuint glCreateShader(GLenum t) { (void)t; return 1; } void glShaderSource(GLuint a, GLsizei b, const char *const *c, const GLint *d) { (void)a; (void)b; (void)c; (void)d; } void glCompileShader(GLuint a) { (void)a; }
void glGetShaderiv(GLuint a, GLenum b, GLint *c) { (void)a; (void)b; *c = 1; } void glGetShaderInfoLog(GLuint a, GLsizei b, GLsizei *c, char *d) { (void)a; (void)b; (void)c; (void)d; }
GLuint glCreateProgram(void) { return 1; } void glAttachShader(GLuint a, GLuint b) { (void)a; (void)b; } void glBindAttribLocation(GLuint a, GLuint b, const char *c) { (void)a; (void)b; (void)c; } void glLinkProgram(GLuint a) { (void)a; }
void glGetProgramiv(GLuint a, GLenum b, GLint *c) { (void)a; (void)b; *c = 1; } void glGetProgramInfoLog(GLuint a, GLsizei b, GLsizei *c, char *d) { (void)a; (void)b; (void)c; (void)d; } void glUseProgram(GLuint a) { (void)a; }
GLint glGetUniformLocation(GLuint a, const char *b) { (void)a; (void)b; return 0; } void glUniform1i(GLint a, GLint b) { (void)a; (void)b; } void glUniform2f(GLint a, float b, float c) { (void)a; (void)b; (void)c; } void glUniform4f(GLint a, float b, float c, float d, float e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void glGenBuffers(GLsizei n, GLuint *o) { for (int i = 0; i < n; i++) o[i] = 1; } void glBindBuffer(GLenum a, GLuint b) { (void)a; (void)b; } void glBufferData(GLenum a, GLsizeiptr b, const void *c, GLenum d) { (void)a; (void)d; if (b > 0 && c) { volatile char s = ((const char *)c)[b - 1]; (void)s; } }
void glEnableVertexAttribArray(GLuint a) { (void)a; } void glDisableVertexAttribArray(GLuint a) { (void)a; } void glVertexAttribPointer(GLuint a, GLint b, GLenum c, GLboolean d, GLsizei e, const void *f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
void glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { (void)a; (void)b; (void)c; (void)d; } void glDisable(GLenum a) { (void)a; } void glEnable(GLenum a) { (void)a; } void glTexParameteri(GLenum a, GLenum b, GLint c) { (void)a; (void)b; (void)c; } void glPixelStorei(GLenum a, GLint b) { (void)a; (void)b; }
void glScissor(GLint a, GLint b, GLsizei c, GLsizei d) { (void)a; (void)b; (void)c; (void)d; } void glViewport(GLint a, GLint b, GLsizei c, GLsizei d) { (void)a; (void)b; (void)c; (void)d; } void glClearColor(float a, float b, float c, float d) { (void)a; (void)b; (void)c; (void)d; } void glClear(GLbitfield a) { (void)a; }
const GLubyte *glGetString(GLenum a) { (void)a; return (const GLubyte *)"fake"; } void glFinish(void) {} GLenum glGetError(void) { return GL_NO_ERROR; }

/* ---------- text and images ---------- */
TTF_Font *TTF_OpenFont(const char *p, int s) { (void)p; (void)s; return (TTF_Font *)1; } int TTF_Init(void) { return 0; } const char *TTF_GetError(void) { return ""; }
int TTF_SizeUTF8(TTF_Font *f, const char *s, int *w, int *h) { (void)f; *w = (int)strlen(s) * 12; *h = 28; return 0; }
SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *f, const char *s, SDL_Color c) { (void)f; (void)c; int w = (int)strlen(s) * 12; return SDL_CreateRGBSurfaceWithFormat(0, w > 0 ? w : 1, 28, 32, SDL_PIXELFORMAT_ARGB8888); }
int IMG_Init(int f) { return f; }
SDL_Surface *IMG_Load(const char *p) {
    if (strstr(p, "corrupt")) { SDL_SetError("corrupt"); return NULL; }
    int w = 130, h = 130;
    if (strstr(p, "wall")) { w = 3840; h = 2160; if (strstr(p, "tiny")) w = h = 4; if (strstr(p, "tall")) { w = 1000; h = 3000; } }
    else if (strstr(p, "snap")) { w = 1280; h = 720; }
    return SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
}
static FT_Slot slot; static unsigned char glyph_px[100]; static struct { FT_Slot *glyph; } face_ = {&slot};
int FT_Init_FreeType(FT_Library *l) { *l = (void *)1; return 0; } int FT_New_Face(FT_Library l, const char *p, long i, FT_Face *f) { (void)l; (void)p; (void)i; *f = (FT_Face)&face_; return 0; }
int FT_Set_Pixel_Sizes(FT_Face f, FT_UInt a, FT_UInt b) { (void)f; (void)a; (void)b; return 0; }
int FT_Load_Char(FT_Face f, unsigned long c, int fl) { (void)f; (void)c; (void)fl; slot.bitmap.width = slot.bitmap.rows = 10; slot.bitmap.pitch = 10; slot.bitmap.buffer = glyph_px; return 0; }

/* ---------- files: paths under /fake/ exist, and change now and then ---------- */
int fake_access(const char *p, int m) { (void)m; if (strstr(p, "/var/lib/homenative/wallpaper")) return 0; return strncmp(p, "/fake/", 6) && strstr(p, "wallpaper") == NULL ? -1 : 0; }
int fake_stat(const char *p, struct stat *s) { memset(s, 0, sizeof *s); if (strncmp(p, "/fake/", 6)) return -1; s->st_mtime = 1000 + (time_t)(frames / 500) * (time_t)(strstr(p, "snap") != NULL); return 0; }

/* ---------- SDL ---------- */
#include <pthread.h>
#include <stdarg.h>
static SDL_PixelFormat fmt_abgr = {SDL_PIXELFORMAT_ABGR8888}, fmt_argb = {SDL_PIXELFORMAT_ARGB8888};
SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 fl, int w, int h, int d, Uint32 f) {
    (void)fl; (void)d;
    SDL_Surface *s = calloc(1, sizeof *s);
    s->format = f == SDL_PIXELFORMAT_ABGR8888 ? &fmt_abgr : &fmt_argb; s->w = w; s->h = h; s->pitch = w * 4; s->pixels = calloc((size_t)h * s->pitch + 1, 1);
    return s;
}
SDL_Surface *SDL_ConvertSurfaceFormat(SDL_Surface *o, Uint32 f, Uint32 fl) {
    (void)fl; SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, o->w, o->h, 32, f);
    memcpy(s->pixels, o->pixels, (size_t)o->h * o->pitch);
    return s;
}
void SDL_FreeSurface(SDL_Surface *s) { if (s) { free(s->pixels); free(s); } }
int SDL_LockSurface(SDL_Surface *s) { (void)s; return 0; } void SDL_UnlockSurface(SDL_Surface *s) { (void)s; }
static __thread char errbuf[128];
int SDL_SetError(const char *f, ...) { va_list a; va_start(a, f); vsnprintf(errbuf, sizeof errbuf, f, a); va_end(a); return -1; }
const char *SDL_GetError(void) { return errbuf; }
int SDL_Init(Uint32 f) { (void)f; return 0; } void SDL_Quit(void) {} SDL_bool SDL_SetHint(const char *a, const char *b) { (void)a; (void)b; return SDL_TRUE; }
int SDL_GL_SetAttribute(int a, int b) { (void)a; (void)b; return 0; } int SDL_GL_SetSwapInterval(int a) { (void)a; return 0; }
SDL_Window *SDL_CreateWindow(const char *t, int x, int y, int w, int h, Uint32 f) { (void)t; (void)x; (void)y; (void)w; (void)h; (void)f; return (SDL_Window *)1; }
SDL_GLContext SDL_GL_CreateContext(SDL_Window *w) { (void)w; return (SDL_GLContext)1; } void SDL_GL_DeleteContext(SDL_GLContext c) { (void)c; } void SDL_DestroyWindow(SDL_Window *w) { (void)w; }
void SDL_GL_GetDrawableSize(SDL_Window *w, int *a, int *b) { (void)w; *a = 1920; *b = 1080; } void SDL_GetWindowSize(SDL_Window *w, int *a, int *b) { (void)w; *a = 1920; *b = 1080; }
void SDL_RaiseWindow(SDL_Window *w) { (void)w; } void SDL_GL_SwapWindow(SDL_Window *w) { (void)w; if (bound && !alive[bound]) die("swap with a freed texture"); }
int SDL_GetCurrentDisplayMode(int i, SDL_DisplayMode *m) { (void)i; m->w = 1920; m->h = 1080; m->refresh_rate = 60; return 0; }
Uint64 SDL_GetPerformanceCounter(void) { return clock_us; } Uint64 SDL_GetPerformanceFrequency(void) { return 1000000; }
SDL_mutex *SDL_CreateMutex(void) { pthread_mutex_t *m = malloc(sizeof *m); pthread_mutex_init(m, NULL); return (SDL_mutex *)m; }
int SDL_LockMutex(SDL_mutex *m) { return pthread_mutex_lock((pthread_mutex_t *)m); } int SDL_UnlockMutex(SDL_mutex *m) { return pthread_mutex_unlock((pthread_mutex_t *)m); }
SDL_cond *SDL_CreateCond(void) { pthread_cond_t *c = malloc(sizeof *c); pthread_cond_init(c, NULL); return (SDL_cond *)c; }
int SDL_CondWait(SDL_cond *c, SDL_mutex *m) { return pthread_cond_wait((pthread_cond_t *)c, (pthread_mutex_t *)m); } int SDL_CondSignal(SDL_cond *c) { return pthread_cond_signal((pthread_cond_t *)c); }
typedef struct { int (*fn)(void *); void *arg; } Start;
static void *thread_start(void *p) { Start s = *(Start *)p; free(p); s.fn(s.arg); return NULL; }
SDL_Thread *SDL_CreateThread(int (*fn)(void *), const char *n, void *arg) { (void)n; Start *s = malloc(sizeof *s); s->fn = fn; s->arg = arg; pthread_t t; pthread_create(&t, NULL, thread_start, s); pthread_detach(t); return (SDL_Thread *)1; }

/* ---------- remote ---------- */
static int shown = 1;
static void luna_tick(void);
static int make_event(SDL_Event *e) {
    memset(e, 0, sizeof *e);
    static int ok_held, blue_held;
    unsigned r = rnd(100);
    if (r < 55) { static const int k[] = {SDLK_LEFT, SDLK_RIGHT, SDLK_LEFT, SDLK_RIGHT, SDLK_UP, SDLK_DOWN}; e->type = SDL_KEYDOWN; e->key.keysym.sym = k[rnd(6)]; }
    else if (r < 70) { e->type = ok_held ? SDL_KEYUP : SDL_KEYDOWN; ok_held = !ok_held; e->key.keysym.sym = SDLK_RETURN; }
    else if (r < 78) { e->type = SDL_KEYDOWN; e->key.keysym.scancode = 482; }
    else if (r < 80) { e->type = blue_held ? SDL_KEYUP : SDL_KEYDOWN; blue_held = !blue_held; e->key.keysym.scancode = 489; }
    else if (r < 90) { e->type = SDL_MOUSEMOTION; e->motion.x = (int)rnd(1920); e->motion.y = (int)rnd(1080); e->motion.xrel = (int)rnd(200); e->motion.yrel = (int)rnd(200); }
    else if (r < 94) { e->type = rnd(2) ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP; e->button.button = SDL_BUTTON_LEFT; e->button.x = (int)rnd(1920); e->button.y = (int)rnd(1080); }
    else if (r < 96) { e->type = SDL_MOUSEWHEEL; e->wheel.y = rnd(2) ? 1 : -1; }
    else {
        static const int ev[] = {SDL_WINDOWEVENT_HIDDEN, SDL_WINDOWEVENT_SHOWN, SDL_WINDOWEVENT_FOCUS_GAINED, SDL_WINDOWEVENT_FOCUS_LOST, SDL_WINDOWEVENT_EXPOSED};
        e->type = SDL_WINDOWEVENT; e->window.event = (Uint8)ev[rnd(5)];
    }
    return 1;
}
static int pump(SDL_Event *e, int wait_ms) {
    static int left; /* events still to come this frame */
    if (!left) {
        frames++;
        clock_us += (Uint64)(wait_ms > 16 ? wait_ms : 16) * 1000;
        if (frames > max_frames) { static int told; if (told) return 0; told = 1; e->type = SDL_QUIT; return 1; }
        luna_tick();
        left = chance(35) ? 1 + (int)rnd(3) : 0;
        /* a long quiet stretch now and then, for what only happens with no input: dimming, marquees */
        static int quiet; if (quiet) { quiet--; left = 0; } else if (chance(1)) quiet = 200;
        if (!left) return 0;
    }
    left--;
    return make_event(e);
}
int SDL_PollEvent(SDL_Event *e) { return pump(e, 16); }
int SDL_WaitEventTimeout(SDL_Event *e, int t) { return pump(e, t); }

/* ---------- luna ---------- */
struct LSMessage { char *payload; }; struct LSHandle { int unused; };
const char *LSMessageGetPayload(LSMessage *m) { return m->payload; }
bool LSRegister(const char *n, LSHandle **h, LSError *e) { (void)n; (void)e; static LSHandle one; *h = &one; return true; }
bool LSGmainContextAttach(LSHandle *h, GMainContext *c, LSError *e) { (void)h; (void)c; (void)e; return true; }

typedef struct { char uri[256]; LSFilterFunc cb; void *ctx; LSMessageToken token; int alive; } Sub;
#define MAXSUB 64
static Sub subs[MAXSUB]; static LSMessageToken next_token = 1;
typedef struct Reply { LSFilterFunc cb; void *ctx; char *json; LSMessageToken token; } Reply;

/* a string of the given flavour: ordinary, very long, multi-byte, or with characters a careless
 * snprintf or shell line would trip on */
static const char *word(void) {
    static char b[8][1000]; static int i; char *s = b[i = (i + 1) % 8];
    switch (rnd(9)) {
    case 0: { int n = 150 + (int)rnd(450); memset(s, 'x', (size_t)n); s[n] = 0; break; }
    case 1: { int o = 0, n = 60 + (int)rnd(200); for (int k = 0; k < n; k++) o += sprintf(s + o, "\xe2\x82\xac"); break; }
    case 2: strcpy(s, "a\"b'c$d`e\\f;g h"); break;
    case 3: strcpy(s, ""); break;
    case 4: strcpy(s, "\xf0\x9f\x8e\xac movie"); break;
    default: sprintf(s, "Name %u", rnd(30)); break;
    }
    return s;
}
static void add_common(cJSON *r, int sub) { cJSON_AddBoolToObject(r, "returnValue", 1); if (sub) cJSON_AddBoolToObject(r, "subscribed", 1); }
static char *finish(cJSON *r) { char *s = cJSON_PrintUnformatted(r); cJSON_Delete(r); return s; }
static const char *img(const char *kind) { static char b[8][120]; static int i; char *s = b[i = (i + 1) % 8]; sprintf(s, "/fake/%s%s%u.png", chance(5) ? "corrupt-" : "", kind, rnd(40)); return s; }
static char *launchpoints(int sub) {
    cJSON *r = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(r, "launchPoints");
    int n = chance(3) ? 0 : 5 + (int)rnd(40);
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject(); char id[40]; sprintf(id, "app.%u", rnd(60));
        cJSON_AddStringToObject(o, "id", chance(3) ? "io.github.tiper.homenative" : chance(3) ? "com.webos.app.homeconnect" : id);
        cJSON_AddStringToObject(o, "launchPointId", chance(3) ? word() : id); cJSON_AddStringToObject(o, "title", word());
        cJSON_AddStringToObject(o, "largeIcon", chance(5) ? "" : img("icon")); cJSON_AddStringToObject(o, "iconColor", chance(10) ? "bad" : "#336699");
        cJSON_AddBoolToObject(o, "removable", (int)rnd(2)); cJSON_AddBoolToObject(o, "systemApp", (int)rnd(2));
        if (chance(10)) { cJSON *p = cJSON_AddObjectToObject(o, "params"); cJSON_AddStringToObject(p, "x", word()); }
        cJSON_AddItemToArray(a, o);
    }
    add_common(r, sub); return finish(r);
}
static char *inputs_reply(int sub) {
    cJSON *r = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(r, "devices");
    for (unsigned i = 0, n = rnd(22); i < n; i++) {
        cJSON *o = cJSON_CreateObject(); char id[40]; sprintf(id, "HDMI_%u", rnd(4));
        cJSON_AddStringToObject(o, "id", id); cJSON_AddStringToObject(o, "appId", id); cJSON_AddStringToObject(o, "label", word());
        cJSON_AddBoolToObject(o, "connected", (int)rnd(4) != 0); cJSON_AddBoolToObject(o, "hdmiPlugIn", (int)rnd(4) != 0);
        cJSON_AddStringToObject(o, "iconPrefix", "/fake/"); cJSON_AddStringToObject(o, "icon", "input.png");
        if (chance(50)) { cJSON *s = cJSON_AddArrayToObject(o, "subList"), *e = cJSON_CreateObject(); cJSON_AddStringToObject(e, "osdName", word()); cJSON_AddItemToArray(s, e); }
        cJSON_AddItemToArray(a, o);
    }
    add_common(r, sub); return finish(r);
}
static char *recents_reply(int sub) {
    cJSON *r = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(r, "recentsAppList");
    for (unsigned i = 0, n = rnd(9); i < n; i++) {
        cJSON *o = cJSON_CreateObject(); char id[40]; sprintf(id, "app.%u", rnd(20));
        cJSON_AddStringToObject(o, "appId", id); cJSON_AddStringToObject(o, "title", word());
        if (chance(70)) cJSON_AddStringToObject(o, "cardSnapShotFilePath", img("snap"));
        cJSON_AddItemToArray(a, o);
    }
    add_common(r, sub); return finish(r);
}
static char *apps_reply(int sub) {
    cJSON *r = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(r, "apps");
    for (int i = 0; i < 20; i++) { cJSON *o = cJSON_CreateObject(); char id[40]; sprintf(id, "app.%d", i); cJSON_AddStringToObject(o, "id", id); cJSON_AddStringToObject(o, "icon", img("icon")); cJSON_AddStringToObject(o, "folderPath", "/fake"); cJSON_AddItemToArray(a, o); }
    add_common(r, sub); return finish(r);
}
static char *rank_reply(void) {
    cJSON *r = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(r, "channelList");
    for (unsigned i = 0, n = rnd(14); i < n; i++) { cJSON *o = cJSON_CreateObject(); char id[40]; sprintf(id, "ch_%u", rnd(30)); cJSON_AddStringToObject(o, "channelId", id); cJSON_AddItemToArray(a, o); }
    cJSON_AddBoolToObject(r, "returnValue", chance(80)); return finish(r);
}
static char *chlist_reply(void) {
    cJSON *r = cJSON_CreateObject(), *n = cJSON_AddObjectToObject(r, "names"), *v = cJSON_AddArrayToObject(r, "values");
    cJSON_AddNumberToObject(n, "channelId", 0); cJSON_AddNumberToObject(n, "channelName", 1); cJSON_AddNumberToObject(n, "channelNumber", 2);
    for (int i = 0; i < 25; i++) { cJSON *o = cJSON_CreateArray(); char id[40]; sprintf(id, "ch_%d", i); cJSON_AddItemToArray(o, cJSON_CreateString(id)); cJSON_AddItemToArray(o, cJSON_CreateString(word())); cJSON_AddItemToArray(o, cJSON_CreateString("12")); cJSON_AddItemToArray(v, o); }
    add_common(r, 0); return finish(r);
}
static char *events_reply(void) {
    cJSON *r = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(r, "programList");
    for (int i = 0; i < 12; i++) { cJSON *o = cJSON_CreateObject(); char id[40]; sprintf(id, "ch_%u", rnd(30)); cJSON_AddStringToObject(o, "channelId", id); cJSON_AddStringToObject(o, "programName", word()); cJSON_AddStringToObject(o, "localStartTime", chance(10) ? "junk" : "2026,10,20,12,00,00"); cJSON_AddStringToObject(o, "localEndTime", "2026,10,20,13,00,00"); cJSON_AddItemToArray(a, o); }
    add_common(r, 0); return finish(r);
}
static char *reply_for(const char *uri, int sub) {
    if (strstr(uri, "listLaunchPoints")) return launchpoints(sub);
    if (strstr(uri, "getAllInputStatus")) return inputs_reply(sub);
    if (strstr(uri, "listRecents")) return recents_reply(sub);
    if (strstr(uri, "listApps")) return apps_reply(sub);
    if (strstr(uri, "getRank")) return rank_reply();
    if (strstr(uri, "getChannelList")) return chlist_reply();
    if (strstr(uri, "getCurrentEventList")) return events_reply();
    if (strstr(uri, "getLastChannelId")) { char b[80]; sprintf(b, "{\"returnValue\":true,\"channelId\":\"ch_%u\"}", rnd(30)); return strdup(b); }
    if (strstr(uri, "getSystemTime")) return strdup("{\"returnValue\":true,\"utc\":1790000000,\"localtime\":{\"year\":2026,\"month\":9,\"day\":21,\"hour\":15,\"minute\":13,\"second\":20}}");
    if (strstr(uri, "getSystemSettings")) { static const char *const l[] = {"pt-PT", "en-US", "fr-FR", "de-DE", "es-ES", "it-IT", "ko-KR", "xx", ""}; char b[160]; sprintf(b, "{\"returnValue\":true,\"settings\":{\"supportAirplay\":true,\"localeInfo\":{\"locales\":{\"UI\":\"%s\"}}}}", l[rnd(9)]); return strdup(b); }
    if (strstr(uri, "getChannelMode")) return strdup("{\"returnValue\":true,\"channelMode\":{\"channelModeName\":\"Cable\"}}");
    if (strstr(uri, "registerApp")) return strdup("{\"returnValue\":true,\"event\":\"registered\"}");
    return strdup("{\"returnValue\":true}");
}
static char *hostile(char *good) {
    if (!chance(6)) return good;
    free(good);
    static const char *const bad[] = {"", "{", "null", "[]", "{\"returnValue\":false,\"errorText\":\"Service disconnected\"}", "{\"returnValue\":false,\"errorText\":\"Denied\"}", "{\"returnValue\":true,\"launchPoints\":{}}", "{\"returnValue\":true,\"devices\":3,\"recentsAppList\":\"x\"}"};
    return strdup(bad[rnd(8)]);
}
static gboolean deliver(gpointer p) {
    Reply *r = p;
    LSMessage m = {r->json};
    int live = !r->token;
    for (int i = 0; i < MAXSUB; i++) if (subs[i].alive && subs[i].token == r->token) live = 1;
    if (live) r->cb(NULL, &m, r->ctx);
    free(r->json); free(r);
    return G_SOURCE_REMOVE;
}
static void send_later(LSFilterFunc cb, void *ctx, char *json, LSMessageToken token) {
    Reply *r = malloc(sizeof *r); r->cb = cb; r->ctx = ctx; r->json = json; r->token = token;
    g_idle_add(deliver, r);
}
bool LSCall(LSHandle *h, const char *uri, const char *pl, LSFilterFunc cb, void *ctx, LSMessageToken *tok, LSError *e) {
    (void)h; (void)pl; (void)e;
    if (chance(2)) { e->message = "busy"; return false; }
    int i = 0; while (i < MAXSUB && subs[i].alive) i++;
    if (i == MAXSUB) die("more than %d live subscriptions: a leak", MAXSUB);
    subs[i] = (Sub){.cb = cb, .ctx = ctx, .token = next_token++, .alive = 1}; snprintf(subs[i].uri, sizeof subs[i].uri, "%s", uri);
    *tok = subs[i].token;
    send_later(cb, ctx, hostile(reply_for(uri, 1)), subs[i].token);
    return true;
}
bool LSCallOneReply(LSHandle *h, const char *uri, const char *pl, LSFilterFunc cb, void *ctx, LSMessageToken *tok, LSError *e) {
    (void)h; (void)tok; (void)pl;
    if (chance(1)) { e->message = "busy"; return false; }
    send_later(cb, ctx, hostile(reply_for(uri, 0)), 0);
    return true;
}
bool LSCallCancel(LSHandle *h, LSMessageToken t, LSError *e) { (void)h; (void)e; for (int i = 0; i < MAXSUB; i++) if (subs[i].alive && subs[i].token == t) subs[i].alive = 0; return true; }

/* the system pushing changes on what the launcher subscribed to */
static void luna_tick(void) {
    if (!chance(4)) return;
    int i = (int)rnd(MAXSUB);
    if (!subs[i].alive) return;
    const char *u = subs[i].uri; char *j;
    if (strstr(u, "registerApp")) j = strdup(chance(60) ? "{\"returnValue\":true,\"event\":\"relaunch\"}" : "{\"returnValue\":true,\"event\":\"pause\"}");
    else if (strstr(u, "listRecents") && chance(50)) j = strdup("{\"returnValue\":true,\"eventType\":\"added\",\"appId\":\"app.1\"}");
    else if (strstr(u, "listLaunchPoints") && chance(40)) j = strdup("{\"returnValue\":true,\"change\":\"added\"}");
    else j = hostile(reply_for(u, 1));
    if (strstr(j, "\"returnValue\":false") || !strchr(j, '{')) subs[i].alive = 1; /* on_reply cancels what it retries */
    send_later(subs[i].cb, subs[i].ctx, j, subs[i].token);
}

extern int homenative_main(int, char **);
int main(int argc, char **argv) {
    if (argc > 1) rng = (unsigned)atoi(argv[1]) * 2654435761u + 1;
    if (argc > 2) max_frames = atol(argv[2]);
    char *fake[] = {argv[0], (char *)"{\"appId\":\"com.webos.app.home\"}", NULL};
    int rc = homenative_main(2, fake);
    printf("ok: %ld frames, %d textures (peak %d), %.1f MB (peak %.1f MB)\n", frames, live_tex, peak_tex, live_bytes / 1e6, peak_bytes / 1e6);
    return rc;
}
