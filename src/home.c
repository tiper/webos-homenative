/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 tiper */
/* homenative — a native home-screen launcher for webOS 6 (LG C1).
 *
 * One window, drawn straight on the GPU: SDL2 (the TV's own LG port) for the Wayland window and
 * input, OpenGL ES 2 for every pixel, SDL2_image/SDL2_ttf only to decode PNGs and rasterise text
 * into textures, libluna-service2 for the system calls. No QML, no JavaScript, no web engine.
 *
 * The look follows LG's own webOS 6 home (UserInterfaceLayer/Containers/Main/AppTile.qml): square
 * tiles along the bottom, the app's icon filling the tile on its iconColor, the title shown inside
 * the tile only while it is focused. On top of that: input pills and a clock.
 *
 * Two shaders: an opaque one for the wallpaper (one texture fetch per pixel, no blending), and a
 * rounded-rectangle one (signed distance field) for everything else: solid, textured, shadow or
 * vertical gradient. */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
/* (the NDK's SDL was built without Wayland; LG's on the TV has it) */
#define SDL_VIDEO_DRIVER_WAYLAND
#include <SDL2/SDL_syswm.h>
#include <GLES2/gl2.h>
#include <glib.h>
#include <glib-unix.h>
#include <luna-service2/lunaservice.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <execinfo.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "cJSON.h"

#define APP_ID "io.github.tiper.homenative"
#define W 1920.0f
#define H 1080.0f

/* ---------- log ---------- */

static Uint64 t0;
static double cpu_ms(void) { /* this thread's CPU time: work, not waiting */
    struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static double now_ms(void) { return (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency(); }
/* The log lives in /tmp, which is RAM, and Home runs for weeks (standby keeps it alive): it
 * starts over past LOG_MAX, and as Home the per-call chatter (vlogf_) stays out unless
 * /var/lib/homenative/verbose exists. */
#define LOG_PATH "/tmp/homenative.log"
/* the dev app runs as an app user, who cannot write Home's (root's) log: it keeps its own */
#define DEV_LOG_PATH "/tmp/homenative-dev.log"
#define LOG_MAX (512 * 1024)
#define LOG_PREV LOG_PATH ".prev" /* the run before this one: what a crash leaves behind */
static const char *log_path = LOG_PATH;
static int verbose = 1;
static void logf_(const char *fmt, ...) {
    if (ftell(stderr) > LOG_MAX && freopen(log_path, "w", stderr)) fprintf(stderr, "(log restarted)\n");
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[%8.1f] ", t0 ? now_ms() : 0.0);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr); fflush(stderr);
    va_end(ap);
}
#define vlogf_(...) do { if (verbose) logf_(__VA_ARGS__); } while (0)
/* A crash, or the watchdog's alarm: what was happening goes to the log (addresses only: the binary
 * is stripped, so use addr2line on an unstripped build), then the signal does what it would have */
/* Where it stopped, from the registers the kernel saved, then the walk up from there. The walk
 * can't cross into LG's libraries (they carry no unwind tables), and a hang is usually in one,
 * so the registers are what names it. dladdr and backtrace take the loader's lock, which a fault
 * inside the loader would already hold, so a fresh alarm ends the process if this hangs too. */
static void fatal_where(const char *name, uintptr_t a) {
    char m[128]; Dl_info d;
    int n = snprintf(m, sizeof m, "%s %08lx", name, (unsigned long)a);
    if (dladdr((void *)a, &d) && d.dli_fname)
        n += snprintf(m + n, sizeof m - (size_t)n, " %s+%#lx", d.dli_fname, (unsigned long)(a - (uintptr_t)d.dli_fbase));
    if (n > (int)sizeof m - 2) n = (int)sizeof m - 2;
    m[n++] = '\n';
    if (write(STDERR_FILENO, m, (size_t)n) < 0) {}
}
static pthread_t main_thread;
static void on_fatal(int sig, siginfo_t *si, void *uc) {
    /* the alarm can land on any thread, such as one of Mali's; the hang is the main one's */
    if (sig == SIGALRM && !pthread_equal(pthread_self(), main_thread)) { pthread_kill(main_thread, sig); return; }
    sigset_t alrm;
    signal(SIGALRM, SIG_DFL);
    sigemptyset(&alrm); sigaddset(&alrm, SIGALRM);
    sigprocmask(SIG_UNBLOCK, &alrm, NULL);
    alarm(3);
    char m[64];
    int n = snprintf(m, sizeof m, "fatal signal %d, address %p; stack:\n", sig, si->si_addr);
    if (write(STDERR_FILENO, m, (size_t)n) < 0) {}
#ifdef __arm__
    const mcontext_t *c = &((ucontext_t *)uc)->uc_mcontext;
    fatal_where("pc", c->arm_pc); fatal_where("lr", c->arm_lr);
#else
    (void)uc;
#endif
    void *bt[32];
    backtrace_symbols_fd(bt, backtrace(bt, 32), STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig);
}
static void fatal_handlers(void) {
    main_thread = pthread_self();
    void *warm[1]; backtrace(warm, 1); /* its first call loads libgcc: not something to do in a crash */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fatal;
    sa.sa_flags = SA_SIGINFO; /* blocked while it runs: a fault in here just ends the process */
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGALRM};
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
}
static void scpy(char *d, size_t n, const char *s) { snprintf(d, n, "%s", s ? s : ""); }

/* ---------- GL ---------- */

typedef struct { GLuint id; int w, h; } Tex;

static GLuint prog, prog_bg, vbo, batch_vbo, batch_ibo;
static GLint ub_rect;

/* Batched: every rounded rect is four vertices carrying everything the old per-draw uniforms
 * did, so consecutive rects become one draw call (split only when the texture changes). Ten
 * glUniform calls per rect were the frame's main cost on this TV's CPU. */
static const char *VS =
    "attribute vec2 a_pos; attribute vec4 a_local; attribute vec2 a_rs; attribute vec4 a_color;\n"
    "attribute vec2 a_grad; attribute vec4 a_uvr; attribute float a_mode;\n"
    "uniform vec2 u_screen;\n"
    "varying vec2 v_local; varying vec2 v_size; varying vec2 v_rs; varying vec4 v_color;\n"
    "varying vec2 v_grad; varying vec4 v_uvr; varying float v_mode;\n"
    "void main(){\n"
    "  v_local = a_local.xy; v_size = a_local.zw; v_rs = a_rs; v_color = a_color;\n"
    "  v_grad = a_grad; v_uvr = a_uvr; v_mode = a_mode;\n"
    "  gl_Position = vec4(a_pos.x/u_screen.x*2.0-1.0, 1.0-a_pos.y/u_screen.y*2.0, 0.0, 1.0);\n"
    "}\n";

/* local coordinates stay within a few hundred px of the rect centre, so mediump is enough */
static const char *FS =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_local; varying vec2 v_size; varying vec2 v_rs; varying vec4 v_color;\n"
    "varying vec2 v_grad; varying vec4 v_uvr; varying float v_mode;\n"
    "void main(){\n"
    "  vec2 uv = v_local / v_size + 0.5;\n"
    "  vec4 c = v_color;\n"
    "  if (v_mode > 1.5) c.a *= texture2D(u_tex, v_uvr.xy + clamp(uv, 0.0, 1.0) * v_uvr.zw).a;\n"
    "  else if (v_mode > 0.5) c *= texture2D(u_tex, v_uvr.xy + clamp(uv, 0.0, 1.0) * v_uvr.zw);\n"
    "  vec2 q = abs(v_local) - 0.5*v_size + vec2(v_rs.x);\n"
    "  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - v_rs.x;\n"
    "  c.a *= (1.0 - smoothstep(-v_rs.y, v_rs.y, d)) * mix(v_grad.x, v_grad.y, clamp(uv.y, 0.0, 1.0));\n"
    "  gl_FragColor = c;\n"
    "}\n";

static const char *VS_BG =
    "attribute vec2 a_pos; uniform vec4 u_rect; uniform vec2 u_screen;\n"
    "varying highp vec2 v_uv;\n"
    "void main(){ vec2 p = u_rect.xy + a_pos*u_rect.zw; v_uv = a_pos;\n"
    "  gl_Position = vec4(p.x/u_screen.x*2.0-1.0, 1.0-p.y/u_screen.y*2.0, 0.0, 1.0); }\n";
static const char *FS_BG =
    "precision mediump float; uniform sampler2D u_tex; varying highp vec2 v_uv;\n"
    "void main(){ gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0); }\n";

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[1024]; glGetShaderInfoLog(s, sizeof b, NULL, b); logf_("shader: %s", b); }
    return s;
}
static GLuint program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    static const char *const attrs[] = {"a_pos", "a_local", "a_rs", "a_color", "a_grad", "a_uvr", "a_mode"};
    for (int i = 0; i < 7; i++) glBindAttribLocation(p, (GLuint)i, attrs[i]);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char b[1024]; glGetProgramInfoLog(p, sizeof b, NULL, b); logf_("link: %s", b); }
    return p;
}

static void gl_init(void) {
    prog_bg = program(VS_BG, FS_BG);
    glUseProgram(prog_bg);
    ub_rect = glGetUniformLocation(prog_bg, "u_rect");
    GLint ub_screen = glGetUniformLocation(prog_bg, "u_screen");
    glUniform1i(glGetUniformLocation(prog_bg, "u_tex"), 0);
    glUniform2f(ub_screen, W, H);

    prog = program(VS, FS);
    glUseProgram(prog);
    GLint u_screen = glGetUniformLocation(prog, "u_screen");
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glUniform2f(u_screen, W, H);
    /* indices for up to 4096 quads per draw call, built once */
    static GLushort idx[4096 * 6];
    for (int q = 0; q < 4096; q++) {
        GLushort v = (GLushort)(q * 4);
        GLushort *o = &idx[q * 6];
        o[0] = v; o[1] = v + 1; o[2] = v + 2; o[3] = v + 2; o[4] = v + 1; o[5] = v + 3;
    }
    glGenBuffers(1, &batch_ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, batch_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
    glGenBuffers(1, &batch_vbo);

    static const float quad[] = {0,0, 1,0, 0,1, 1,1};
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    /* keep destination alpha at 1: the window is transparent to the compositor, and a straight
     * SRC_ALPHA blend on the alpha channel would let the video plane bleed through soft edges */
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
}

typedef struct { float r, g, b, a; } Col;
static Col rgba(float r, float g, float b, float a) { Col c = {r, g, b, a}; return c; }
static Col hexcol(const char *s, Col fallback) {
    if (!s || s[0] != '#' || strlen(s) < 7) return fallback;
    unsigned v = (unsigned)strtoul(s + 1, NULL, 16);
    return rgba(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, 1);
}
static float luma(Col c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; }

static float g_alpha = 1.f;  /* multiplies every draw: fades an item in or out */
static float tex_mode = 1.f; /* 1: multiply by the texture, 2: use only its alpha */
static float g_uvr[4] = {0, 0, 1, 1}; /* the part of the texture a draw shows (cover crops) */

/* draw a rounded rect; tex may be NULL; soft > 1 grows the quad for soft edges (shadows) */
/* the batch: pos 2, local+size 4, radius+soft 2, colour 4, gradient 2, uv rect 4, mode 1 */
typedef struct { float x, y, lx, ly, sw, sh, rad, soft, r, g, b, a, ga, gb, u0, v0, us, vs, mode; } Vert;
static Vert batch[4096 * 4];
static int nquads;
static GLuint batch_tex;
static int draw_calls; /* per frame, for the profile line */

static void flush(void) {
    if (!nquads) return;
    glBindBuffer(GL_ARRAY_BUFFER, batch_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(Vert) * 4 * (size_t)nquads), batch, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, batch_ibo);
    static const struct { int n, off; } at[7] = {{2, 0}, {4, 2}, {2, 6}, {4, 8}, {2, 12}, {4, 14}, {1, 18}};
    for (int i = 0; i < 7; i++) {
        glEnableVertexAttribArray((GLuint)i);
        glVertexAttribPointer((GLuint)i, at[i].n, GL_FLOAT, GL_FALSE, sizeof(Vert), (const void *)(sizeof(float) * (size_t)at[i].off));
    }
    if (batch_tex) glBindTexture(GL_TEXTURE_2D, batch_tex);
    glDrawElements(GL_TRIANGLES, nquads * 6, GL_UNSIGNED_SHORT, 0);
    draw_calls++;
    nquads = 0;
}

static void draw(float x, float y, float w, float h, float radius, float soft, Col c,
                 const Tex *tex, float ga, float gb) {
    float mode = tex && tex->id ? tex_mode : 0.f;
    /* an untextured rect never breaks a batch; a textured one only when its texture differs */
    if (mode > 0 && tex->id != batch_tex) { flush(); batch_tex = tex->id; }
    if (nquads >= 4096) flush();
    float pad = soft > 1.f ? soft : 1.f, sf = soft > 0.5f ? soft : 0.5f;
    float x0 = x - pad, y0 = y - pad, x1 = x + w + pad, y1 = y + h + pad, cx = x + w * 0.5f, cy = y + h * 0.5f;
    Vert *v = &batch[nquads * 4];
    for (int k = 0; k < 4; k++) {
        float px = (k & 1) ? x1 : x0, py = (k & 2) ? y1 : y0;
        v[k] = (Vert){px, py, px - cx, py - cy, w, h, radius, sf, c.r, c.g, c.b, c.a * g_alpha,
                      ga, gb, g_uvr[0], g_uvr[1], g_uvr[2], g_uvr[3], mode};
    }
    nquads++;
}
/* a glyph-like image drawn as a mask: only its alpha, in colour c */
static void draw_mask(float x, float y, float w, float h, Col c, const Tex *tex) {
    tex_mode = 2.f;
    draw(x, y, w, h, 0, 0, c, tex, 1, 1);
    tex_mode = 1.f;
}
static void fill(float x, float y, float w, float h, float r, Col c) { draw(x, y, w, h, r, 0, c, NULL, 1, 1); }

static Tex upload(SDL_Surface *s) {
    Tex t = {0, 0, 0};
    if (!s) return t;
    /* the decoder thread hands surfaces over already in GL's byte order */
    SDL_Surface *c = s->format->format == SDL_PIXELFORMAT_ABGR8888 ? s : SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
    if (!c) return t;
    glGenTextures(1, &t.id); glBindTexture(GL_TEXTURE_2D, t.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (c->pitch == c->w * 4) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->w, c->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, c->pixels);
    } else { /* repack rows */
        unsigned char *buf = malloc((size_t)c->w * c->h * 4);
        for (int y = 0; y < c->h; y++) memcpy(buf + (size_t)y * c->w * 4, (unsigned char *)c->pixels + (size_t)y * c->pitch, (size_t)c->w * 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->w, c->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        free(buf);
    }
    t.w = c->w; t.h = c->h;
    if (c != s) SDL_FreeSurface(c);
    return t;
}
static void tex_free(Tex *t) { if (t->id) glDeleteTextures(1, &t->id); t->id = 0; }

/* Decoding runs on its own thread (see "decoder" below), so these touch no GL and no log. */
static SDL_Surface *decode_png(const char *path) {
    SDL_Surface *s = IMG_Load(path);
    if (!s || s->format->format == SDL_PIXELFORMAT_ABGR8888) return s;
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(s);
    return c;
}

/* src scaled and centre-cropped to fill w x h, as a new surface; src is freed. Averages a block of
 * source pixels for each one when shrinking, interpolates when not. On the decoder thread: the
 * TV's memory should not depend on the size of a picture someone chose. */
static SDL_Surface *fit(SDL_Surface *src, int w, int h) {
    if (src->w == w && src->h == h) return src;
    SDL_Surface *d = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!d) return src;
    double ca = (double)w / h, cw = src->w, ch = src->h;
    if (cw / ch > ca) cw = ch * ca; else ch = cw / ca;
    double sx = cw / w, sy = ch / h, ox = (src->w - cw) * 0.5, oy = (src->h - ch) * 0.5;
    int fx = sx >= 2 ? (int)sx : 1, fy = sy >= 2 ? (int)sy : 1;
    const unsigned char *in = src->pixels;
    for (int y = 0; y < h; y++) {
        unsigned char *o = (unsigned char *)d->pixels + (size_t)y * d->pitch;
        double py = oy + y * sy;
        int y0 = (int)py, y1 = y0 + 1 < src->h ? y0 + 1 : y0;
        if (y0 >= src->h) y0 = y1 = src->h - 1;
        int wy = (int)((py - y0) * 256);
        for (int x = 0; x < w; x++, o += 4) {
            double px = ox + x * sx;
            int x0 = (int)px, x1 = x0 + 1 < src->w ? x0 + 1 : x0;
            if (x0 >= src->w) x0 = x1 = src->w - 1;
            if (fx > 1 || fy > 1) {
                unsigned sum[3] = {0, 0, 0}, n = 0;
                for (int yy = y0; yy < y0 + fy && yy < src->h; yy++) {
                    const unsigned char *q = in + (size_t)yy * src->pitch + (size_t)x0 * 4;
                    for (int xx = 0; xx < fx && x0 + xx < src->w; xx++, q += 4, n++) { sum[0] += q[0]; sum[1] += q[1]; sum[2] += q[2]; }
                }
                for (int c = 0; c < 3; c++) o[c] = (unsigned char)(sum[c] / n);
            } else {
                int wx = (int)((px - x0) * 256);
                const unsigned char *a = in + (size_t)y0 * src->pitch, *b = in + (size_t)y1 * src->pitch;
                for (int c = 0; c < 3; c++) {
                    int top = a[x0 * 4 + c] * (256 - wx) + a[x1 * 4 + c] * wx, bot = b[x0 * 4 + c] * (256 - wx) + b[x1 * 4 + c] * wx;
                    o[c] = (unsigned char)((top * (256 - wy) + bot * wy) >> 16);
                }
            }
            o[3] = 255;
        }
    }
    SDL_FreeSurface(src);
    return d;
}

/* The wallpaper carries its own scrims (darker top for the clock, darker bottom for the rows),
 * baked into the pixels once at load: drawing them as full-screen blended quads every frame
 * is what kept the Mali-G51 at ~50 fps. */
static void scrim_wallpaper(SDL_Surface *c) {
    for (int y = 0; y < c->h; y++) {
        float fy = y / (float)c->h, k = 1.f;
        if (fy < 0.16f) k = 0.72f + 0.28f * (fy / 0.16f);              /* top scrim */
        if (fy > 0.62f) { float t = (fy - 0.62f) / 0.38f; k = 1.f - 0.62f * t * t; } /* bottom fade */
        if (k >= 0.999f) continue;
        unsigned char *row = (unsigned char *)c->pixels + (size_t)y * c->pitch;
        int m = (int)(k * 256.f);
        for (int x = 0; x < c->w * 4; x += 4) {
            row[x] = (unsigned char)(row[x] * m >> 8);
            row[x + 1] = (unsigned char)(row[x + 1] * m >> 8);
            row[x + 2] = (unsigned char)(row[x + 2] * m >> 8);
        }
    }
}

/* The frosted copy of the wallpaper behind the dashboard and Recents: an eighth of its size
 * (bilinear filtering does the rest when it is stretched back), box-blurred three times (close
 * to a Gaussian), and darkened so white cards and text stay legible on it. */
static void box_blur(unsigned char *p, int w, int h, int r, int dx) {
    int n = dx ? w : h, lines = dx ? h : w;
    int *acc = malloc(sizeof(int) * (size_t)n * 3);
    for (int l = 0; l < lines; l++) {
        unsigned char *base = dx ? p + (size_t)l * w * 4 : p + (size_t)l * 4;
        size_t step = dx ? 4 : (size_t)w * 4;
        for (int c = 0; c < 3; c++) {
            int sum = 0;
            for (int i = -r; i <= r; i++) sum += base[(size_t)(i < 0 ? 0 : i >= n ? n - 1 : i) * step + c];
            for (int i = 0; i < n; i++) {
                acc[i * 3 + c] = sum / (2 * r + 1);
                int out = i - r, in = i + r + 1;
                sum += base[(size_t)(in >= n ? n - 1 : in) * step + c] - base[(size_t)(out < 0 ? 0 : out) * step + c];
            }
        }
        for (int i = 0; i < n; i++)
            for (int c = 0; c < 3; c++) base[(size_t)i * step + c] = (unsigned char)acc[i * 3 + c];
    }
    free(acc);
}
static SDL_Surface *frost(SDL_Surface *src) {
    const int f = 8, w = src->w / f, h = src->h / f;
    if (w < 8 || h < 8) return NULL;
    SDL_Surface *d = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!d) return NULL;
    for (int y = 0; y < h; y++) {
        unsigned char *o = (unsigned char *)d->pixels + (size_t)y * d->pitch;
        for (int x = 0; x < w; x++) {
            unsigned sum[3] = {0, 0, 0};
            for (int yy = 0; yy < f; yy++) {
                const unsigned char *q = (const unsigned char *)src->pixels + (size_t)(y * f + yy) * src->pitch + (size_t)x * f * 4;
                for (int xx = 0; xx < f; xx++) { sum[0] += q[xx * 4]; sum[1] += q[xx * 4 + 1]; sum[2] += q[xx * 4 + 2]; }
            }
            for (int c = 0; c < 3; c++) o[x * 4 + c] = (unsigned char)(sum[c] / (f * f));
            o[x * 4 + 3] = 255;
        }
    }
    unsigned char *p = d->pixels;
    for (int k = 0; k < 3; k++) { box_blur(p, w, h, 4, 1); box_blur(p, w, h, 4, 0); }
    for (int y = 0; y < h; y++) { /* darken: 35% of the picture over a little grey */
        unsigned char *o = (unsigned char *)d->pixels + (size_t)y * d->pitch;
        for (int x = 0; x < w * 4; x += 4)
            for (int c = 0; c < 3; c++) o[x + c] = (unsigned char)(o[x + c] * 0.35f + 10);
    }
    return d;
}

/* ---------- text ---------- */

static TTF_Font *f_clock, *f_date;

/* Render s, shortened with an ellipsis until it is at most maxw px wide (0 = no limit). */
static Tex text_fit(TTF_Font *f, const char *s, int maxw) {
    Tex t = {0, 0, 0};
    if (!f || !s || !*s) return t;
    char buf[256 + 4]; scpy(buf, 256, s);
    int w = 0, h = 0;
    TTF_SizeUTF8(f, buf, &w, &h);
    if (maxw > 0 && w > maxw) {
        char full[256]; scpy(full, sizeof full, s);
        /* the longest start that fits with its ellipsis: lo bytes do, hi do not */
        size_t lo = 0, hi = strlen(full);
        while (hi - lo > 1) {
            size_t m = (lo + hi) / 2;
            while (m > lo + 1 && ((unsigned char)full[m] & 0xC0) == 0x80) m--; /* a whole UTF-8 char */
            memcpy(buf, full, m); memcpy(buf + m, "\xE2\x80\xA6", 4); /* … */
            TTF_SizeUTF8(f, buf, &w, &h);
            if (w <= maxw) lo = m; else hi = m;
        }
        while (lo > 0 && ((unsigned char)full[lo] & 0xC0) == 0x80) lo--;
        while (lo > 0 && full[lo - 1] == ' ') lo--;
        memcpy(buf, full, lo); memcpy(buf + lo, "\xE2\x80\xA6", 4);
    }
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, buf, white);
    t = upload(surf); SDL_FreeSurface(surf);
    return t;
}
static void draw_text(const Tex *t, float x, float y, Col c, int shadow) {
    if (!t->id) return;
    if (shadow) draw(x + 2, y + 2, (float)t->w, (float)t->h, 0, 0, rgba(0, 0, 0, 0.45f * c.a), t, 1, 1);
    draw(x, y, (float)t->w, (float)t->h, 0, 0, c, t, 1, 1);
}

/* ---------- locale ----------
 * The TV's UI language comes from settingsservice (localeInfo.locales.UI, e.g. "pt-PT"). The
 * firmware carries no glibc locale data, so dates and our few strings come from small built-in
 * tables (English when a language is missing). App, input and channel names come from the
 * system and are already localized. */

static char ui_lang[8] = "en";
static int hour12; /* the clock: 3:12 PM rather than 15:12 */

enum { S_MOBILE, S_TVIN, S_GUIDE, S_CHANNELS, S_CHLIST, S_CHLIST_SUB, S_NOINFO, S_EDIT_HINT,
       S_CABLE, S_ANTENNA, S_SATELLITE, S_LIVETV, S_BROWSE_HINT, S_DELETE_Q, S_DELETE, S_CANCEL,
       S_RECENTS, S_CLOSE_ALL, S_FIRST_HINT, S_NO_RECENTS, S_CANT_OPEN, NSTR };
static const char *const str_en[NSTR] = {
    "Mobile", "TV / Inputs", "Connection Guide", "Frequently Watched Channels", "View channel list",
    "You can browse all channels", "No information", "Use the arrows to move    OK to put down",
    "Cable", "Antenna", "Satellite", "Live TV",
    "OK to move    Up to hide or delete    Back when done", "Delete \xe2\x80\x9c%s\xe2\x80\x9d?", "Delete", "Cancel",
    "Recents", "Close All", "Hold OK on an app to edit    Hold blue for the screen saver",
    "No recent apps", "Couldn't open %s"};
static const char *const str_pt[NSTR] = {
    "Telem\xc3\xb3vel", "TV / Entradas", "Guia de Liga\xc3\xa7\xc3\xa3o", "Canais Vistos Frequentemente",
    "Ver lista de canais", "Pode explorar todos os canais", "Sem informa\xc3\xa7\xc3\xa3o",
    "Use as setas para mover    OK para pousar", "Cabo", "Antena", "Sat\xc3\xa9lite", "Live TV",
    "OK para mover    Cima para ocultar ou eliminar    Voltar para terminar", "Eliminar \xe2\x80\x9c%s\xe2\x80\x9d?", "Eliminar", "Cancelar",
    "Listas Recentes", "Fechar Tudo", "Mantenha OK numa app para editar    Mantenha o azul para a prote\xc3\xa7\xc3\xa3o de ecr\xc3\xa3",
    "Sem aplica\xc3\xa7\xc3\xb5" "es recentes", "N\xc3\xa3o foi poss\xc3\xadvel abrir %s"};
static const char *const str_es[NSTR] = {
    "M\xc3\xb3vil", "TV / Entradas", "Gu\xc3\xad" "a de conexi\xc3\xb3n", "Canales m\xc3\xa1s vistos",
    "Ver lista de canales", "Explora todos los canales", "Sin informaci\xc3\xb3n",
    "Usa las flechas para mover    OK para soltar", "Cable", "Antena", "Sat\xc3\xa9lite", "TV en directo",
    "OK para mover    Arriba para ocultar o eliminar    Atr\xc3\xa1s para terminar", "\xc2\xbf" "Eliminar \xe2\x80\x9c%s\xe2\x80\x9d?", "Eliminar", "Cancelar",
    "Recientes", "Cerrar todo", "Mant\xc3\xa9n OK en una app para editar    Mant\xc3\xa9n azul para el salvapantallas",
    "No hay aplicaciones recientes", "No se pudo abrir %s"};
static const char *const str_fr[NSTR] = {
    "Mobile", "TV / Entr\xc3\xa9" "es", "Guide de connexion", "Cha\xc3\xaennes les plus regard\xc3\xa9" "es",
    "Voir la liste des cha\xc3\xaennes", "Parcourez toutes les cha\xc3\xaennes", "Aucune information",
    "Fl\xc3\xa8" "ches pour d\xc3\xa9placer    OK pour poser", "C\xc3\xa2" "ble", "Antenne", "Satellite", "TV en direct",
    "OK pour d\xc3\xa9placer    Haut pour masquer ou supprimer    Retour pour terminer", "Supprimer \xe2\x80\x9c%s\xe2\x80\x9d ?", "Supprimer", "Annuler",
    "R\xc3\xa9" "cents", "Tout fermer", "Maintenez OK sur une app pour modifier    Maintenez bleu pour l\xe2\x80\x99\xc3\xa9" "conomiseur",
    "Aucune application r\xc3\xa9" "cente", "Impossible d\xe2\x80\x99ouvrir %s"};
static const char *const str_de[NSTR] = {
    "Mobil", "TV / Eing\xc3\xa4nge", "Verbindungsanleitung", "H\xc3\xa4ufig gesehene Sender",
    "Senderliste anzeigen", "Alle Sender durchsuchen", "Keine Informationen",
    "Mit den Pfeilen bewegen    OK zum Ablegen", "Kabel", "Antenne", "Satellit", "Live-TV",
    "OK zum Bewegen    Hoch zum Ausblenden oder L\xc3\xb6schen    Zur\xc3\xbc" "ck zum Beenden", "\xe2\x80\x9e%s\xe2\x80\x9c l\xc3\xb6schen?", "L\xc3\xb6schen", "Abbrechen",
    "Zuletzt verwendet", "Alle schlie\xc3\x9f" "en", "OK auf einer App halten zum Bearbeiten    Blau halten f\xc3\xbcr den Bildschirmschoner",
    "Keine zuletzt verwendeten Apps", "%s konnte nicht ge\xc3\xb6" "ffnet werden"};
static const char *const str_it[NSTR] = {
    "Mobile", "TV / Ingressi", "Guida alla connessione", "Canali pi\xc3\xb9 visti",
    "Vedi elenco canali", "Sfoglia tutti i canali", "Nessuna informazione",
    "Usa le frecce per spostare    OK per posare", "Cavo", "Antenna", "Satellite", "TV in diretta",
    "OK per spostare    Su per nascondere o eliminare    Indietro per finire", "Eliminare \xe2\x80\x9c%s\xe2\x80\x9d?", "Elimina", "Annulla",
    "Recenti", "Chiudi tutto", "Tieni premuto OK su un\xe2\x80\x99" "app per modificare    Tieni premuto blu per il salvaschermo",
    "Nessuna app recente", "Impossibile aprire %s"};
static const char *S(int k) {
    static const struct { const char *lang; const char *const *s; } tr[] = {
        {"pt", str_pt}, {"es", str_es}, {"fr", str_fr}, {"de", str_de}, {"it", str_it}};
    for (unsigned i = 0; i < sizeof tr / sizeof *tr; i++) if (!strcmp(ui_lang, tr[i].lang)) return tr[i].s[k];
    return str_en[k];
}

typedef struct { const char *lang, *wd[7], *mo[12], *fmt; } DateLocale;
/* fmt: W weekday, D day, M month */
static const DateLocale date_locales[] = {
    {"pt", {"domingo", "segunda-feira", "ter\xc3\xa7" "a-feira", "quarta-feira", "quinta-feira", "sexta-feira", "s\xc3\xa1" "bado"},
     {"janeiro", "fevereiro", "mar\xc3\xa7o", "abril", "maio", "junho", "julho", "agosto", "setembro", "outubro", "novembro", "dezembro"}, "W, D de M"},
    {"es", {"domingo", "lunes", "martes", "mi\xc3\xa9rcoles", "jueves", "viernes", "s\xc3\xa1" "bado"},
     {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"}, "W, D de M"},
    {"fr", {"dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"},
     {"janvier", "f\xc3\xa9vrier", "mars", "avril", "mai", "juin", "juillet", "ao\xc3\xbbt", "septembre", "octobre", "novembre", "d\xc3\xa9" "cembre"}, "W D M"},
    {"de", {"Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag"},
     {"Januar", "Februar", "M\xc3\xa4rz", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November", "Dezember"}, "W, D. M"},
    {"it", {"domenica", "luned\xc3\xac", "marted\xc3\xac", "mercoled\xc3\xac", "gioved\xc3\xac", "venerd\xc3\xac", "sabato"},
     {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre"}, "W D M"},
    {"en", {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
     {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"}, "W, D M"},
};

static void format_date(char *out, size_t n, const struct tm *tm) {
    const DateLocale *L = &date_locales[sizeof date_locales / sizeof *date_locales - 1];
    for (unsigned i = 0; i < sizeof date_locales / sizeof *date_locales; i++)
        if (!strcmp(date_locales[i].lang, ui_lang)) L = &date_locales[i];
    size_t o = 0;
    for (const char *f = L->fmt; *f && o + 1 < n; f++) {
        int w = 0;
        if (*f == 'W') w = snprintf(out + o, n - o, "%s", L->wd[tm->tm_wday]);
        else if (*f == 'D') w = snprintf(out + o, n - o, "%d", tm->tm_mday);
        else if (*f == 'M') w = snprintf(out + o, n - o, "%s", L->mo[tm->tm_mon]);
        else { out[o] = *f; w = 1; }
        o += (size_t)w < n - o ? (size_t)w : n - o - 1;
    }
    out[o] = 0;
    /* a sentence starts with a capital, as LG's own clock writes it */
    if ((unsigned char)out[0] >= 'a' && (unsigned char)out[0] <= 'z') out[0] = (char)(out[0] - 32);
}

/* ---------- model ----------
 * The page is a column of rows, as on LG's home: input pills and the app tiles over the
 * wallpaper; below them (Down from the apps scrolls the page up, LG's "dashboard") the Home
 * Dashboard / Mobile / TV & Inputs cards and the frequently watched channels. LG's Browser shelf
 * and footer buttons are left out: they repeat the top buttons and the app row. */

typedef enum { K_APP, K_PILL, K_HOMEDASH, K_WHITE, K_CHANNEL, K_RECENT, K_TOP } Kind;

typedef struct {
    char id[200];    /* identity across updates (launch point, input, channel, content id) */
    char app[160];   /* what launch() starts */
    char lpid[200];  /* launchPointId, for moveLaunchPoint */
    char title[200], sub[200], sub2[200], group[80];
    char icon[512], image[512];
    char *params;    /* launch params, JSON */
    Col color;
    Tex icon_tex, image_tex, title_tex, sub_tex, sub2_tex;
    int icon_state;  /* 0 pending, 1 tried */
    int show_title;  /* apps: LG shows the name on focus only when titleVisible, else only system apps */
    int removable;   /* apps: LG offers the trash only for these */
    unsigned glyph;  /* top buttons: LG's Dingbat codepoint */
    int highlight;   /* channels: the channel the TV is on */
    float progress;  /* channels: how far into the current programme, <0 none */
    float focus, press;
    float zoom, zin, z0; /* cards: LG's focus zoom (zoom_step) */
    double fstart;   /* when it took focus (title scrolling) */
    int hidden;      /* apps: on our hidden list (shown only in edit mode) */
    float hf;        /* apps: 0 shown .. 1 collapsed out of the row */
    float life;      /* 0..1: how much of its slot the item takes; grows in when added, shrinks out when removed */
    int dying;
    float ax; int ax_set; /* animated x: reorders and inserts glide instead of jumping */
    float tx;             /* target x this frame, from layout_x() */
} Item;

typedef struct {
    Item *items; int n, sel;
    Kind kind;
    float scroll, y, gap, x0;
    const char *label; /* heading above the row (cards rows), NULL for none */
    int back_sel, back_ok; /* where focus left this row going up or down, until a move sideways */
} Row;

/* ROW_RECENTS is not part of the page: it lives in the Recents panel Back opens */
enum { ROW_TOP, ROW_INPUTS, ROW_APPS, ROW_DASH, ROW_CHANNELS, ROW_RECENTS, NROWS };
#define LAST_PAGE_ROW ROW_CHANNELS
static Row rows[NROWS];
static int cur_row = ROW_APPS;
static int dirty = 1;
static int on_screen = 1; /* the window is shown (another app, or the TV in standby, hides it) */
static Tex clock_tex, date_tex;
static char clock_str[16];
/* OLED care: after a minute with no input Home dims, and the clock moves a few pixels each
 * minute, so nothing sits brighter in one place for hours */
#define IDLE_DIM_MS 60000.0
#define IDLE_DIM 0.45f
static double last_input_ms;
static float dim_f, clock_dx, clock_dy;
/* the first time Home is used, a hint for the two hidden gestures, until a key or 12 s; then the
 * flag file keeps it from coming back. It starts on the first key, the one sure sign that someone
 * is looking: as Home the window is preloaded, and even gets focus, behind Live TV at boot. */
#define HINTED_PATH "/var/lib/homenative/hinted"
enum { HINT_PENDING, HINT_SHOWING, HINT_DONE };
static int hint_state;
static double hint_since;
static float hint_f;
static void hint_done(void) {
    if (hint_state == HINT_DONE) return;
    hint_state = HINT_DONE;
    mkdir("/var/lib/homenative", 0755);
    FILE *f = fopen(HINTED_PATH, "w");
    if (f) fclose(f);
    logf_("first hint: done%s", f ? "" : " (cannot write the flag)");
}
static void hint_input(void) {
    if (hint_state == HINT_PENDING) { hint_state = HINT_SHOWING; hint_since = now_ms(); logf_("first hint"); }
    else if (hint_state == HINT_SHOWING && now_ms() - hint_since > 1500) hint_done();
}
static long tz_offset; /* seconds, from systemservice */
static char app_dir[PATH_MAX];
static float page, page_target; /* vertical page scroll */

/* LG's tile, measured off its home at 1080p */
#define TILE 124.f
#define TILE_R 12.f
#define TILE_FOCUS 1.24f
#define PILL_H 54.f
#define PILL_MAXW 300
#define CARD_R 12.f
#define DASH_PAGE 880.f /* page scroll that puts the apps row at the top of the screen */

static float kind_h(Kind k) {
    switch (k) {
    case K_APP: return TILE;
    case K_PILL: return PILL_H;
    case K_HOMEDASH: return 192;
    case K_WHITE: return 192;  /* MobileComponent: homeConnectHeight */
    case K_CHANNEL: return 216;
    case K_RECENT: return 189;
    case K_TOP: return 60;
    }
    return 100;
}
static float row_h(const Row *r) { return kind_h(r->kind); }

static float item_w(const Row *r, const Item *it) {
    switch (r->kind) {
    case K_APP: return TILE;
    case K_PILL: {
        float w = 40.f + (it->title_tex.id ? it->title_tex.w : 80);
        if (it->icon_tex.id) w += 38.f;
        return w;
    }
    case K_WHITE: return 308; /* homeConnectWidth, the Home Dashboard card too */
    case K_HOMEDASH: return 308;
    case K_CHANNEL: return 308;
    case K_RECENT: return 336;
    case K_TOP: return 60;
    }
    return 100;
}
static Kind item_kind(const Row *r, const Item *it) { return r->kind == K_WHITE && it->image[0] ? K_HOMEDASH : r->kind; }

/* Textures outlive the rows: a refresh rebuilds rows from luna and must not decode or
 * rasterise everything again. Items hold non-owning copies. */
/* open addressing on an FNV hash: render looks headings and labels up every frame */
#define TCACHE_SLOTS 2048
#define TCACHE_SWEEP 1024 /* entries that start a sweep (tcache_sweep) */
#define TCACHE_BYTES (24 * 1024 * 1024) /* so do this many bytes of pixels */
typedef struct { char *key; unsigned h; Tex t; double used; } CacheEnt;
static CacheEnt tcache[TCACHE_SLOTS];
static int ntcache; /* entries; update() reads it to tell a real decode from a cache hit */
static long tcache_bytes;
static long tex_bytes(const Tex *t) { return (long)t->w * t->h * 4; }
static unsigned fnv(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
static CacheEnt *tcache_slot(const char *key, unsigned h) {
    for (unsigned i = h & (TCACHE_SLOTS - 1);; i = (i + 1) & (TCACHE_SLOTS - 1))
        if (!tcache[i].key || (tcache[i].h == h && !strcmp(tcache[i].key, key))) return &tcache[i];
}
static const Tex *tcache_get(const char *key) {
    CacheEnt *e = tcache_slot(key, fnv(key));
    if (!e->key) return NULL;
    e->used = now_ms();
    return &e->t;
}
static Tex tcache_put(const char *key, Tex t) {
    /* full enough: stop caching rather than probe forever (the next sweep makes room; the
     * texture is freed with nothing to find it again, so this must stay rare) */
    if (ntcache >= TCACHE_SLOTS * 3 / 4) { logf_("tcache full: %s", key); return t; }
    unsigned h = fnv(key);
    CacheEnt *e = tcache_slot(key, h);
    if (!e->key) { e->key = strdup(key); e->h = h; ntcache++; }
    tcache_bytes += tex_bytes(&t) - tex_bytes(&e->t);
    e->t = t; e->used = now_ms();
    return t;
}
/* Home runs for weeks: every programme name is a new text, and every time an app is used, a new
 * snapshot. Past TCACHE_SWEEP entries, textures no item holds and nothing looked up for a minute
 * are deleted; past TCACHE_BYTES the minute becomes a second. Runs between frames only (update()),
 * so no texture a frame is drawing goes away. Empty entries stay: they remember what is missing. */
static int cmp_gluint(const void *a, const void *b) { GLuint x = *(const GLuint *)a, y = *(const GLuint *)b; return x < y ? -1 : x > y; }
static void tcache_sweep(int force) {
    static double last;
    int heavy = tcache_bytes > TCACHE_BYTES;
    if (!force && ((ntcache < TCACHE_SWEEP && !heavy) || now_ms() - last < 30 * 1000.0)) return;
    /* forced (dev "sweep"): everything not drawn in the last second goes, to test re-creating it */
    double a = last = now_ms(), cutoff = a - (force || heavy ? 1000.0 : 60 * 1000.0);
    /* the textures items hold, sorted, to look each entry up in */
    static GLuint *held; static size_t cap;
    size_t nh = 0;
    for (int ri = 0; ri < NROWS; ri++)
        for (int i = 0; i < rows[ri].n; i++) {
            const Item *it = &rows[ri].items[i];
            if (nh + 5 > cap) { cap = cap ? cap * 2 : 512; held = realloc(held, cap * sizeof *held); }
            held[nh++] = it->icon_tex.id; held[nh++] = it->image_tex.id; held[nh++] = it->title_tex.id;
            held[nh++] = it->sub_tex.id; held[nh++] = it->sub2_tex.id;
        }
    if (nh) qsort(held, nh, sizeof *held, cmp_gluint);
    static CacheEnt keep[TCACHE_SLOTS];
    int nk = 0, freed = 0;
    for (int i = 0; i < TCACHE_SLOTS; i++) {
        CacheEnt *e = &tcache[i];
        if (!e->key) continue;
        if (!e->t.id || e->used > cutoff || (nh && bsearch(&e->t.id, held, nh, sizeof *held, cmp_gluint))) keep[nk++] = *e;
        else { tcache_bytes -= tex_bytes(&e->t); glDeleteTextures(1, &e->t.id); free(e->key); freed++; }
    }
    memset(tcache, 0, sizeof tcache); ntcache = 0;
    for (int k = 0; k < nk; k++) { *tcache_slot(keep[k].key, keep[k].h) = keep[k]; ntcache++; }
    logf_("tcache sweep: %d freed, %d kept, %.1f MB, %.1f ms", freed, nk, tcache_bytes / 1e6, now_ms() - a);
}

/* A font is a weight and a pixel size, opened on first use: FR(27) is LG Smart UI Regular 27,
 * FS(21) SemiBold 21. The sizes are LG's own (its GlobalStyle getRValue(n) is n px at 1080p). */
#define FR(n) (n)
#define FS(n) (1000 + (n))
enum { F_PILL = FS(24), F_TILE = FS(21), F_CARD_TITLE = FS(27), F_CARD_SUB = FR(24), F_CARD_NAME = FS(30),
       F_HEAD = FR(30), F_HEAD_FOCUS = FS(30), F_TITLE = FR(45) };
static TTF_Font *font(int f) {
    static TTF_Font *open[2][128];
    int bold = f >= 1000, px = bold ? f - 1000 : f;
    if (px <= 0 || px >= 128) px = 24;
    if (!open[bold][px])
        open[bold][px] = TTF_OpenFont(bold ? "/usr/share/fonts/LG_Smart_UI-SemiBold.ttf" : "/usr/share/fonts/LG_Smart_UI-Regular.ttf", px);
    return open[bold][px];
}
/* LG's UI font is Latin, Greek and Cyrillic. A name in another script is drawn whole in the first
 * of the TV's other fonts that has it. SDL_ttf neither shapes nor reorders, so Arabic and Hebrew
 * show their letters, not joined up. */
static TTF_Font *font_for(int f, const char *s) {
    TTF_Font *base = font(f);
    if (!base || !s) return base;
    static const char *const files[] = {"/usr/share/fonts/LG_Smart_UI_JP.ttf", "/usr/share/fonts/LG_Smart_UI_AR_HE_TH-Regular.ttf", "/usr/share/fonts/DroidSansFallback.ttf"};
    enum { NFILES = sizeof files / sizeof *files };
    static TTF_Font *open[NFILES][128];
    int px = f >= 1000 ? f - 1000 : f;
    if (px <= 0 || px >= 128) px = 24;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0xC0) continue;
        unsigned cp = *p >= 0xF0 ? 0xFFFF : *p >= 0xE0 ? (*p & 15u) << 12 | (p[1] & 63u) << 6 | (p[1] ? p[2] & 63u : 0) : (*p & 31u) << 6 | (p[1] & 63u);
        if (cp == 0xFFFF || TTF_GlyphIsProvided(base, (Uint16)cp)) continue;
        for (int i = 0; i < NFILES; i++) {
            if (!open[i][px]) open[i][px] = TTF_OpenFont(files[i], px);
            if (open[i][px] && TTF_GlyphIsProvided(open[i][px], (Uint16)cp)) return open[i][px];
        }
        break;
    }
    return base;
}
/* LG's top buttons are glyphs of its "Sandstone Icons" font above U+FFFF, which SDL_ttf 2.0.14 (UCS-2)
 * cannot reach, so FreeType renders them directly */
static Tex glyph_tex(unsigned cp, int px) {
    static FT_Library lib;
    static FT_Face face;
    Tex z = {0, 0, 0};
    char key[40]; snprintf(key, sizeof key, "G%x/%d", cp, px);
    const Tex *c = tcache_get(key);
    if (c) return *c;
    if (!lib && FT_Init_FreeType(&lib)) return z;
    if (!face && FT_New_Face(lib, "/usr/share/fonts/Sandstone_Icons.ttf", 0, &face)) { logf_("icon font missing"); return z; }
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px);
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER) || !face->glyph->bitmap.width) { logf_("glyph %x missing", cp); return tcache_put(key, z); }
    FT_Bitmap *b = &face->glyph->bitmap;
    SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, (int)b->width, (int)b->rows, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!sf) return z;
    for (unsigned y = 0; y < b->rows; y++) {
        unsigned char *dst = (unsigned char *)sf->pixels + (size_t)y * sf->pitch;
        for (unsigned x = 0; x < b->width; x++) {
            dst[x * 4] = dst[x * 4 + 1] = dst[x * 4 + 2] = 255;
            dst[x * 4 + 3] = b->buffer[y * b->pitch + x];
        }
    }
    Tex t = upload(sf); SDL_FreeSurface(sf);
    return tcache_put(key, t);
}

/* an eye, drawn here (LG's Dingbat font has none): an almond outline and a pupil, crossed out
 * when slashed. A white mask, 40 x 28, anti-aliased by distance. */
static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, t = ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy);
    t = fminf(1, fmaxf(0, t));
    return hypotf(px - ax - t * vx, py - ay - t * vy);
}
static Tex eye_tex(int slashed) {
    char key[16]; snprintf(key, sizeof key, "eye%d", slashed);
    const Tex *c = tcache_get(key);
    if (c) return *c;
    const int W_ = 40, H_ = 28;
    SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, W_, H_, 32, SDL_PIXELFORMAT_ABGR8888);
    Tex z = {0, 0, 0};
    if (!sf) return z;
    const float cx = W_ * 0.5f, cy = H_ * 0.5f, a = 18, b = 10, R = (a * a + b * b) / (2 * b), off = R - b;
    for (int y = 0; y < H_; y++) {
        unsigned char *row = (unsigned char *)sf->pixels + (size_t)y * sf->pitch;
        for (int x = 0; x < W_; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            /* the almond: where two circles overlap; its edge is a 2.4 px stroke */
            float sd = fmaxf(hypotf(px - cx, py - (cy + off)) - R, hypotf(px - cx, py - (cy - off)) - R);
            float al = fminf(1, fmaxf(0, 1.7f - fabsf(sd)));
            al = fmaxf(al, fminf(1, fmaxf(0, 5.5f - hypotf(px - cx, py - cy)))); /* pupil */
            if (slashed) {
                float d = seg_dist(px, py, 7, H_ - 2, W_ - 7, 2);
                al *= fminf(1, fmaxf(0, d - 2.2f)); /* a gap either side of the slash */
                al = fmaxf(al, fminf(1, fmaxf(0, 1.7f - d)));
            }
            row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 255;
            row[x * 4 + 3] = (unsigned char)(al * 255);
        }
    }
    Tex t = upload(sf); SDL_FreeSurface(sf);
    return tcache_put(key, t);
}

static Tex text_cached(int f, int maxw, const char *s) {
    if (!s || !*s) { Tex z = {0, 0, 0}; return z; }
    char key[300]; snprintf(key, sizeof key, "T%d/%d:%s", f, maxw, s);
    const Tex *c = tcache_get(key);
    return c ? *c : tcache_put(key, text_fit(font_for(f, s), s, maxw));
}

static Item *row_add(Row *r) {
    r->items = realloc(r->items, sizeof(Item) * (size_t)(r->n + 1));
    Item *it = &r->items[r->n++];
    memset(it, 0, sizeof *it);
    it->progress = -1;
    return it;
}

/* Replace r's items by those of nw, animating the difference: kept items (same id) keep their
 * state (focus, position, life), new ones grow in from nothing, removed ones stay where they were
 * as dying and shrink away, so neighbours slide together. A reorder glides (animated x). The
 * first fill appears at once. */
static void row_merge(Row *r, Row *nw) {
    char keep[200] = "";
    if (r->n) scpy(keep, sizeof keep, r->items[r->sel].id);
    int first = r->n == 0;
    Item *out = malloc(sizeof(Item) * (size_t)(r->n + nw->n + 1));
    char *matched = calloc((size_t)r->n + 1, 1);
    int n = 0;
    for (int j = 0; j < nw->n; j++) {
        Item it = nw->items[j];
        it.life = first ? 1 : 0;
        for (int i = 0; i < r->n; i++) {
            Item *o = &r->items[i];
            if (matched[i] || o->dying || strcmp(o->id, it.id)) continue;
            matched[i] = 1;
            it.focus = o->focus; it.press = o->press; it.life = o->life; it.ax = o->ax; it.ax_set = o->ax_set; it.hf = o->hf;
            it.zoom = o->zoom; it.zin = o->zin; it.z0 = o->z0; it.fstart = o->fstart; /* a list update is no reason to bounce */
            /* until load_item has looked again, what was there */
            it.icon_tex = o->icon_tex; it.image_tex = o->image_tex; it.title_tex = o->title_tex; it.sub_tex = o->sub_tex; it.sub2_tex = o->sub2_tex;
            break;
        }
        out[n++] = it;
    }
    for (int i = 0; i < r->n; i++) {
        Item *o = &r->items[i];
        if (matched[i]) { free(o->params); continue; }
        o->dying = 1;
        int at = i < n ? i : n; /* stays about where it was */
        memmove(&out[at + 1], &out[at], sizeof(Item) * (size_t)(n - at));
        out[at] = *o; n++;
    }
    free(matched); free(r->items); free(nw->items);
    nw->items = NULL; nw->n = 0;
    int oldsel = r->sel;
    r->items = out; r->n = n; r->sel = -1;
    for (int i = 0; i < n; i++) if (!out[i].dying && !strcmp(out[i].id, keep)) r->sel = i;
    if (r->sel < 0) { /* the focused item went away: the nearest live neighbour takes focus */
        for (int d = 0; d < n && r->sel < 0; d++) {
            if (oldsel + d < n && !out[oldsel + d].dying) r->sel = oldsel + d;
            else if (oldsel - d >= 0 && oldsel - d < n && !out[oldsel - d].dying) r->sel = oldsel - d;
        }
        if (r->sel < 0) r->sel = 0;
    }
}

static int row_live(const Row *r) { int c = 0; for (int i = 0; i < r->n; i++) c += !r->items[i].dying; return c; }

static const char *jstr(cJSON *o, const char *k) { cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsString(v) ? v->valuestring : ""; }

/* ---------- luna ---------- */

static LSHandle *ls;
typedef void (*json_cb)(cJSON *root, const char *raw);
typedef struct { json_cb cb; char uri[256]; char *payload; int sub, via_hb, backoff; LSMessageToken token; } Call;

static bool on_reply(LSHandle *h, LSMessage *msg, void *ctx);

/* A Developer Mode app is outside some ACG groups. On a rooted set Homebrew Channel's root
 * service runs luna-send for us. Its exec wraps the command in single quotes, so the command
 * may contain none: the payload goes in double quotes, escaped. */
static int call_via_hb(Call *c) {
    if (!c->payload || strchr(c->payload, '\'')) return 0;
    size_t n = strlen(c->uri) + strlen(c->payload) * 2 + 64;
    char *cmd = malloc(n), *o = cmd;
    o += sprintf(o, "luna-send -n 1 %s \"", c->uri);
    for (const char *p = c->payload; *p; p++) {
        if (*p == '"' || *p == '\\' || *p == '$' || *p == '`') *o++ = '\\';
        *o++ = *p;
    }
    *o++ = '"'; *o = 0;
    cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "command", cmd);
    char *ps = cJSON_PrintUnformatted(p); cJSON_Delete(p); free(cmd);
    c->via_hb = 1;
    LSError e; LSErrorInit(&e);
    int ok = LSCallOneReply(ls, "luna://org.webosbrew.hbchannel.service/exec", ps, on_reply, c, NULL, &e);
    if (!ok) { logf_("hb call: %s", e.message); LSErrorFree(&e); }
    free(ps);
    return ok;
}

static void call_free(Call *c) { free(c->payload); free(c); }
/* the service refused this caller: asking again, the same way, will not help */
static int refused(const char *raw) { return raw && (strstr(raw, "enied") || strstr(raw, "ermission")); }

static int sub_start(Call *c);
static gboolean sub_retry(gpointer p) {
    Call *c = p;
    logf_("resubscribe %s", c->uri);
    if (!sub_start(c)) g_timeout_add_seconds((guint)c->backoff, sub_retry, c);
    return G_SOURCE_REMOVE;
}

static bool on_reply(LSHandle *h, LSMessage *msg, void *ctx) {
    (void)h;
    Call *c = ctx;
    const char *raw = LSMessageGetPayload(msg);
    cJSON *root = cJSON_Parse(raw ? raw : "");
    cJSON *rv = cJSON_GetObjectItem(root, "returnValue");
    int ok = root && (!rv || cJSON_IsTrue(rv));
    if (ok) vlogf_("luna %s%s -> ok (%zu bytes)", c->via_hb ? "[hb] " : "", c->uri, raw ? strlen(raw) : 0);
    else logf_("luna %s%s -> FAIL", c->via_hb ? "[hb] " : "", c->uri);
    if (!ok) logf_("  %.300s", raw ? raw : "");
    if (c->sub && ok) c->backoff = 0;
    if (c->sub && !ok && !cJSON_IsTrue(cJSON_GetObjectItem(root, "subscribed"))) {
        /* the subscription ended (its service went down or restarted; Home outlives them all
         * in standby): subscribe again, waiting longer after each failure, up to a minute.
         * A refusal rarely changes, so that one is only tried again now and then. */
        LSCallCancel(ls, c->token, NULL);
        c->backoff = refused(raw) ? 300 : c->backoff ? (c->backoff * 2 > 60 ? 60 : c->backoff * 2) : 2;
        g_timeout_add_seconds((guint)c->backoff, sub_retry, c);
        cJSON_Delete(root);
        return true;
    }
    if (c->via_hb) {
        cJSON *inner = ok ? cJSON_Parse(jstr(root, "stdoutString")) : NULL;
        if (inner && c->cb) c->cb(inner, raw);
        cJSON_Delete(inner);
        call_free(c);
    } else if (!ok && !c->sub && !strstr(c->uri, "hbchannel") && refused(raw)) {
        if (!call_via_hb(c)) call_free(c);
    } else {
        if (c->cb) c->cb(root, raw);
        if (!c->sub) call_free(c);
    }
    cJSON_Delete(root);
    dirty = 1;
    return true;
}

static void call(const char *uri, const char *payload, json_cb cb) {
    if (!ls) return;
    Call *c = calloc(1, sizeof *c);
    c->cb = cb; scpy(c->uri, sizeof c->uri, uri); c->payload = strdup(payload);
    LSError e; LSErrorInit(&e);
    if (!LSCallOneReply(ls, uri, payload, on_reply, c, NULL, &e)) { logf_("call %s: %s", uri, e.message); LSErrorFree(&e); call_free(c); }
}

/* a subscription: cb runs on the first reply and on every change the service pushes */
static void subscribe(const char *uri, const char *payload, json_cb cb) {
    if (!ls) return;
    Call *c = calloc(1, sizeof *c);
    c->cb = cb; c->sub = 1; scpy(c->uri, sizeof c->uri, uri); c->payload = strdup(payload);
    if (!sub_start(c)) { c->backoff = 2; g_timeout_add_seconds(2, sub_retry, c); }
}
static int sub_start(Call *c) {
    LSError e; LSErrorInit(&e);
    if (LSCall(ls, c->uri, c->payload, on_reply, c, &c->token, &e)) return 1;
    logf_("subscribe %s: %s", c->uri, e.message); LSErrorFree(&e);
    return 0;
}

/* Started by SAM as com.webos.app.home (our binary bind-mounted over LG's /usr/bin/com.webos.app.home)
 * rather than as the Developer Mode app. */
static int as_home;
#define DEV_DIR "/media/developer/apps/usr/palm/applications/" APP_ID

static void luna_init(void) {
    LSError e; LSErrorInit(&e);
    /* Home's role grants its permissions to the name com.webos.app.home; an anonymous client of
     * that executable may only reach two video services. The dev app registers anonymously. */
    for (int tries = 0; !LSRegister(as_home ? "com.webos.app.home" : NULL, &ls, &e); tries++) {
        logf_("LSRegister: %s", e.message); LSErrorFree(&e); ls = NULL;
        if (!as_home) return;
        /* the name may still be held by the instance that just died. Without the bus Home is
         * an empty screen that looks alive: better a failed start, which the safety net counts */
        if (tries == 20) { logf_("no bus: giving up"); _exit(1); }
        SDL_Delay(500);
    }
    if (!LSGmainContextAttach(ls, g_main_context_default(), &e)) { logf_("LSGmainContextAttach: %s", e.message); LSErrorFree(&e); }
    logf_("luna registered");
}

/* ---------- icons ----------
 * The Developer Mode jail hides other apps' files (/media/cryptofs, /usr/palm, /mnt/otncabi).
 * Homebrew Channel's root service copies what we need, once, into our own directory; later
 * launches read the copies. (As the real Home this is not needed.) */

static char *copy_cmd;      /* pending "cp ...;" commands */
static int copy_inflight;   /* one copy round at a time; each source is tried once */

static void icon_cache_path(const char *src, char *out, size_t n) {
    snprintf(out, n, "%s/iconcache/%08x.png", app_dir, fnv(src));
}

static void queue_copy(const char *src, const char *dst) {
    if (strpbrk(src, "'\" ;$`\\")) return;
    char mark[600]; snprintf(mark, sizeof mark, "X:%s", src);
    if (tcache_get(mark)) return; /* already tried: a file that isn't there is not asked for again */
    Tex z = {0, 0, 0}; tcache_put(mark, z);
    char one[1400];
    snprintf(one, sizeof one, "cp -f %s %s;", src, dst);
    if (copy_cmd && strstr(copy_cmd, one)) return; /* (a full cache cannot keep the mark) */
    size_t a = copy_cmd ? strlen(copy_cmd) : 0;
    copy_cmd = realloc(copy_cmd, a + strlen(one) + 1);
    strcpy(copy_cmd + a, one);
}

/* ---------- decoder ----------
 * PNGs decode on a thread of their own (a recents snapshot or the wallpaper takes tens of ms,
 * which on the main thread was a dropped frame); the main thread only uploads the pixels, a few
 * per frame (decoder_poll), and puts them in the cache. A key being decoded is "pending": asked
 * for again meanwhile, it is not queued twice. */

typedef struct Job { struct Job *next; char key[640]; char path[PATH_MAX + 32]; int wallpaper, thumb; SDL_Surface *s, *blur; double ms; char err[160]; } Job;
static Job *todo_head, *todo_tail, *done_head;
static SDL_mutex *dec_lock;
static SDL_cond *dec_cond;
static SDL_Thread *decoder;
static char **pend; static int npend, cpend;
static Tex wallpaper, wallpaper_frost;
static char wallpaper_shipped[PATH_MAX + 32]; /* what a wallpaper that won't load falls back to */
/* what Recents draws a snapshot at, focused; LG's own files are several times that */
#define THUMB_W 400
#define THUMB_H 225

static void job_decode(Job *j) {
    double a = now_ms();
    j->s = decode_png(j->path);
    if (!j->s) scpy(j->err, sizeof j->err, SDL_GetError()); /* SDL's error is per thread */
    if (j->s && j->wallpaper) { j->s = fit(j->s, (int)W, (int)H); j->blur = frost(j->s); scrim_wallpaper(j->s); }
    if (j->s && j->thumb) j->s = fit(j->s, THUMB_W, THUMB_H);
    j->ms = now_ms() - a;
}

static int decoder_main(void *unused) {
    (void)unused;
    for (;;) {
        SDL_LockMutex(dec_lock);
        while (!todo_head) SDL_CondWait(dec_cond, dec_lock);
        Job *j = todo_head; todo_head = j->next; if (!todo_head) todo_tail = NULL;
        SDL_UnlockMutex(dec_lock);
        job_decode(j);
        SDL_LockMutex(dec_lock);
        j->next = done_head; done_head = j;
        SDL_UnlockMutex(dec_lock);
    }
    return 0;
}

static void decoder_start(void) {
    dec_lock = SDL_CreateMutex(); dec_cond = SDL_CreateCond();
    if (dec_lock && dec_cond) decoder = SDL_CreateThread(decoder_main, "png", NULL);
    if (!decoder) logf_("decoder thread: %s (decoding inline)", SDL_GetError());
}

static int pending(const char *key) { for (int i = 0; i < npend; i++) if (!strcmp(pend[i], key)) return 1; return 0; }

static void decoder_push(const char *key, const char *path, int is_wallpaper, int thumb) {
    Job *j = calloc(1, sizeof *j);
    scpy(j->key, sizeof j->key, key); scpy(j->path, sizeof j->path, path); j->wallpaper = is_wallpaper; j->thumb = thumb;
    if (!decoder) { /* no thread: decode here */
        job_decode(j);
        j->next = done_head; done_head = j;
    } else {
        SDL_LockMutex(dec_lock);
        if (todo_tail) todo_tail->next = j; else todo_head = j;
        todo_tail = j;
        SDL_CondSignal(dec_cond);
        SDL_UnlockMutex(dec_lock);
    }
    if (npend == cpend) { cpend = cpend ? cpend * 2 : 32; pend = realloc(pend, sizeof *pend * (size_t)cpend); }
    pend[npend++] = strdup(key);
}

/* upload up to max decoded images; returns how many landed */
static int decoder_poll(int max) {
    if (!done_head) return 0; /* a stale read at worst: the next frame looks again */
    SDL_LockMutex(dec_lock);
    Job *list = NULL;
    for (int k = 0; k < max && done_head; k++) { Job *j = done_head; done_head = j->next; j->next = list; list = j; }
    SDL_UnlockMutex(dec_lock);
    int n = 0;
    while (list) {
        Job *j = list; list = j->next;
        Tex t = upload(j->s);
        if (!j->s) logf_("img: %s: %s", j->path, j->err);
        else vlogf_("img: %s %dx%d %.1fms", j->path, t.w, t.h, j->ms);
        if (j->s) SDL_FreeSurface(j->s);
        if (j->wallpaper && t.id) {
            tex_free(&wallpaper); wallpaper = t;
            tex_free(&wallpaper_frost); wallpaper_frost = upload(j->blur);
            if (j->blur) SDL_FreeSurface(j->blur);
        } else if (j->wallpaper) { /* a broken file of someone's own must not leave Home black */
            if (j->blur) SDL_FreeSurface(j->blur);
            if (strcmp(j->path, wallpaper_shipped)) decoder_push("@wallpaper-shipped", wallpaper_shipped, 1, 0);
        }
        else tcache_put(j->key, t); /* an empty one too: a file that won't decode isn't tried again */
        for (int i = 0; i < npend; i++)
            if (!strcmp(pend[i], j->key)) { free(pend[i]); pend[i] = pend[--npend]; break; }
        free(j); n++;
    }
    return n;
}

/* versioned: the key carries the file's mtime, for pictures rewritten in place (recents
 * snapshots, an updated app's icon). The file itself when we can read it (as Home), else our
 * copy (the dev app). icon_waiting says the empty result is only "not decoded yet". */
static int icon_waiting;
static Tex icon_load_(const char *src, int versioned, int thumb) {
    Tex z = {0, 0, 0};
    icon_waiting = 0;
    if (!src || !*src || strncmp(src, "/", 1)) return z; /* local files only */
    char key[640];
    struct stat st;
    if (versioned && stat(src, &st) == 0) snprintf(key, sizeof key, "%s#%ld", src, (long)st.st_mtime);
    else scpy(key, sizeof key, src);
    const Tex *c = tcache_get(key);
    if (c) return *c;
    icon_waiting = 1;
    if (pending(key)) return z;
    char cp[PATH_MAX + 32]; icon_cache_path(src, cp, sizeof cp);
    if (access(src, R_OK) == 0) decoder_push(key, src, 0, thumb);
    else if (access(cp, R_OK) == 0) decoder_push(key, cp, 0, thumb);
    else { queue_copy(src, cp); icon_waiting = 0; }
    return z;
}
static Tex icon_load(const char *src) { return icon_load_(src, 0, 0); }
/* an item's picture: the one it shows stays until its replacement has landed */
static void load_picture(Tex *t, const char *src, int versioned, int thumb) {
    Tex n = icon_load_(src, versioned, thumb);
    if (n.id || !icon_waiting) *t = n;
}

static void on_copied(cJSON *root, const char *raw) {
    (void)raw;
    const char *err = jstr(root, "stderrString");
    logf_("icon copy done%s%.300s", *err ? ": " : "", err);
    copy_inflight = 0;
    for (int ri = 0; ri < NROWS; ri++)
        for (int i = 0; i < rows[ri].n; i++) rows[ri].items[i].icon_state = 0;
}

static void request_copies(void) {
    if (!copy_cmd || copy_inflight) return;
    copy_inflight = 1;
    size_t n = strlen(copy_cmd) + 2 * strlen(app_dir) + 64;
    char *cmd = malloc(n);
    snprintf(cmd, n, "mkdir -p %s/iconcache; %s chmod 644 %s/iconcache/*", app_dir, copy_cmd, app_dir);
    cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "command", cmd);
    char *ps = cJSON_PrintUnformatted(p); cJSON_Delete(p);
    logf_("icon copy: %zu bytes of commands", strlen(copy_cmd));
    call("luna://org.webosbrew.hbchannel.service/exec", ps, on_copied);
    free(ps); free(cmd); free(copy_cmd); copy_cmd = NULL;
}

/* ---------- data: apps ---------- */

#define HOME_IMG "/usr/palm/applications/com.webos.app.home/qml/UserInterfaceLayer/Components/images/2k/"

static char dash_title[160] = "";   /* the Home Dashboard's own title ("Painel Casa") */

static int own_index = -1;          /* our launch point's place in SAM's list (excluded from the row) */
static void build_dash(void);

/* LG's top-right buttons (SystemProperties.qml topButtonList), left to right as drawn */
static void build_top(void) {
    static const struct { const char *id, *app, *params; unsigned glyph; } b[] = {
        {"top:guide", "com.webos.app.livemenu", NULL, 0xF0164},
        {"top:search", "com.webos.app.voice", "{\"activateType\":\"search\",\"launchMode\":\"keyword\",\"params\":{},\"caller\":\"home\"}", 0xEFFF6},
        {"top:settings", "@sysui:com.webos.app.quicksettings", NULL, 0xF002B}, /* goToSettingsMain */
        {"top:notifications", "com.webos.app.notificationcenter", NULL, 0xF0105},
        {"top:account", "com.webos.app.membership",
         "{\"appReturn\":{\"appId\":\"com.webos.app.home\",\"params\":{\"target\":\"System\",\"item\":\"accountManagement\"}}}", 0xF0179},
    };
    Row nw = {0};
    for (unsigned i = 0; i < sizeof b / sizeof *b; i++) {
        Item *it = row_add(&nw);
        scpy(it->id, sizeof it->id, b[i].id);
        scpy(it->app, sizeof it->app, b[i].app);
        if (b[i].params) it->params = strdup(b[i].params);
        it->glyph = b[i].glyph;
    }
    row_merge(&rows[ROW_TOP], &nw);
}

static void on_launchpoints(cJSON *root, const char *raw);
static void on_launchpoints_changed(cJSON *root, const char *raw) {
    (void)raw;
    if (cJSON_IsArray(cJSON_GetObjectItem(root, "launchPoints"))) on_launchpoints(root, raw);
    else if (cJSON_GetObjectItem(root, "change"))
        call("luna://com.webos.applicationManager/listLaunchPoints", "{}", on_launchpoints);
}

/* Apps the user hid (edit mode, the eye over a tile): launch point ids, one a line. They stay
 * in SAM's list, so moving tiles keeps its positions; the row just collapses them. */
#define HIDDEN_PATH "/var/lib/homenative/hidden"
#define HIDDEN_MAX 128
static char *hidden_ids[HIDDEN_MAX];
static int nhidden;
static int is_hidden(const char *id) {
    for (int i = 0; i < nhidden; i++) if (!strcmp(hidden_ids[i], id)) return 1;
    return 0;
}
static void hidden_load(void) {
    FILE *f = fopen(HIDDEN_PATH, "r");
    if (!f) return;
    char line[256];
    while (nhidden < HIDDEN_MAX && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && !is_hidden(line)) hidden_ids[nhidden++] = strdup(line);
    }
    fclose(f);
    logf_("hidden: %d apps", nhidden);
}
/* write a copy, then rename it over, so a power cut leaves the old list or the new one */
static void hidden_save(void) {
    mkdir("/var/lib/homenative", 0755);
    FILE *f = fopen(HIDDEN_PATH ".tmp", "w");
    if (!f) { logf_("hidden: cannot write %s", HIDDEN_PATH); return; }
    for (int i = 0; i < nhidden; i++) fprintf(f, "%s\n", hidden_ids[i]);
    if (fclose(f) == 0) rename(HIDDEN_PATH ".tmp", HIDDEN_PATH);
}
static void hidden_set(const char *id, int on) {
    if (on == is_hidden(id)) return;
    if (on) {
        if (nhidden >= HIDDEN_MAX) { logf_("hidden: list full"); return; }
        hidden_ids[nhidden++] = strdup(id);
    } else for (int i = 0; i < nhidden; i++)
        if (!strcmp(hidden_ids[i], id)) { free(hidden_ids[i]); hidden_ids[i] = hidden_ids[--nhidden]; break; }
    hidden_save();
    logf_("%s %s", on ? "hide" : "unhide", id);
}
static int edit_mode_moving(void);
static void apps_fix_sel(void);
enum { ED_OFF, ED_BROWSE, ED_MOVE, ED_TRASH, ED_CONFIRM, ED_HIDE }; /* ED_HIDE: the eye over a tile, ours */
static int edit_mode;
static int lp_stale; /* a launch point update arrived while a tile was carried */
static void on_launchpoints(cJSON *root, const char *raw) {
    (void)raw;
    cJSON *lps = cJSON_GetObjectItem(root, "launchPoints");
    if (!cJSON_IsArray(lps)) return;
    /* merging now would reorder the row under the carried tile: read the list again on drop */
    if (edit_mode_moving()) { lp_stale = 1; return; }
    Row nw = {0}, *r = &nw;
    cJSON *lp;
    int idx = 0;
    own_index = -1;
    dash_title[0] = 0; /* set again below while the Home Dashboard app is there */
    cJSON_ArrayForEach(lp, lps) {
        const char *id = jstr(lp, "id");
        if (!strcmp(id, "com.webos.app.homeconnect")) {
            /* LG's card (TitleComponent.qml) shows a picture its server supplies; ours is a
             * sage gradient with LG's own house icon, the title and "powered by ThinQ" */
            scpy(dash_title, sizeof dash_title, jstr(lp, "title"));
        }
        if (!strcmp(id, APP_ID)) { own_index = idx++; continue; }
        idx++;
        Item *it = row_add(r);
        scpy(it->id, sizeof it->id, jstr(lp, "launchPointId"));
        if (!it->id[0]) scpy(it->id, sizeof it->id, id);
        scpy(it->lpid, sizeof it->lpid, jstr(lp, "launchPointId"));
        scpy(it->app, sizeof it->app, id);
        scpy(it->title, sizeof it->title, jstr(lp, "title"));
        /* the same order as LG's AppTile.getAppIconSource */
        static const char *const keys[] = {"extraLargeIcon", "mediumLargeIcon", "largeIcon", "icon"};
        for (unsigned k = 0; k < 4 && !it->icon[0]; k++) scpy(it->icon, sizeof it->icon, jstr(lp, keys[k]));
        it->color = hexcol(jstr(lp, "iconColor"), hexcol(jstr(lp, "bgColor"), rgba(0.1f, 0.1f, 0.12f, 1)));
        it->removable = cJSON_IsTrue(cJSON_GetObjectItem(lp, "removable"));
        cJSON *tv = cJSON_GetObjectItem(lp, "titleVisible");
        it->show_title = tv ? cJSON_IsTrue(tv) : cJSON_IsTrue(cJSON_GetObjectItem(lp, "systemApp"));
        cJSON *p = cJSON_GetObjectItem(lp, "params");
        if (cJSON_IsObject(p) && p->child) it->params = cJSON_PrintUnformatted(p);
        it->hidden = is_hidden(it->id);
        it->hf = it->hidden; /* a new hidden app starts collapsed (row_merge keeps a known one's) */
    }
    logf_("apps: %d launch points", nw.n);
    Row *ar = &rows[ROW_APPS];
    char was[200] = ""; if (ar->n) scpy(was, sizeof was, ar->items[ar->sel].id);
    row_merge(ar, &nw);
    apps_fix_sel();
    /* the trash, the eye and the dialog are about the focused app: if that changed under them, they no longer are */
    if (edit_mode >= ED_TRASH && (!ar->n || strcmp(was, ar->items[ar->sel].id))) edit_mode = ED_BROWSE;
    build_dash();
}

/* ---------- data: inputs ---------- */

typedef struct { char app[160], name[160], label[80], icon[512]; } Input;
static Input inputs[16];
static int ninputs;
static char channel_mode[40] = "";
static int airplay_on;

static void build_pills(void);
static void on_inputs(cJSON *root, const char *raw) {
    (void)raw;
    cJSON *devs = cJSON_GetObjectItem(root, "devices");
    if (!cJSON_IsArray(devs)) return;
    ninputs = 0;
    cJSON *d;
    cJSON_ArrayForEach(d, devs) {
        /* LG's rule (ConnectionsInterfaces.qml): only inputs with something plugged in */
        cJSON *plug = cJSON_GetObjectItem(d, "hdmiPlugIn");
        if (!cJSON_IsTrue(cJSON_GetObjectItem(d, "connected")) || (plug && !cJSON_IsTrue(plug)) ||
            cJSON_IsTrue(cJSON_GetObjectItem(d, "dongleConnected")) || ninputs >= 16)
            continue;
        Input *in = &inputs[ninputs++];
        scpy(in->app, sizeof in->app, jstr(d, "appId"));
        /* the device's own HDMI-CEC name when it gives one (LG's getOSDName); else, unless the
         * input was renamed, the name LG's device detection found ("PS5"): the label it builds
         * from that adds the type in the UI language ("PS5 Game Console"); else the label */
        const char *name = "", *found = "";
        cJSON *sub;
        cJSON_ArrayForEach(sub, cJSON_GetObjectItem(d, "subList")) {
            if (!*name) name = jstr(sub, "osdName");
            if (!*found) found = jstr(sub, "labelName");
        }
        if (!*name && !cJSON_IsTrue(cJSON_GetObjectItem(d, "modified"))) name = found;
        if (!*name) name = jstr(d, "label");
        scpy(in->name, sizeof in->name, *name ? name : jstr(d, "id"));
        /* "HDMI_1" -> "HDMI 1", the port under the name on LG's cards */
        scpy(in->label, sizeof in->label, jstr(d, "id"));
        for (char *p = in->label; *p; p++) if (*p == '_') *p = ' ';
        snprintf(in->icon, sizeof in->icon, "%s%s", jstr(d, "iconPrefix"), jstr(d, "icon"));
    }
    logf_("inputs: %d", ninputs);
    build_pills();
    build_dash();
}

static void build_pills(void) {
    Row nw = {0};
    Item *tv = row_add(&nw);
    scpy(tv->id, sizeof tv->id, "com.webos.app.livetv");
    scpy(tv->app, sizeof tv->app, "com.webos.app.livetv");
    scpy(tv->title, sizeof tv->title, S(S_LIVETV));
    scpy(tv->icon, sizeof tv->icon, "/usr/palm/plugins/inputapps/assets/small/antenna.png");
    for (int i = 0; i < ninputs; i++) {
        Item *it = row_add(&nw);
        scpy(it->id, sizeof it->id, inputs[i].app);
        scpy(it->app, sizeof it->app, inputs[i].app);
        scpy(it->title, sizeof it->title, inputs[i].name);
        scpy(it->icon, sizeof it->icon, inputs[i].icon);
    }
    row_merge(&rows[ROW_INPUTS], &nw);
}

static void on_channel_mode(cJSON *root, const char *raw) {
    (void)raw;
    if (!cJSON_IsTrue(cJSON_GetObjectItem(root, "returnValue"))) return;
    const char *m = jstr(cJSON_GetObjectItem(root, "channelMode"), "channelModeName");
    scpy(channel_mode, sizeof channel_mode, m);
    build_dash();
}

static void on_airplay(cJSON *root, const char *raw) {
    (void)raw;
    if (!cJSON_IsTrue(cJSON_GetObjectItem(root, "returnValue"))) return;
    airplay_on = cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "settings"), "supportAirplay"));
    build_dash();
}

/* Home Dashboard | Mobile: Connection Guide, AirPlay | TV / Inputs: Live TV, each input */
static void build_dash(void) {
    Row nw = {0};
    Item *it;
    if (dash_title[0]) {
        it = row_add(&nw);
        scpy(it->id, sizeof it->id, "dash:homeconnect");
        scpy(it->app, sizeof it->app, "com.webos.app.homeconnect");
        scpy(it->group, sizeof it->group, dash_title);
        scpy(it->title, sizeof it->title, dash_title);
        scpy(it->image, sizeof it->image, "-"); /* marks the card kind; no picture */
        scpy(it->icon, sizeof it->icon, HOME_IMG "app_title_icon_homedashboard.png");
        scpy(it->sub, sizeof it->sub, HOME_IMG "logo_thinq.png"); /* (an image path, see draw_card) */
    }
    it = row_add(&nw);
    scpy(it->id, sizeof it->id, "dash:guide");
    scpy(it->app, sizeof it->app, "com.webos.app.tvuserguide");
    it->params = strdup("{\"target\":\"share\",\"target2\":\"mobileconnection\"}");
    scpy(it->group, sizeof it->group, S(S_MOBILE));
    scpy(it->title, sizeof it->title, S(S_GUIDE));
    scpy(it->icon, sizeof it->icon, HOME_IMG "mobileguide.png");
    if (airplay_on) {
        it = row_add(&nw);
        scpy(it->id, sizeof it->id, "dash:airplay");
        scpy(it->app, sizeof it->app, "@airplay");
        scpy(it->group, sizeof it->group, S(S_MOBILE));
        scpy(it->title, sizeof it->title, "AirPlay");
        scpy(it->icon, sizeof it->icon, HOME_IMG "airplay.png");
    }
    it = row_add(&nw);
    scpy(it->id, sizeof it->id, "dash:livetv");
    scpy(it->app, sizeof it->app, "com.webos.app.livetv");
    scpy(it->group, sizeof it->group, S(S_TVIN));
    scpy(it->title, sizeof it->title, S(S_LIVETV));
    const char *mode = !strcmp(channel_mode, "Cable") ? S(S_CABLE) : !strcmp(channel_mode, "Satellite") ? S(S_SATELLITE)
                     : !strncmp(channel_mode, "Terrestrial", 11) ? S(S_ANTENNA) : channel_mode;
    scpy(it->sub, sizeof it->sub, mode);
    scpy(it->icon, sizeof it->icon, "/usr/palm/plugins/inputapps/assets/small/antenna.png");
    for (int i = 0; i < ninputs; i++) {
        it = row_add(&nw);
        snprintf(it->id, sizeof it->id, "dash:%s", inputs[i].app);
        scpy(it->app, sizeof it->app, inputs[i].app);
        scpy(it->group, sizeof it->group, S(S_TVIN));
        scpy(it->title, sizeof it->title, inputs[i].name);
        scpy(it->sub, sizeof it->sub, inputs[i].label);
        scpy(it->icon, sizeof it->icon, inputs[i].icon);
    }
    row_merge(&rows[ROW_DASH], &nw);
}

/* ---------- data: channels ----------
 * LG's "Frequently Watched Channels" (LiveTVInterfaces.qml): usercontextmanager's ranking of the
 * last 90 days, names and numbers from iepg's channel list, the programme on now from
 * iepg/getCurrentEventList (its localStartTime/localEndTime give the progress bar). */

typedef struct { char id[64], name[120], number[16]; char prog[200]; time_t start, end; } Chan;
static Chan chans[9];
static int nchans;
static char last_channel[64];
/* iepg's channel list, reduced to what the cards show (the reply is ~67 KB of JSON) */
typedef struct { char id[64], name[120], number[16]; } ChanName;
static ChanName *chnames;
static int nchnames = -1;    /* -1: not received yet */
static int ranked;           /* ranking received */
static void build_channels(void);

static int rank_failed; /* usercontextmanager refuses until its memory is loaded, a minute or two after boot */
static void on_rank(cJSON *root, const char *raw) {
    (void)raw;
    rank_failed = !cJSON_IsTrue(cJSON_GetObjectItem(root, "returnValue"));
    if (rank_failed) return; /* keep what we had; the minute tick asks again */
    /* what is known about a channel stays until the new programme list arrives: cards don't
     * blink to "No information" each time Home comes back */
    static Chan old[9];
    int nold = nchans;
    memcpy(old, chans, sizeof(Chan) * (size_t)nold);
    nchans = 0;
    cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(root, "channelList")) {
        if (nchans >= 9) break;
        Chan *n = &chans[nchans++];
        memset(n, 0, sizeof *n);
        scpy(n->id, sizeof n->id, jstr(c, "channelId"));
        for (int i = 0; i < nold; i++) if (!strcmp(old[i].id, n->id)) *n = old[i];
    }
    ranked = 1;
    logf_("channels: %d ranked", nchans);
    build_channels();
}

static void on_chlist(cJSON *root, const char *raw) {
    (void)raw;
    /* compressed: "names" maps a field to its column in each of "values" */
    cJSON *names = cJSON_GetObjectItem(root, "names"), *vals = cJSON_GetObjectItem(root, "values");
    cJSON *ci = cJSON_GetObjectItem(names, "channelId"), *cn = cJSON_GetObjectItem(names, "channelName"),
          *cu = cJSON_GetObjectItem(names, "channelNumber");
    if (!cJSON_IsNumber(ci) || !cJSON_IsNumber(cn) || !cJSON_IsNumber(cu) || !cJSON_IsArray(vals)) return;
    free(chnames);
    chnames = calloc((size_t)cJSON_GetArraySize(vals) + 1, sizeof *chnames);
    nchnames = 0;
    cJSON *v;
    cJSON_ArrayForEach(v, vals) {
        cJSON *id = cJSON_GetArrayItem(v, ci->valueint), *nm = cJSON_GetArrayItem(v, cn->valueint), *nu = cJSON_GetArrayItem(v, cu->valueint);
        if (!cJSON_IsString(id)) continue;
        ChanName *c = &chnames[nchnames++];
        scpy(c->id, sizeof c->id, id->valuestring);
        scpy(c->name, sizeof c->name, cJSON_IsString(nm) ? nm->valuestring : "");
        scpy(c->number, sizeof c->number, cJSON_IsString(nu) ? nu->valuestring : "");
    }
    build_channels();
}

static time_t parse_epg_time(const char *s) { /* "2026,10,20,12,20,00", the TV's wall clock */
    struct tm tm = {0};
    if (sscanf(s, "%d,%d,%d,%d,%d,%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
    tm.tm_year -= 1900; tm.tm_mon -= 1;
    return timegm(&tm);
}

static void on_events(cJSON *root, const char *raw) {
    (void)raw;
    cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItem(root, "programList")) {
        const char *cid = jstr(p, "channelId"), *sig = jstr(p, "signalChannelId");
        for (int i = 0; i < nchans; i++) {
            size_t li = strlen(chans[i].id), ls_ = strlen(sig);
            int match = *cid ? !strcmp(cid, chans[i].id)
                             : (ls_ && li > ls_ && !strcmp(chans[i].id + li - ls_, sig) && chans[i].id[li - ls_ - 1] == '_');
            if (!match) continue;
            scpy(chans[i].prog, sizeof chans[0].prog, jstr(p, "programName"));
            chans[i].start = parse_epg_time(jstr(p, "localStartTime"));
            chans[i].end = parse_epg_time(jstr(p, "localEndTime"));
        }
    }
    build_channels();
}

static void on_last_channel(cJSON *root, const char *raw) {
    (void)raw;
    scpy(last_channel, sizeof last_channel, jstr(root, "channelId"));
    build_channels();
}

static void request_events(void) {
    if (!nchans) return;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "dataType", 0); cJSON_AddNumberToObject(p, "sort", 0);
    cJSON *ids = cJSON_AddArrayToObject(p, "channelId");
    for (int i = 0; i < nchans; i++) cJSON_AddItemToArray(ids, cJSON_CreateString(chans[i].id));
    char *s = cJSON_PrintUnformatted(p); cJSON_Delete(p);
    call("luna://com.webos.service.iepg/getCurrentEventList", s, on_events);
    free(s);
}

static void request_rank(void) {
    time_t from = time(NULL) - 90 * 86400;
    struct tm tm; gmtime_r(&from, &tm);
    char pl[200];
    snprintf(pl, sizeof pl, "{\"eventType\":\"channel\",\"fromDate\":%04d%02d%02d,\"orderBy\":\"duration\"}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    call("luna://com.webos.service.usercontextmanager/getRank", pl, on_rank);
}

static void request_channels(void) {
    request_rank();
    call("luna://com.webos.service.iepg/getChannelList",
         "{\"dataType\":0,\"sort\":0,\"attribute\":[{\"filterType\":\"skipped\",\"value\":false},{\"filterType\":\"Invisible\",\"value\":false}],"
         "\"channelGroup\":[\"All\"],\"channelMode\":[\"Tuner\",\"IP\"],\"requestField\":[\"channelId\",\"channelName\",\"channelNumber\"],\"format\":1}",
         on_chlist);
}

static int events_requested;
static void build_channels(void) {
    if (!ranked || nchnames < 0) return;
    int kept = 0;
    for (int i = 0; i < nchans; i++) {
        Chan *c = &chans[i];
        for (int k = 0; !c->name[0] && k < nchnames; k++)
            if (!strcmp(chnames[k].id, c->id)) { scpy(c->name, sizeof c->name, chnames[k].name); scpy(c->number, sizeof c->number, chnames[k].number); }
        if (c->name[0]) chans[kept++] = *c; /* a ranked channel no longer in the list is dropped */
    }
    nchans = kept;
    if (!events_requested && nchans) { events_requested = 1; request_events(); }

    Row nw = {0};
    if (!nchnames) { row_merge(&rows[ROW_CHANNELS], &nw); return; } /* no tuner set up: no row */
    Item *it = row_add(&nw);
    scpy(it->id, sizeof it->id, "ch:list");
    scpy(it->app, sizeof it->app, "com.webos.app.livemenu");
    it->params = strdup("{\"activateType\":\"channels\"}");
    scpy(it->title, sizeof it->title, S(S_CHLIST));
    scpy(it->sub2, sizeof it->sub2, S(S_CHLIST_SUB));
    scpy(it->icon, sizeof it->icon, HOME_IMG "ic_livetv_channels.png");
    time_t now = time(NULL) + tz_offset;
    for (int i = 0; i < nchans; i++) {
        Chan *c = &chans[i];
        it = row_add(&nw);
        snprintf(it->id, sizeof it->id, "ch:%s", c->id);
        scpy(it->app, sizeof it->app, "com.webos.app.livetv");
        char pj[160]; snprintf(pj, sizeof pj, "{\"id\":\"channelChange\",\"channelId\":\"%s\",\"sender\":\"home\"}", c->id);
        it->params = strdup(pj);
        scpy(it->title, sizeof it->title, c->name);
        scpy(it->sub, sizeof it->sub, c->number);
        scpy(it->sub2, sizeof it->sub2, c->prog[0] ? c->prog : S(S_NOINFO));
        it->highlight = !strcmp(c->id, last_channel);
        if (c->end > c->start && now >= c->start) {
            float p = (float)(now - c->start) / (float)(c->end - c->start);
            it->progress = p > 1 ? 1 : p;
        }
    }
    row_merge(&rows[ROW_CHANNELS], &nw);
    /* progress moves on, and texts may change: let the merged items take the new values */
    Row *r = &rows[ROW_CHANNELS];
    for (int i = 0; i < r->n; i++) r->items[i].icon_state = 0;
}

/* ---------- misc data ---------- */

static void on_time(cJSON *root, const char *raw) {
    (void)raw;
    cJSON *lt = cJSON_GetObjectItem(root, "localtime");
    cJSON *utc = cJSON_GetObjectItem(root, "utc");
    if (!cJSON_IsObject(lt) || !cJSON_IsNumber(utc)) return;
    static const char *const f[6] = {"year", "month", "day", "hour", "minute", "second"};
    int v[6];
    for (int i = 0; i < 6; i++) {
        cJSON *n = cJSON_GetObjectItem(lt, f[i]);
        if (!cJSON_IsNumber(n)) { logf_("time: no %s in %.200s", f[i], raw ? raw : ""); return; }
        v[i] = n->valueint;
    }
    struct tm tm = {0};
    tm.tm_year = v[0] - 1900; tm.tm_mon = v[1] - 1; tm.tm_mday = v[2];
    tm.tm_hour = v[3]; tm.tm_min = v[4]; tm.tm_sec = v[5];
    /* round to 15 min, west of Greenwich too */
    long off = lround((double)(timegm(&tm) - (time_t)utc->valuedouble) / 900.0) * 900;
    clock_str[0] = 0; /* force redraw: the time itself may have been set */
    if (off != tz_offset) logf_("time: offset %lds", off);
    tz_offset = off;
}

static void on_locale(cJSON *root, const char *raw) {
    (void)raw;
    cJSON *li = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "settings"), "localeInfo");
    const char *ui = jstr(cJSON_GetObjectItem(li, "locales"), "UI");
    if (!*ui) return;
    char lang[8]; scpy(lang, sizeof lang, ui);
    char *dash = strchr(lang, '-'); if (dash) *dash = 0;
    /* the setting when there is one, else what the region's clocks say */
    const char *fmt = jstr(cJSON_GetObjectItem(li, "locales"), "FMT");
    const char *ck = jstr(li, "clock"), *region = strrchr(*fmt ? fmt : ui, '-');
    static const char regions12[] = " US CA AU NZ PH IN KR MY SG SA EG PK BD CO ";
    char want[6] = ""; if (region) snprintf(want, sizeof want, " %.2s ", region + 1);
    int h12 = !strcmp(ck, "12") || (strcmp(ck, "24") && *want && strstr(regions12, want));
    logf_("locale: UI %s -> %s, %d hour clock", ui, lang, h12 ? 12 : 24);
    if (h12 != hour12) { hour12 = h12; clock_str[0] = 0; }
    if (!strcmp(lang, ui_lang)) return;
    scpy(ui_lang, sizeof ui_lang, lang);
    clock_str[0] = 0;
    build_pills(); build_dash(); build_channels();
}

static void on_launched(cJSON *root, const char *raw) { (void)root; vlogf_("reply: %.200s", raw ? raw : ""); }

/* an app that would not start says so, in the system's own toast */
static char launching[200];
static void on_app_launched(cJSON *root, const char *raw) {
    if (!root || cJSON_IsTrue(cJSON_GetObjectItem(root, "returnValue")) || (raw && strstr(raw, "already"))) { vlogf_("reply: %.200s", raw ? raw : ""); return; }
    char m[300]; snprintf(m, sizeof m, S(S_CANT_OPEN), launching);
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "sourceId", "com.webos.app.home"); cJSON_AddStringToObject(p, "message", m);
    char *ps = cJSON_PrintUnformatted(p); cJSON_Delete(p);
    call("luna://com.webos.notification/createToast", ps, on_launched);
    free(ps);
}

static double last_launch_ms = -1e9;
static void launch(Item *it) {
    /* one press can arrive as both an OK key and a click; act on the first only */
    if (now_ms() - last_launch_ms < 600) return;
    last_launch_ms = now_ms();
    it->press = 1;
    if (!strcmp(it->app, "@airplay")) { /* LG opens AirPlay through its service, not an app */
        call("luna://airplay.service/showAirplayPage", "{}", on_launched);
        return;
    }
    if (!strncmp(it->app, "@sysui:", 7)) { /* system UI (the settings panel): LG asks systemUIManager */
        char p[200]; snprintf(p, sizeof p, "{\"appId\":\"%s\"}", it->app + 7);
        logf_("systemUI launch %s", p);
        call("luna://com.webos.service.systemUIManager/launch", p, on_launched);
        return;
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "id", it->app);
    if (it->params) cJSON_AddItemToObject(p, "params", cJSON_Parse(it->params));
    char *s = cJSON_PrintUnformatted(p); cJSON_Delete(p);
    logf_("launch %s", s);
    scpy(launching, sizeof launching, it->title);
    call("luna://com.webos.applicationManager/launch", s, on_app_launched);
    free(s);
}

/* apps, inputs, locale are subscriptions (an input plugged in or renamed over CEC, an app
 * installed, removed or moved shows up at once, as on LG's home) */
/* Native lifecycle v2 (LG's Home declares it): SAM wants the process to subscribe to
 * applicationManager/registerApp; later launches of the app (every Home key press) then arrive
 * here as "relaunch" events instead of SAM killing and restarting an unregistered process. */
static int relaunch_pending;
static void on_lifecycle(cJSON *root, const char *raw) {
    const char *ev = jstr(root, "event");
    logf_("lifecycle: %.200s", raw ? raw : "");
    if (!strcmp(ev, "relaunch")) relaunch_pending = 1;
}

static void on_recents(cJSON *root, const char *raw);
static void on_apps_info(cJSON *root, const char *raw);
static void start_subscriptions(void) {
    if (as_home) subscribe("luna://com.webos.applicationManager/registerApp", "{}", on_lifecycle);
    subscribe("luna://com.webos.applicationManager/listLaunchPoints", "{\"subscribe\":true}", on_launchpoints_changed);
    subscribe("luna://com.webos.service.eim/getAllInputStatus", "{\"subscribe\":true}", on_inputs);
    subscribe("luna://com.webos.settingsservice/getSystemSettings", "{\"keys\":[\"localeInfo\"],\"subscribe\":true}", on_locale);
    subscribe("luna://com.webos.service.recent/listRecents", "{\"subscribe\":true}", on_recents);
    subscribe("luna://com.webos.applicationManager/listApps",
              "{\"subscribe\":true,\"properties\":[\"id\",\"folderPath\",\"iconColor\",\"icon\",\"largeIcon\",\"mediumLargeIcon\"]}", on_apps_info);
    /* pushes time zone and daylight-saving changes (refresh() also asks, in case it doesn't) */
    subscribe("luna://com.palm.systemservice/time/getSystemTime", "{\"subscribe\":true}", on_time);
    subscribe("luna://com.webos.service.utp.channel/getLastChannelId", "{\"sourceType\":\"INPUTSOURCE_NONE\",\"subscribe\":true}", on_last_channel);
}
/* Coming back to the front re-reads what changes while away (clock, the channel ranking and the
 * programmes on now). The big, slow-changing list (iepg's channel list, ~67 KB) is parsed on
 * this thread, so it is read at most every 30 min, or until one read has worked: services
 * can still be starting when Home does. */
static double last_full_refresh = -1e12;
static void refresh(void) {
    call("luna://com.palm.systemservice/time/getSystemTime", "{}", on_time);
    events_requested = 0;
    if (now_ms() - last_full_refresh > 30 * 60 * 1000.0 || nchnames < 0 || !channel_mode[0]) {
        last_full_refresh = now_ms();
        call("luna://com.webos.service.utp.channel/getChannelMode", "{}", on_channel_mode);
        call("luna://com.webos.settingsservice/getSystemSettings", "{\"key\":\"supportAirplay\",\"category\":\"other\"}", on_airplay);
        request_channels();
    } else request_rank();
}

/* LG's edit mode (MainRibbon.qml), entered by a long press on an app: browse the tiles, pick one
 * up and carry it with the arrows, or go Up to the trash over it and confirm */
/* an item focus passes over: gone, or hidden while not editing */
static int skipped(const Item *it) { return it->dying || (it->hidden && !edit_mode); }
/* focus must not rest on a hidden app outside edit mode: the nearest shown one takes it */
static void apps_fix_sel(void) {
    Row *r = &rows[ROW_APPS];
    if (!r->n || !skipped(&r->items[r->sel])) return;
    for (int d = 1; d < r->n; d++) {
        if (r->sel + d < r->n && !skipped(&r->items[r->sel + d])) { r->sel += d; return; }
        if (r->sel - d >= 0 && !skipped(&r->items[r->sel - d])) { r->sel -= d; return; }
    }
}
static int confirm_delete;   /* the confirm dialog's focused button: 1 Delete, 0 Cancel */
static float trash_f, confirm_t, hide_f; /* animated: trash focus, dialog presence, eye focus */
/* where the last frame put the eye and trash over the focused tile, and the dialog's buttons
 * (Delete, Cancel), for the pointer; [0] < 0 when not drawn */
static float ed_eye_r[4] = {-1}, ed_trash_r[4] = {-1}, ed_conf_r[2][4] = {{-1}, {-1}};
static void set_rect(float *r, float x, float y, float w, float h) { r[0] = x; r[1] = y; r[2] = w; r[3] = h; }
static int in_rect(const float *r, float x, float y) { return r[0] >= 0 && x >= r[0] && x <= r[0] + r[2] && y >= r[1] && y <= r[1] + r[3]; }

/* ---------- recents ----------
 * LG's Recents (HomeRecentInterfaces.qml / Recents.qml): Back on the home screen opens a panel
 * over the lower screen with the apps used lately, from com.webos.service.recent. The trash over
 * a card is removeRecent, which also closes that app; "Close All" is removeAllRecents. */

/* every installed app's icon and colour (applicationManager/listApps), for cards whose recent
 * entry has only a splash: LG shows the icon on the app's colour there (getRecentAppInfo) */
typedef struct { char id[160], icon[512]; Col color; } AppInfo;
static AppInfo *apps_info;
static int napps_info;

static void on_recents(cJSON *root, const char *raw);
static cJSON *last_recents;
static void on_apps_info(cJSON *root, const char *raw) {
    (void)raw;
    cJSON *list = cJSON_GetObjectItem(root, "apps");
    if (!cJSON_IsArray(list)) return;
    free(apps_info);
    napps_info = cJSON_GetArraySize(list);
    apps_info = calloc((size_t)napps_info + 1, sizeof *apps_info);
    int n = 0;
    cJSON *a;
    cJSON_ArrayForEach(a, list) {
        AppInfo *ai = &apps_info[n++];
        scpy(ai->id, sizeof ai->id, jstr(a, "id"));
        const char *ic = jstr(a, "mediumLargeIcon");
        if (!*ic) ic = jstr(a, "largeIcon");
        if (!*ic) ic = jstr(a, "icon");
        if (*ic == '/') scpy(ai->icon, sizeof ai->icon, ic);
        else snprintf(ai->icon, sizeof ai->icon, "%s/%s", jstr(a, "folderPath"), ic);
        ai->color = hexcol(jstr(a, "iconColor"), rgba(1, 1, 1, 1));
    }
    napps_info = n;
    if (last_recents) on_recents(last_recents, NULL);
}

static int recents_open;
static int closing_all;          /* Close All sent: the panel waits for the list to empty */
static double closing_since;
static double recents_refetch_at; /* a pending re-read of the list, coalesced (0 = none) */
static void recents_show(int on);
static int rec_part;          /* focus inside the panel: 0 the cards, 1 the trash, 2 Close All */
static float recents_t, rec_trash_f, rec_close_f;
/* where draw_recents last put things, for the pointer: the panel top, Close All, the trash */
static float rec_py = H, rec_close_r[4] = {-1}, rec_trash_r[4] = {-1};

static void on_recents(cJSON *root, const char *raw) {
    (void)raw;
    cJSON *list = cJSON_GetObjectItem(root, "recentsAppList");
    if (!cJSON_IsArray(list)) {
        /* after the first reply the service pushes one change at a time ({"eventType":"added",
         * "appId":…}): read the whole list again, once for a burst (closing four apps sends four) */
        if (cJSON_GetObjectItem(root, "eventType") && !recents_refetch_at) recents_refetch_at = now_ms() + 250;
        return;
    }
    if (root != last_recents) { cJSON_Delete(last_recents); last_recents = cJSON_Duplicate(root, 1); }
    Row nw = {0};
    cJSON *e;
    cJSON_ArrayForEach(e, list) {
        const char *app = jstr(e, "appId");
        if (!*app || !strcmp(app, APP_ID) || !strcmp(app, "com.webos.app.home")) continue; /* no Home in Recents */
        Item *it = row_add(&nw);
        scpy(it->id, sizeof it->id, app);
        scpy(it->app, sizeof it->app, app);
        scpy(it->title, sizeof it->title, jstr(e, "title"));
        /* LG's getRecentImage: the custom image, else the card snapshot; where there is only
         * the splash, the app's icon on its colour */
        const char *img = jstr(e, "customImageFilePath");
        if (!*img) img = jstr(e, "cardSnapShotFilePath");
        scpy(it->image, sizeof it->image, img);
        it->color = rgba(0.16f, 0.16f, 0.17f, 1);
        if (!*img) {
            for (int i = 0; i < napps_info; i++)
                if (!strcmp(apps_info[i].id, app)) { scpy(it->icon, sizeof it->icon, apps_info[i].icon); it->color = apps_info[i].color; }
            if (!it->icon[0]) scpy(it->image, sizeof it->image, jstr(e, "splashBackground"));
        }
    }
    vlogf_("recents: %d", nw.n);
    if (closing_all) {
        int left = nw.n;
        for (int i = 0; i < nw.n; i++) free(nw.items[i].params);
        free(nw.items);
        if (!left) { logf_("close all: done in %.0f ms", now_ms() - closing_since); closing_all = 0; recents_show(0); }
        return;
    }
    row_merge(&rows[ROW_RECENTS], &nw);
}

static void recents_show(int on) {
    recents_open = on;
    rec_part = 0;
    if (on) rows[ROW_RECENTS].sel = 0;
}

static void recents_key(int dx, int dy, int ok, int back) {
    Row *r = &rows[ROW_RECENTS];
    int live = row_live(r);
    if (closing_all) return; /* closing everything: wait for it */
    if (back) { recents_show(0); return; }
    if (rec_part == 2) { /* Close All */
        if (dy > 0 && live) rec_part = 0;
        else if (ok && live) {
            /* the cards shrink away together; the panel stays until the apps are really
             * closed, so its own animation doesn't run while the TV is busy closing them */
            for (int i = 0; i < r->n; i++) r->items[i].dying = 1;
            closing_all = 1; closing_since = now_ms();
            call("luna://com.webos.service.recent/removeAllRecents", "{}", on_launched);
        } else if (ok) recents_show(0);
        return;
    }
    if (!live) { if (ok) recents_show(0); return; }
    if (dx) {
        int j = r->sel + dx;
        while (j >= 0 && j < r->n && r->items[j].dying) j += dx;
        if (j >= 0 && j < r->n) r->sel = j;
        return;
    }
    if (dy < 0) { rec_part = rec_part == 0 ? 1 : 2; return; }
    if (dy > 0) { rec_part = 0; return; }
    if (ok) {
        Item *it = &r->items[r->sel];
        if (it->dying) return; /* already closing: neither close it again nor launch it */
        if (rec_part == 1) {
            char p[220]; snprintf(p, sizeof p, "{\"appId\":\"%s\"}", it->app);
            logf_("removeRecent %s", p);
            call("luna://com.webos.service.recent/removeRecent", p, on_launched);
            rec_part = 0; /* the card shrinks away when the service pushes the new list */
        } else { recents_show(0); launch(it); }
    }
}

/* ---------- layout & animation ---------- */

/* the focus zoom: LG's timing (RelatedList.qml: 400 ms on Easing.OutBack, back in 100 ms) with a
 * gentler overshoot than LG's 4, the same for cards and app tiles. zoom is that curve's 0..1
 * (past 1 while it overshoots). */
#define FOCUS_OVERSHOOT 1.5f
static float out_back(float t, float o) { t -= 1; return 1 + (o + 1) * t * t * t + o * t * t; }
static double marquee_until; /* a title is scrolling: keep drawing frames */
static int zoom_step(Item *it, int on, float dt, float overshoot) {
    if (on) {
        if (it->zin >= 1) return 0;
        if (it->zin == 0) { it->z0 = it->zoom; it->fstart = now_ms(); }
        it->zin = fminf(1, it->zin + dt / 0.4f);
        it->zoom = it->z0 + (1 - it->z0) * out_back(it->zin, overshoot);
    } else {
        it->zin = 0;
        if (it->zoom == 0) return 0;
        it->zoom = fmaxf(0, it->zoom - dt / 0.1f);
    }
    return 1;
}
static float approach(float v, float target, float rate, float dt) {
    float k = 1.f - expf(-rate * dt);
    float n = v + (target - v) * k;
    return fabsf(n - target) < 0.0005f ? target : n;
}

/* Recents cards come and go in two steps: a card zooms out to its own centre and fades while
 * its slot stays, then the slot closes and the neighbours slide over (a new card: the reverse).
 * life runs 1 -> 0 on the way out; the first part of it is the slot, the rest the card itself. */
#define SLOT_PART 0.45f
static float slot_of(const Item *it) { return fminf(1.f, it->life / SLOT_PART); }
static float vis_of(const Item *it) { return fmaxf(0.f, (it->life - SLOT_PART) / (1.f - SLOT_PART)); }

/* every item's target x, once per frame (a running sum over the row) */
static void layout_x(int ri) {
    Row *r = &rows[ri];
    float x = r->x0;
    for (int k = 0; k < r->n; k++) {
        r->items[k].tx = x;
        if (k + 1 == r->n) break;
        float g = r->gap;
        /* a tile being carried: its neighbours make room for LG's arrows */
        if (ri == ROW_APPS && edit_mode == ED_MOVE && (k == r->sel - 1 || k == r->sel)) g += 44;
        x += (item_w(r, &r->items[k]) + g) * (ri == ROW_RECENTS ? slot_of(&r->items[k]) : r->items[k].life * (1 - r->items[k].hf));
    }
}
static float item_x(int ri, int i) { return rows[ri].items[i].tx; }

static float row_target_scroll(int ri) {
    Row *r = &rows[ri];
    if (!r->n || ri == ROW_TOP) return 0; /* the top buttons are pinned to the right edge */
    float margin = ri == ROW_APPS || ri == ROW_INPUTS ? TILE * 1.2f : 60.f;
    float x = item_x(ri, r->sel), w = item_w(r, &r->items[r->sel]);
    float t = r->scroll;
    if (x - margin < t + r->x0) t = x - margin - r->x0;
    if (x + w + margin > t + W - r->x0) t = x + w + margin - (W - r->x0);
    int last = r->n - 1; /* the last item that takes room (a hidden app at the end does not) */
    while (last > 0 && (r->items[last].dying || r->items[last].hf > 0.99f)) last--;
    float total = item_x(ri, last) + item_w(r, &r->items[last]) + r->x0;
    if (t > total - W) t = total - W;
    if (t < 0) t = 0;
    return t;
}

/* card top to the next row's card top: LG's shelf heights plus its 170 px list spacing
 * (MainView.qml), the channels' measured off its screen */
static float row_pitch(int ri) {
    switch (ri) {
    case ROW_DASH: return 242 + 170;
    case ROW_CHANNELS: return 462;
    default: return row_h(&rows[ri]) + 170;
    }
}
/* rows below the apps are laid out in page coordinates, skipping empty ones */
static void layout_rows(void) {
    float y = DASH_PAGE + 444;
    for (int ri = ROW_DASH; ri <= LAST_PAGE_ROW; ri++) {
        rows[ri].y = y;
        if (row_live(&rows[ri])) y += row_pitch(ri);
    }
    while (cur_row > ROW_APPS && cur_row <= LAST_PAGE_ROW && !row_live(&rows[cur_row])) cur_row--; /* the row left from under the focus */
    if (cur_row <= ROW_APPS) page_target = 0;
    else page_target = DASH_PAGE + fmaxf(0, rows[cur_row].y + row_h(&rows[cur_row]) - DASH_PAGE - 1040);
}


static void load_item(int ri, Item *it) {
    Row *r = &rows[ri];
    Kind k = item_kind(r, it);
    if (it->icon[0]) load_picture(&it->icon_tex, it->icon, k == K_APP || k == K_RECENT, 0);
    if (it->image[0]) load_picture(&it->image_tex, it->image, k == K_RECENT, k == K_RECENT);
    float w = item_w(r, it);
    switch (k) {
    case K_APP: it->title_tex = text_cached(F_TILE, (int)(TILE * TILE_FOCUS) - 16, it->title); break;
    case K_PILL: it->title_tex = text_cached(F_PILL, PILL_MAXW, it->title); break;
    case K_HOMEDASH:
        it->title_tex = text_cached(F_CARD_NAME, (int)w - 44, it->title);
        it->sub_tex = icon_load(it->sub);
        break;
    case K_WHITE: /* MobileComponent: title SemiBold 27, sub Regular 24 */
        it->title_tex = text_cached(F_CARD_TITLE, (int)w - 48, it->title);
        it->sub_tex = text_cached(F_CARD_SUB, (int)w - 48, it->sub);
        it->sub2_tex = text_cached(F_CARD_SUB, (int)w - 48, it->sub2);
        break;
    case K_TOP: it->icon_tex = glyph_tex(it->glyph, 30); break; /* LG (TopGlobalAction.qml): Dingbat 30 in a 60 px button */
    case K_RECENT: /* RecentComponent: SemiBold 30 */
        it->title_tex = text_cached(FS(30), (int)w, it->title);
        break;
    case K_CHANNEL:
        /* TVProgramComponent: name SemiBold 33, number SemiBold 21, programme Regular 21 */
        it->title_tex = text_cached(it->icon[0] ? F_CARD_TITLE : FS(33), (int)w - 44, it->title);
        it->sub_tex = text_cached(FS(21), (int)w - 44, it->sub);
        it->sub2_tex = text_cached(it->icon[0] ? F_CARD_SUB : FR(21), (int)w - 44, it->sub2);
        break;
    }
}

/* returns 1 while anything is still moving */
static int update(float dt) {
    int moving = now_ms() < marquee_until;
    tcache_sweep(0);
    layout_rows();
    float nt = approach(trash_f, edit_mode == ED_TRASH || edit_mode == ED_CONFIRM ? 1.f : 0.f, 16.f, dt);
    float nc = approach(confirm_t, edit_mode == ED_CONFIRM ? 1.f : 0.f, 14.f, dt);
    if (nt != trash_f || nc != confirm_t) { trash_f = nt; confirm_t = nc; moving = 1; }
    float nh = approach(hide_f, edit_mode == ED_HIDE ? 1.f : 0.f, 16.f, dt);
    if (nh != hide_f) { hide_f = nh; moving = 1; }
    if (recents_refetch_at && now_ms() >= recents_refetch_at) {
        recents_refetch_at = 0;
        call("luna://com.webos.service.recent/listRecents", "{}", on_recents);
    }
    if (closing_all && now_ms() - closing_since > 4000) { /* never wait forever */
        logf_("close all: timed out"); closing_all = 0; recents_show(0);
        call("luna://com.webos.service.recent/listRecents", "{}", on_recents);
    }
    if (closing_all) moving = 1;
    float rt = approach(recents_t, recents_open ? 1.f : 0.f, 11.f, dt);
    float rtr = approach(rec_trash_f, recents_open && rec_part == 1 ? 1.f : 0.f, 16.f, dt);
    float rcl = approach(rec_close_f, recents_open && rec_part == 2 ? 1.f : 0.f, 16.f, dt);
    if (rt != recents_t || rtr != rec_trash_f || rcl != rec_close_f) { recents_t = rt; rec_trash_f = rtr; rec_close_f = rcl; moving = 1; }
    if (hint_state == HINT_SHOWING && now_ms() - hint_since > 12000) hint_done();
    float nhf = approach(hint_f, hint_state == HINT_SHOWING && !edit_mode && !recents_open && cur_row <= ROW_APPS ? 1.f : 0.f, 6.f, dt);
    if (nhf != hint_f) { hint_f = nhf; moving = 1; }
    /* dims slowly, wakes quickly */
    int idle = on_screen && now_ms() - last_input_ms > IDLE_DIM_MS;
    float nd = approach(dim_f, idle ? 1.f : 0.f, idle ? 1.2f : 12.f, dt);
    if (nd != dim_f) { dim_f = nd; moving = 1; }
    float np = approach(page, page_target, 10.f, dt);
    if (np != page) { page = np; moving = 1; }
    for (int ri = 0; ri < NROWS; ri++) {
        Row *r = &rows[ri];
        layout_x(ri);
        float ns = approach(r->scroll, row_target_scroll(ri), 12.f, dt);
        if (ns != r->scroll) { r->scroll = ns; moving = 1; }
        for (int i = 0; i < r->n; i++) {
            Item *it = &r->items[i];
            float tf = ri == ROW_RECENTS ? (recents_open && rec_part != 2 && i == r->sel ? 1.f : 0.f)
                                         : (!recents_open && ri == cur_row && i == r->sel ? 1.f : 0.f);
            float nf = approach(it->focus, tf, 18.f, dt);
            if (nf != it->focus) { it->focus = nf; moving = 1; }
            /* the zoom ignores Recents: the panel covers the row, and closing it must not spring
             * the selected tile or card again */
            int zon = ri == cur_row && i == r->sel;
            if (ri >= ROW_DASH && ri <= ROW_CHANNELS && zoom_step(it, zon, dt, FOCUS_OVERSHOOT)) moving = 1;
            /* app tiles: the same spring; none while editing */
            if (ri == ROW_APPS && zoom_step(it, zon && !edit_mode, dt, FOCUS_OVERSHOOT)) moving = 1;
            if (ri == ROW_APPS) {
                float nh2 = approach(it->hf, it->hidden && !edit_mode ? 1.f : 0.f, 9.f, dt);
                if (nh2 != it->hf) { it->hf = nh2; moving = 1; }
            }
            if (it->press > 0) { it->press = approach(it->press, 0, 9.f, dt); moving = 1; }
            float nl = approach(it->life, it->dying ? 0.f : 1.f, 9.f, dt);
            if (nl != it->life) { it->life = nl; moving = 1; }
            float tx = item_x(ri, i);
            if (!it->ax_set) { it->ax = tx; it->ax_set = 1; }
            float na = approach(it->ax, tx, 14.f, dt);
            if (na != it->ax) { it->ax = na; moving = 1; }
            if (it->dying && it->life < 0.01f) {
                free(it->params);
                memmove(it, it + 1, sizeof(Item) * (size_t)(r->n - i - 1));
                r->n--;
                if (r->sel > i) r->sel--;
                if (r->sel >= r->n) r->sel = r->n - 1;
                if (r->sel < 0) r->sel = 0;
                i--;
            }
        }
    }
    /* images the decoder finished: upload a few, then let the items that wait for them look again */
    if (decoder_poll(4)) {
        for (int ri = 0; ri < NROWS; ri++)
            for (int i = 0; i < rows[ri].n; i++) rows[ri].items[i].icon_state = 0;
        moving = 1;
    }
    /* rasterise at most two new texts per frame so input never waits on them */
    int budget = 2;
    for (int ri = 0; ri < NROWS && budget; ri++)
        for (int i = 0; i < rows[ri].n && budget; i++) {
            Item *it = &rows[ri].items[i];
            if (it->icon_state) continue;
            it->icon_state = 1;
            int before = ntcache; /* only real decodes (new cache entries) spend the budget */
            load_item(ri, it);
            if (ntcache != before) budget--;
            moving = 1;
        }
    if (budget) request_copies(); /* everything was tried: fetch what the jail hid */
    /* headings switch weight when their row takes focus: rasterise both ahead of time, so a
     * focus change never waits on text inside a frame */
    for (int ri = ROW_DASH; budget && ri <= LAST_PAGE_ROW; ri++) {
        Row *r = &rows[ri];
        const char *lbl[16]; int nl = 0;
        if (r->label) lbl[nl++] = r->label;
        for (int i = 0; i < r->n && nl < 16; i++)
            if (r->items[i].group[0] && (!i || strcmp(r->items[i - 1].group, r->items[i].group))) lbl[nl++] = r->items[i].group;
        for (int k = 0; k < nl && budget; k++) {
            int before = ntcache;
            text_cached(F_HEAD, 0, lbl[k]); text_cached(F_HEAD_FOCUS, 0, lbl[k]);
            if (ntcache != before) budget--;
        }
    }
    /* clock */
    time_t t = time(NULL) + tz_offset;
    struct tm tm; gmtime_r(&t, &tm);
    char cs[16];
    if (hour12) snprintf(cs, sizeof cs, "%d:%02d %s", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
    else snprintf(cs, sizeof cs, "%02d:%02d", tm.tm_hour, tm.tm_min);
    /* hidden, no one sees it: it catches up when Home comes back */
    if (strcmp(cs, clock_str) && (on_screen || !clock_tex.id)) {
        char ds[96];
        scpy(clock_str, sizeof clock_str, cs);
        format_date(ds, sizeof ds, &tm);
        tex_free(&clock_tex); tex_free(&date_tex);
        clock_tex = text_fit(f_clock, clock_str, 0); date_tex = text_fit(f_date, ds, 0);
        static const signed char shift[8][2] = {{0, 0}, {3, 1}, {5, 3}, {3, 5}, {0, 4}, {-2, 2}, {-3, 0}, {-1, -2}};
        clock_dx = shift[tm.tm_min % 8][0]; clock_dy = shift[tm.tm_min % 8][1];
        /* once a minute: the programmes' progress moves on, and a programme may have ended.
         * Not while hidden (standby can last weeks): coming back to the front refreshes them. */
        if (rank_failed && on_screen) request_rank();
        if (nchnames < 0 && on_screen) refresh();
        /* the system may not push a daylight-saving change, which leaves the UTC clock alone */
        static int asked_hour = -1;
        if (tm.tm_min == 0 && tm.tm_hour != asked_hour) { asked_hour = tm.tm_hour; call("luna://com.palm.systemservice/time/getSystemTime", "{}", on_time); }
        if (nchans && on_screen) { build_channels(); if (tm.tm_min % 5 == 0) request_events(); }
        moving = 1;
    }
    return moving;
}

/* the one focus look for app tiles, cards and Recents: a drop shadow, a soft glow (in the app's
 * colour for tiles, white for the rest) and a thin light edge */
static void focus_glow(float x, float y, float w, float h, float rad, float f, Col g) {
    draw(x, y + 10 * f, w, h, rad, 22.f, rgba(0, 0, 0, 0.6f * f), NULL, 1, 1);
    draw(x, y, w, h, rad, 34.f, rgba(g.r, g.g, g.b, 0.5f * f), NULL, 1, 1);
    fill(x - 2, y - 2, w + 4, h + 4, rad + 2, rgba(1, 1, 1, 0.85f * f));
}

static Col on_tile(Col bg, float a) { return luma(bg) > 0.6f ? rgba(0.1f, 0.1f, 0.1f, a) : rgba(1, 1, 1, a); }

/* LG's trash icon, centred in a d-wide button at x, y; its focused look past f 0.5 */
static void draw_trash(float x, float y, float d, float f) {
    Tex ic = icon_load(f > 0.5f ? HOME_IMG "ic_delete_f.png" : HOME_IMG "ic_delete_n.png");
    draw_text(&ic, x + (d - ic.w) * 0.5f, y + (d - ic.h) * 0.5f, rgba(1, 1, 1, 1), 0);
}

static void draw_app_tile(Item *it, float x, float y) {
    float f = it->focus;
    float lift = edit_mode == ED_MOVE && f > 0.5f ? 1.f : 0.f;
    /* in edit mode tiles keep their size; the one being carried is raised, as in LG's edit list */
    float grow = edit_mode ? 0.f : (TILE_FOCUS - 1.f);
    float s = (1.f + grow * it->zoom - 0.06f * it->press) * (0.4f + 0.6f * it->life);
    float w = TILE * s;
    /* grow upwards from the row's baseline, like LG's */
    float cx = x + TILE * 0.5f * it->life, x0 = cx - w * 0.5f, y0 = y + TILE - w - 40 * lift;
    float rad = TILE_R * s;
    Col bg = it->color; bg.a = 1;
    if (f > 0.01f) {
        if (edit_mode) { /* editing keeps LG's white ring: it says which tile the buttons act on */
            draw(x0, y0 + 10 * f + 10 * lift, w, w, rad, 22.f + 10 * lift, rgba(0, 0, 0, 0.6f * f), NULL, 1, 1);
            fill(x0 - 4, y0 - 4, w + 8, w + 8, rad + 4, rgba(1, 1, 1, f * (1 - 0.6f * fmaxf(trash_f, hide_f))));
        } else { /* the glow in the app's own colour (white for dark tiles) */
            Col g = luma(bg) < 0.18f ? rgba(1, 1, 1, 1) : rgba(fminf(1, bg.r * 1.25f + 0.1f), fminf(1, bg.g * 1.25f + 0.1f), fminf(1, bg.b * 1.25f + 0.1f), 1);
            focus_glow(x0, y0, w, w, rad, f, g);
        }
    }
    fill(x0, y0, w, w, rad, bg);
    if (it->icon_tex.id) {
        /* the icon fills the tile (LG's appIcon is tile-sized), keeping its aspect */
        float iw = w, ih = w * it->icon_tex.h / (float)it->icon_tex.w;
        if (ih > w) { ih = w; iw = w * it->icon_tex.w / (float)it->icon_tex.h; }
        draw(cx - iw * 0.5f, y0 + (w - ih) * 0.5f, iw, ih, rad, 0, rgba(1, 1, 1, 1), &it->icon_tex, 1, 1);
        if (f > 0.01f && it->title_tex.id && it->show_title)
            draw_text(&it->title_tex, cx - it->title_tex.w * 0.5f, y0 + w - it->title_tex.h - 11 * s, on_tile(bg, f), 0);
    } else if (it->title_tex.id) {
        draw_text(&it->title_tex, cx - it->title_tex.w * 0.5f, y0 + (w - it->title_tex.h) * 0.5f, on_tile(bg, 1), 0);
    }
    if (lift) { /* LG's arrows beside a tile being carried */
        Tex l = icon_load(HOME_IMG "arrow_left_n.png"), rr = icon_load(HOME_IMG "arrow_right_n.png");
        draw_text(&l, x0 - 12 - l.w, y0 + (w - l.h) * 0.5f, rgba(1, 1, 1, 1), 0);
        draw_text(&rr, x0 + w + 12, y0 + (w - rr.h) * 0.5f, rgba(1, 1, 1, 1), 0);
    }
    if (edit_mode && it->hidden) { /* a hidden app: a crossed eye in its corner */
        Tex e = eye_tex(1);
        float d = 40, bx = x0 + w - d - 6, by = y0 + 6;
        fill(bx, by, d, d, d * 0.5f, rgba(0, 0, 0, 0.7f));
        draw_mask(bx + (d - e.w * 0.7f) * 0.5f, by + (d - e.h * 0.7f) * 0.5f, e.w * 0.7f, e.h * 0.7f, rgba(1, 1, 1, 1), &e);
    }
    if (edit_mode && f > 0.5f) { /* our eye (hide / show), and LG's trash right of it */
        float d = 60, ty = y0 - 16 - d, hx = it->removable ? cx - d - 8 : cx - d * 0.5f;
        set_rect(ed_eye_r, hx, ty, d, d);
        fill(hx, ty, d, d, d * 0.5f, rgba(1, 1, 1, 0.22f + 0.78f * hide_f));
        Tex e = eye_tex(!it->hidden); /* what OK will do: hide (crossed) or show */
        draw_mask(hx + (d - e.w) * 0.5f, ty + (d - e.h) * 0.5f, (float)e.w, (float)e.h,
                  hide_f > 0.5f ? rgba(0.2f, 0.2f, 0.22f, 1) : rgba(1, 1, 1, 1), &e);
    }
    if (edit_mode && f > 0.5f && it->removable) { /* LG's trash, over the tile */
        float d = 60, tx = cx + 8, ty = y0 - 16 - d;
        set_rect(ed_trash_r, tx, ty, d, d);
        fill(tx, ty, d, d, d * 0.5f, rgba(1, 1, 1, 0.22f + 0.78f * trash_f));
        draw_trash(tx, ty, d, trash_f);
    }
}

static void draw_pill(Item *it, float x, float y, float w) {
    float f = it->focus;
    float s = (1.f + 0.06f * f - 0.04f * it->press) * (0.4f + 0.6f * it->life);
    float pw = w * s, ph = PILL_H * s;
    float x0 = x + (w * it->life - pw) * 0.5f, y0 = y + (PILL_H - ph) * 0.5f;
    if (f > 0.01f) draw(x0, y0 + 6 * f, pw, ph, ph * 0.5f, 16.f, rgba(0, 0, 0, 0.5f * f), NULL, 1, 1);
    float g = 0.16f + 0.84f * f;
    fill(x0, y0, pw, ph, ph * 0.5f, rgba(g, g, g + 0.02f * (1 - f), 0.58f + 0.42f * f));
    Col fg = f > 0.5f ? rgba(0.08f, 0.08f, 0.1f, 1) : rgba(1, 1, 1, 0.92f);
    float tx = x0 + 20 * s, cy = y0 + ph * 0.5f;
    if (it->icon_tex.id) {
        float ih = 30 * s, iw = ih * it->icon_tex.w / (float)it->icon_tex.h;
        draw_mask(tx, cy - ih * 0.5f, iw, ih, fg, &it->icon_tex);
        tx += 38 * s;
    }
    draw_text(&it->title_tex, tx, cy - it->title_tex.h * 0.5f, fg, 0);
}

/* how much focus the cards under a heading spanning x .. x + w have (0..1) */
static float under_focus(const Row *r, float x, float w) {
    float f = 0;
    for (int i = 0; i < r->n; i++) {
        float ix = r->items[i].ax - r->scroll;
        if (ix < x + w && ix + item_w(r, &r->items[i]) > x) f += r->items[i].focus;
    }
    return fminf(1.f, f);
}

/* draw order, so a growing card is never under its neighbours: still ones, then ones losing
 * focus (shrinking), then the selected one */
static int draw_pass(const Row *r, int i) {
    const Item *it = &r->items[i];
    if (i == r->sel) return 2;
    return it->focus > 0.01f || it->zoom > 0.f;
}

/* a frosted-glass panel: the blurred wallpaper under it (the dashboard's own background, so it
 * reads as one sheet of glass), a light tint that brightens with focus, and a sheen at the top.
 * Opaque, so nothing drawn under it shows through. */
static void glass(float x, float y, float w, float h, float rad, float f) {
    if (wallpaper_frost.id) {
        g_uvr[0] = x / W; g_uvr[1] = y / H; g_uvr[2] = w / W; g_uvr[3] = h / H;
        draw(x, y, w, h, rad, 0, rgba(1, 1, 1, 1), &wallpaper_frost, 1, 1);
        g_uvr[0] = g_uvr[1] = 0; g_uvr[2] = g_uvr[3] = 1;
    } else fill(x, y, w, h, rad, rgba(0.12f, 0.12f, 0.13f, 1));
    fill(x, y, w, h, rad, rgba(1, 1, 1, 0.10f + 0.06f * f));
    draw(x, y, w, h, rad, 0, rgba(1, 1, 1, 0.07f), NULL, 1, 0);
}

/* Home Dashboard / Mobile / TV / Inputs / channel cards */
static void draw_card(Row *r, Item *it, float x, float y) {
    Kind k = item_kind(r, it);
    float f = it->focus;
    float cw = item_w(r, it), ch = kind_h(k);
    float s = (1.f + 0.15f * it->zoom - 0.03f * it->press) * (0.4f + 0.6f * it->life);
    float w = cw * s, h = ch * s;
    /* LG zooms about a point that slides from the card's left edge at the screen's left to its
     * right edge at the screen's right (RelatedList.qml's Scale origin), so edge cards grow inwards */
    float ox = fminf(cw, fmaxf(0, cw * 0.5f * (x - r->x0) / (W * 0.5f - cw * 0.5f)));
    float x0 = x + ox * it->life - ox * s, y0 = y + (ch - h) * 0.5f, cx = x0 + w * 0.5f;
    float rad = CARD_R;
    if (f > 0.01f) focus_glow(x0, y0, w, h, rad, f, rgba(1, 1, 1, 1));
    float pad = 24 * s; /* LG's card insets */
    if (k == K_HOMEDASH) {
        fill(x0, y0, w, h, rad, rgba(0.30f, 0.40f, 0.34f, 1));
        draw(x0, y0, w, h, rad, 0, rgba(0.56f, 0.66f, 0.58f, 1), NULL, 1, 0); /* lighter at the top */
        float th = it->title_tex.id ? (float)it->title_tex.h : 0, lh = it->sub_tex.id ? 16 * s : 0;
        float is = 64 * s, total = is + 10 * s + th + (lh ? 8 * s + lh : 0), yy = y0 + (h - total) * 0.5f;
        if (it->icon_tex.id) draw(cx - is * 0.5f, yy, is, is, 0, 0, rgba(1, 1, 1, 1), &it->icon_tex, 1, 1);
        yy += is + 10 * s;
        if (it->title_tex.id) draw_text(&it->title_tex, cx - it->title_tex.w * 0.5f, yy, rgba(1, 1, 1, 1), 1);
        yy += th + 8 * s;
        if (it->sub_tex.id) { /* the ThinQ logo */
            float lw = lh * it->sub_tex.w / (float)it->sub_tex.h;
            draw_mask(cx - lw * 0.5f, yy, lw, lh, rgba(1, 1, 1, 0.85f), &it->sub_tex);
        }
        return;
    }
    /* dark glass, as Recents: white text, the second line LG's #c1c1c1 */
    Col fg = rgba(1, 1, 1, 0.95f), fg2 = rgba(0.76f, 0.76f, 0.76f, 1);
    if (it->highlight) fg = fg2 = rgba(0.93f, 0.62f, 0.1f, 1); /* the channel the TV is on, LG's orange */
    glass(x0, y0, w, h, rad, f);
    if (it->icon_tex.id) {
        float is = 54 * s; /* LG draws these 2k assets at their own 54 px */
        draw_mask(x0 + pad, y0 + pad, is * it->icon_tex.w / (float)it->icon_tex.h, is, fg, &it->icon_tex);
    }
    if (k == K_CHANNEL && !it->icon[0]) {
        /* name, progress bar, number, programme */
        /* placed as on LG's TVProgramComponent: name at the top, the bar under it, number and
         * programme at the foot */
        if (it->title_tex.id) draw_text(&it->title_tex, x0 + pad, y0 + 34 * s - it->title_tex.h * 0.5f + 16 * s, fg, 0);
        float by = y0 + 106 * s, bw = w - 2 * pad;
        fill(x0 + pad, by, bw, 4 * s, 2, rgba(1, 1, 1, 0.18f));
        if (it->progress >= 0) fill(x0 + pad, by, bw * it->progress, 4 * s, 2, it->highlight ? fg : rgba(0.85f, 0.85f, 0.85f, 1));
        if (it->sub_tex.id) draw_text(&it->sub_tex, x0 + pad, y0 + 152 * s - it->sub_tex.h * 0.5f, fg2, 0);
        if (it->sub2_tex.id) draw_text(&it->sub2_tex, x0 + pad, y0 + 184 * s - it->sub2_tex.h * 0.5f, fg2, 0);
        return;
    }
    /* icon top-left, title and the line(s) under it at the foot */
    float ty = y0 + h - pad;
    const Tex *lower = it->sub_tex.id ? &it->sub_tex : &it->sub2_tex;
    if (lower->id) { ty -= lower->h; draw_text(lower, x0 + pad, ty, fg2, 0); ty -= 3 * s; }
    if (!it->title_tex.id) return;
    float avail = w - 2 * pad;
    Tex full = f > 0.5f && k == K_WHITE ? text_cached(F_CARD_TITLE, 0, it->title) : it->title_tex;
    if (full.w <= avail + 1) { draw_text(&it->title_tex, x0 + pad, ty - it->title_tex.h, fg, 0); return; }
    /* too long, and focused: it scrolls round after a second, as LG's ScrollText does */
    float gap = 80, cycle = full.w + gap, t = (float)(now_ms() - it->fstart) / 1000.f - 1.f;
    float off = t > 0 ? fmodf(t * 70.f, cycle) : 0;
    flush();
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)(x0 + pad), (GLint)(H - (ty + 2)), (GLsizei)avail, (GLsizei)(full.h + 4));
    draw_text(&full, x0 + pad - off, ty - full.h, fg, 0);
    if (off > cycle - avail) draw_text(&full, x0 + pad - off + cycle, ty - full.h, fg, 0);
    flush();
    glDisable(GL_SCISSOR_TEST);
    if (now_ms() - last_input_ms < IDLE_DIM_MS) marquee_until = now_ms() + 100; /* left alone: stand still */
}

/* the Recents panel: LG's layout, measured off its screen at 1080p */
static void draw_recents(void) {
    float t = recents_t, py = 600 + 480 * (1 - t) * (1 - t);
    if (wallpaper_frost.id) { /* frosted glass: the blurred wallpaper behind it, a touch lighter */
        g_uvr[1] = py / H; g_uvr[3] = (H - py) / H;
        draw(0, py, W, H - py + 2, 0, 0, rgba(1, 1, 1, 1), &wallpaper_frost, 1, 1);
        g_uvr[1] = 0; g_uvr[3] = 1;
        fill(0, py, W, H - py + 2, 0, rgba(1, 1, 1, 0.07f));
        fill(0, py, W, 2, 0, rgba(1, 1, 1, 0.12f)); /* its top edge catches the light */
    } else fill(0, py, W, H - py + 2, 0, rgba(0.19f, 0.20f, 0.22f, 0.97f));
    Tex title = text_cached(F_TITLE, 0, S(S_RECENTS));
    draw_text(&title, 48, py + 50, rgba(1, 1, 1, 1), 0);
    Row *r = &rows[ROW_RECENTS];
    rec_py = py;
    rec_trash_r[0] = rec_close_r[0] = -1;
    if (!row_live(r) && !closing_all) { /* nothing to close: a line says so where the cards would be */
        Tex none = text_cached(F_CARD_SUB, 0, S(S_NO_RECENTS));
        draw_text(&none, (W - none.w) * 0.5f, py + 240, rgba(1, 1, 1, 0.7f), 0);
        return;
    }
    /* Close All */
    /* LG's SimpleButton, small: SemiBold 30, 21 px margins, at least 150 x 60, #7d848c / #e6e6e6 */
    Tex ct = text_cached(FS(30), 0, S(S_CLOSE_ALL));
    /* measured off LG's screen: the label sits in 34 px of padding a side (its margin items
     * plus the label area's own inset) */
    float bw = fmaxf(150.f, ct.w + 68.f), bh = 60, bx = W - 48 - bw, by = py + 40;
    set_rect(rec_close_r, bx, by, bw, bh);
    if (rec_close_f > 0.01f) draw(bx, by + 6, bw, bh, 6, 14, rgba(0, 0, 0, 0.5f * rec_close_f), NULL, 1, 1);
    const Col nb = {0x7d / 255.f, 0x84 / 255.f, 0x8c / 255.f, 1}, fb = {0xe6 / 255.f, 0xe6 / 255.f, 0xe6 / 255.f, 1};
    const Col nt = fb, ft = {0x4c / 255.f, 0x50 / 255.f, 0x59 / 255.f, 1};
    float q = rec_close_f;
    fill(bx, by, bw, bh, 6, rgba(nb.r + (fb.r - nb.r) * q, nb.g + (fb.g - nb.g) * q, nb.b + (fb.b - nb.b) * q, 1));
    draw_text(&ct, bx + (bw - ct.w) * 0.5f, by + (bh - ct.h) * 0.5f, q > 0.5f ? ft : nt, 0);
    /* the cards */
    float y = py + 180;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < r->n; i++) {
            Item *it = &r->items[i];
            if (draw_pass(r, i) != pass) continue;
            float x = it->ax - r->scroll;
            if (x > W || x + 336 < 0) continue;
            float v = vis_of(it);
            g_alpha = v * t;
            float f = it->focus, s = (1.f + 0.15f * f - 0.04f * it->press) * (0.6f + 0.4f * v);
            float w = 336 * s, h = 189 * s, cx = x + 168 * slot_of(it), x0 = cx - w * 0.5f, y0 = y + (189 - h) * 0.5f;
            if (f > 0.01f) focus_glow(x0, y0, w, h, 2, f, rgba(1, 1, 1, 1));
            Col bg = it->color; bg.a = 1;
            fill(x0, y0, w, h, 2, bg);
            if (it->image_tex.id) { /* cover: crop the snapshot to the card's aspect */
                float ia = it->image_tex.w / (float)it->image_tex.h, ca = w / h;
                if (ia > ca) { g_uvr[2] = ca / ia; g_uvr[0] = (1 - g_uvr[2]) * 0.5f; }
                else { g_uvr[3] = ia / ca; g_uvr[1] = (1 - g_uvr[3]) * 0.5f; }
                draw(x0, y0, w, h, 2, 0, rgba(1, 1, 1, 1), &it->image_tex, 1, 1);
                g_uvr[0] = g_uvr[1] = 0; g_uvr[2] = g_uvr[3] = 1;
            } else if (it->icon_tex.id) {
                float is = h * 0.5f, iw = is * it->icon_tex.w / (float)it->icon_tex.h;
                draw(cx - iw * 0.5f, y0 + (h - is) * 0.5f, iw, is, 0, 0, rgba(1, 1, 1, 1), &it->icon_tex, 1, 1);
            }
            if (it->title_tex.id) {
                float tw = (float)it->title_tex.w;
                draw_text(&it->title_tex, cx - tw * 0.5f, y0 + h + 14, rgba(1, 1, 1, 0.75f + 0.25f * f), 0);
            }
            if (f > 0.5f) { /* the trash over the focused card closes that app */
                float d = 56, tx = cx - d * 0.5f, ty = y0 - 18 - d;
                set_rect(rec_trash_r, tx, ty, d, d);
                if (rec_trash_f > 0.01f) fill(tx, ty, d, d, d * 0.5f, rgba(1, 1, 1, rec_trash_f));
                draw_trash(tx, ty, d, rec_trash_f);
            }
            g_alpha = 1;
        }
}

static void render(void) {
    glViewport(0, 0, (GLsizei)W, (GLsizei)H);
    float dash = fminf(1.f, page / DASH_PAGE); /* 0 on the home screen, 1 on the dashboard */
    draw_calls = 0;
    /* Cleared first, even under an opaque wallpaper: the wallpaper rises slower than the page
     * (parallax) and leaves a strip at the bottom uncovered, and on this tile-based GPU a clear
     * also spares reading the last frame back in. */
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    /* the wallpaper on the home screen; on the dashboard (LG's is black) its frosted copy,
     * cross-faded while the page scrolls. Each is one opaque full-screen draw when still. */
    const Tex *bgt = dash < 0.999f || !wallpaper_frost.id ? &wallpaper : &wallpaper_frost;
    if (bgt->id && (dash < 0.999f || wallpaper_frost.id)) {
        glDisable(GL_BLEND);
        glUseProgram(prog_bg);
        for (int i = 1; i < 7; i++) glDisableVertexAttribArray((GLuint)i);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glUniform4f(ub_rect, 0, bgt == &wallpaper ? -page * 0.25f : 0, W, H);
        glBindTexture(GL_TEXTURE_2D, bgt->id);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        batch_tex = 0;
        glUseProgram(prog);
        glEnable(GL_BLEND);
        if (bgt == &wallpaper && dash > 0.001f) {
            if (wallpaper_frost.id) draw(0, 0, W, H, 0, 0, rgba(1, 1, 1, dash), &wallpaper_frost, 1, 1);
            else fill(0, 0, W, H, 0, rgba(0, 0, 0, dash));
        }
    } else glEnable(GL_BLEND);

    draw_text(&clock_tex, 64 + clock_dx, 36 + clock_dy - page, rgba(1, 1, 1, 1), 1);
    draw_text(&date_tex, 68 + clock_dx, 36 + clock_dy + (float)clock_tex.h - 6 - page, rgba(1, 1, 1, 0.8f), 1);

    /* LG's top-right buttons: a glyph, on a light rounded square when focused */
    Row *tr = &rows[ROW_TOP];
    for (int i = 0; i < tr->n; i++) {
        Item *it = &tr->items[i];
        float x = it->ax - tr->scroll, y = tr->y - page, f = it->focus;
        if (y < -60) break;
        if (f > 0.01f) fill(x, y, 60, 60, 6, rgba(0.9f, 0.9f, 0.9f, f));
        if (it->icon_tex.id) {
            Col c = rgba(0.9f + (0.3f - 0.9f) * f, 0.9f + (0.31f - 0.9f) * f, 0.9f + (0.35f - 0.9f) * f, 1);
            draw_mask(x + (60 - it->icon_tex.w) * 0.5f, y + (60 - it->icon_tex.h) * 0.5f, (float)it->icon_tex.w, (float)it->icon_tex.h, c, &it->icon_tex);
        }
    }

    /* inputs: pills (hidden in edit mode: the hint takes their place) */
    Row *r = &rows[ROW_INPUTS];
    float ry = r->y - page;
    if (!edit_mode && ry > -PILL_H)
        for (int i = 0; i < r->n; i++) {
            Item *it = &r->items[i];
            float x = it->ax - r->scroll, w = item_w(r, it);
            if (x > W || x + w < 0) continue;
            g_alpha = it->life * it->life * (1 - dash) * (1 - recents_t);
            draw_pill(it, x, ry, w);
            g_alpha = 1;
        }
    if (edit_mode) {
        Tex hint = text_cached(F_PILL, 0, S(edit_mode == ED_MOVE ? S_EDIT_HINT : S_BROWSE_HINT));
        draw_text(&hint, (W - hint.w) * 0.5f, rows[ROW_APPS].y - page - 150, rgba(1, 1, 1, 1), 1);
    }
    if (hint_f > 0.01f) { /* the first-launch hint, on glass over the input pills */
        Tex ht = text_cached(F_PILL, 0, S(S_FIRST_HINT));
        float hw = ht.w + 64.f, hh = 64.f, hx = (W - hw) * 0.5f, hy = rows[ROW_INPUTS].y - 110 - page + 12 * (1 - hint_f);
        g_alpha = hint_f * (1 - dash);
        glass(hx, hy, hw, hh, hh * 0.5f, 0);
        draw_text(&ht, hx + 32, hy + (hh - ht.h) * 0.5f, rgba(1, 1, 1, 1), 0);
        g_alpha = 1;
    }
    /* apps: unfocused first, focused last so its shadow and scale sit on top */
    r = &rows[ROW_APPS];
    ed_eye_r[0] = ed_trash_r[0] = ed_conf_r[0][0] = ed_conf_r[1][0] = -1;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < r->n; i++) {
            Item *it = &r->items[i];
            if (draw_pass(r, i) != pass) continue;
            float x = it->ax - r->scroll;
            if (x > W || x + TILE < 0) continue;
            if (it->hf > 0.99f) continue;
            g_alpha = it->life * it->life * (1 - recents_t) * (1 - it->hf) * (it->hidden ? 0.4f + 0.6f * it->focus : 1.f);
            draw_app_tile(it, x, r->y - page);
            g_alpha = 1;
        }
    /* the dashboard rows */
    for (int ri = ROW_DASH; ri <= LAST_PAGE_ROW; ri++) {
        r = &rows[ri];
        float y = r->y - page;
        if (!row_live(r) || y > H || y + row_h(r) < -120) continue;
        /* headings over the cards, placed as on LG's screen. A heading with the focused card
         * under it rises out of the zoomed card's way (Shelf.qml shiftMainCategory: 15% of 30% of
         * the card's height), following the focus so it glides rather than bounces. */
        float hy = y - (ri == ROW_DASH ? 46 : 54), lift = 0.045f * row_h(r);
        int row_focus = ri == cur_row && !recents_open;
        if (r->label) {
            Tex lt = text_cached(row_focus ? F_HEAD_FOCUS : F_HEAD, 0, r->label);
            draw_text(&lt, r->x0, hy - lift * under_focus(r, r->x0, (float)lt.w), rgba(1, 1, 1, 0.9f), 0);
        }
        for (int i = 0; i < r->n; i++) { /* group headings, over each group's first card */
            Item *it = &r->items[i];
            if (!it->group[0] || (i && !strcmp(r->items[i - 1].group, it->group))) continue;
            /* RelatedList.qml: the Home Dashboard's title white, SemiBold in the focused row;
             * the other categories Regular #c1c1c1 */
            int title = i == 0 && dash_title[0] && !strcmp(it->group, dash_title);
            Tex gt = text_cached(title && row_focus ? F_HEAD_FOCUS : F_HEAD, 0, it->group);
            float gx = it->ax - r->scroll;
            g_alpha = it->life;
            draw_text(&gt, gx, hy - lift * under_focus(r, gx, (float)gt.w), title ? rgba(1, 1, 1, 1) : rgba(0.76f, 0.76f, 0.76f, 1), 0);
            g_alpha = 1;
        }
        for (int pass = 0; pass < 3; pass++)
            for (int i = 0; i < r->n; i++) {
                Item *it = &r->items[i];
                if (draw_pass(r, i) != pass) continue;
                float x = it->ax - r->scroll;
                if (x > W || x + item_w(r, it) < 0) continue;
                g_alpha = it->life * it->life;
                draw_card(r, it, x, y);
                g_alpha = 1;
            }
    }
    if (recents_t > 0.01f) draw_recents();
    if (confirm_t > 0.01f && rows[ROW_APPS].n) { /* "Delete …?" over a dimmed screen */
        Item *it = &rows[ROW_APPS].items[rows[ROW_APPS].sel];
        g_alpha = confirm_t;
        fill(0, 0, W, H, 0, rgba(0, 0, 0, 0.6f));
        float pw = 820, ph = 300, px = (W - pw) * 0.5f, py = (H - ph) * 0.5f + 30 * (1 - confirm_t);
        draw(px, py + 16, pw, ph, 18, 30, rgba(0, 0, 0, 0.6f), NULL, 1, 1);
        fill(px, py, pw, ph, 18, rgba(0.16f, 0.16f, 0.17f, 1));
        char q[300]; snprintf(q, sizeof q, S(S_DELETE_Q), it->title);
        Tex qt = text_cached(F_CARD_NAME, (int)pw - 80, q);
        draw_text(&qt, px + (pw - qt.w) * 0.5f, py + 70, rgba(1, 1, 1, 1), 0);
        for (int b = 0; b < 2; b++) {
            int is_del = b == 0, focused = confirm_delete == is_del;
            float bw = 300, bh = 70, bx = px + pw * 0.5f + (is_del ? -bw - 16 : 16), by = py + ph - bh - 50;
            set_rect(ed_conf_r[b], bx, by, bw, bh);
            fill(bx, by, bw, bh, bh * 0.5f, focused ? rgba(1, 1, 1, 1) : rgba(1, 1, 1, 0.12f));
            Tex bt = text_cached(F_CARD_TITLE, 0, S(is_del ? S_DELETE : S_CANCEL));
            draw_text(&bt, bx + (bw - bt.w) * 0.5f, by + (bh - bt.h) * 0.5f, focused ? rgba(0.1f, 0.1f, 0.1f, 1) : rgba(1, 1, 1, 0.9f), 0);
        }
        g_alpha = 1;
    }
    if (dim_f > 0.001f) fill(0, 0, W, H, 0, rgba(0, 0, 0, IDLE_DIM * dim_f));
    flush();
}

/* ---------- input ----------
 * LG's rule: the first D-pad press hides the Magic Remote pointer and hands focus to the keys;
 * with the pointer hidden the system delivers OK as a key, not a click at the pointer. The
 * pointer comes back (the compositor shows it) when the remote is moved, and only a deliberate
 * move takes focus back, not the jiggle of a hand pressing OK. */

static SDL_bool (*cursor_visibility)(SDL_bool);
static int dpad_mode;
static float motion_since_dpad;
#define DPAD_MOTION_GATE 40.f
#define LONG_PRESS_MS 700.0

static void enter_dpad_mode(void) {
    if (!dpad_mode && cursor_visibility) cursor_visibility(SDL_FALSE);
    dpad_mode = 1;
    motion_since_dpad = 0;
}

static void on_moved(cJSON *root, const char *raw) { (void)root; logf_("moveLaunchPoint: %.200s", raw ? raw : ""); }

static int move_from = -1; /* where the carried tile was picked up */

/* put the carried tile down: tell SAM if it moved */
static int edit_mode_moving(void) { return edit_mode == ED_MOVE; }
static void edit_drop(void) {
    if (edit_mode != ED_MOVE) return;
    edit_mode = ED_BROWSE;
    Row *r = &rows[ROW_APPS];
    if (r->n && r->sel != move_from) {
    Item *it = &r->items[r->sel];
    /* SAM's position counts our own launch point, which the row leaves out */
    int pos = 0;
    for (int i = 0; i < r->sel; i++) pos += !r->items[i].dying;
    if (own_index >= 0 && pos >= own_index) pos++;
    char p[300]; snprintf(p, sizeof p, "{\"launchPointId\":\"%s\",\"position\":%d}", it->lpid, pos);
    logf_("move %s", p);
    call("luna://com.webos.applicationManager/moveLaunchPoint", p, on_moved);
    }
    /* after the move, or the reply would carry the old order */
    if (lp_stale) { lp_stale = 0; call("luna://com.webos.applicationManager/listLaunchPoints", "{}", on_launchpoints); }
}

static void edit_exit(void) { edit_drop(); edit_mode = ED_OFF; apps_fix_sel(); }

static char removing_app[160];
static void on_removed(cJSON *root, const char *raw) {
    logf_("remove: %.200s", raw ? raw : "");
    /* SAM may refuse a Developer Mode app's launch point: uninstall it the dev way */
    if (!cJSON_IsTrue(cJSON_GetObjectItem(root, "returnValue")) && removing_app[0]) {
        char p[220]; snprintf(p, sizeof p, "{\"id\":\"%s\"}", removing_app);
        removing_app[0] = 0;
        call("luna://com.webos.appInstallService/dev/remove", p, on_launched);
    }
}

static void edit_delete(void) {
    Row *r = &rows[ROW_APPS];
    if (!r->n) return;
    /* forgotten only here, so it doesn't come back hidden if reinstalled. An app merely missing
     * from a list (a USB drive out, a service restarting) is still wanted hidden when it returns */
    hidden_set(r->items[r->sel].id, 0);
    Item *it = &r->items[r->sel];
    /* LG deletes through SAM's removeLaunchPoint (LaunchPointInterfaces.qml); for an app's
     * default launch point that uninstalls it. The row closes up when SAM pushes the change. */
    char p[300]; snprintf(p, sizeof p, "{\"launchPointId\":\"%s\"}", it->lpid);
    scpy(removing_app, sizeof removing_app, strstr(it->icon, "/media/developer/") ? it->app : "");
    logf_("delete %s", p);
    call("luna://com.webos.applicationManager/removeLaunchPoint", p, on_removed);
}

/* every key while editing; returns 1 when it was consumed */
static int edit_key_(int dx, int dy, int ok, int back);
static int edit_key(int dx, int dy, int ok, int back) {
    int before = edit_mode, used = edit_key_(dx, dy, ok, back);
    if (edit_mode != before) logf_("edit: state %d -> %d (dx %d dy %d ok %d back %d)", before, edit_mode, dx, dy, ok, back);
    return used;
}
static int edit_key_(int dx, int dy, int ok, int back) {
    Row *r = &rows[ROW_APPS];
    if (!edit_mode || !r->n) return 0;
    Item *cur = &r->items[r->sel];
    switch (edit_mode) {
    case ED_MOVE:
        if (dx) { /* the tile travels with the arrows; the others glide out of its way */
            int j = r->sel + dx;
            while (j >= 0 && j < r->n && r->items[j].dying) j += dx;
            if (j >= 0 && j < r->n) { Item t = r->items[r->sel]; r->items[r->sel] = r->items[j]; r->items[j] = t; r->sel = j; }
        } else if (ok) edit_drop();
        else if (dy < 0) { edit_drop(); edit_mode = ED_HIDE; }
        else if (dy > 0 || back) edit_exit();
        return 1;
    case ED_BROWSE:
        if (dx) {
            int j = r->sel + dx;
            while (j >= 0 && j < r->n && r->items[j].dying) j += dx;
            if (j >= 0 && j < r->n) r->sel = j;
        } else if (ok) { edit_mode = ED_MOVE; move_from = r->sel; }
        else if (dy < 0) edit_mode = ED_HIDE;
        else if (dy > 0 || back) edit_exit();
        return 1;
    /* over a tile: the eye, and LG's trash right of it when the app can be deleted. Left and
     * right walk the buttons, then on to the next tile's */
    case ED_HIDE:
        if (ok) { cur->hidden = !cur->hidden; hidden_set(cur->id, cur->hidden); }
        else if (back) edit_exit();
        else if (dy > 0) edit_mode = ED_BROWSE;
        else if (dx > 0 && cur->removable) edit_mode = ED_TRASH;
        else if (dx) {
            int was = r->sel;
            edit_mode = ED_BROWSE; edit_key_(dx, 0, 0, 0);
            edit_mode = dx < 0 && r->sel != was && r->items[r->sel].removable ? ED_TRASH : ED_HIDE;
        }
        return 1;
    case ED_TRASH:
        if (ok) { edit_mode = ED_CONFIRM; confirm_delete = 0; } /* Cancel first: deleting takes intent */
        else if (back) edit_exit();
        else if (dy > 0) edit_mode = ED_BROWSE;
        else if (dx < 0) edit_mode = ED_HIDE;
        else if (dx > 0) {
            int was = r->sel;
            edit_mode = ED_BROWSE; edit_key_(dx, 0, 0, 0);
            edit_mode = r->sel != was ? ED_HIDE : ED_TRASH;
        }
        return 1;
    case ED_CONFIRM:
        if (dx) confirm_delete = dx < 0;
        else if (ok) { if (confirm_delete) edit_delete(); edit_mode = ED_BROWSE; }
        else if (back) edit_mode = ED_BROWSE;
        return 1;
    }
    return 0;
}

static void move(int dx, int dy) {
    enter_dpad_mode();
    if (recents_open) { recents_key(dx, dy, 0, 0); return; }
    if (edit_key(dx, dy, 0, 0)) return;
    if (dy) {
        int nr = cur_row + dy;
        while (nr >= 0 && nr <= LAST_PAGE_ROW && !row_live(&rows[nr])) nr += dy;
        if (nr >= 0 && nr <= LAST_PAGE_ROW) {
            Row *c = &rows[cur_row], *t = &rows[nr];
            c->back_sel = c->sel; c->back_ok = 1;
            if (t->back_ok && t->back_sel < t->n && !skipped(&t->items[t->back_sel])) {
                /* straight back up or down: to the item focus left this row from, so a trip
                 * through rows of wider or narrower items (cards, pills) does not shift it */
                t->sel = t->back_sel;
            } else {
                /* otherwise keep the column under the focus */
                float fx = c->n ? c->items[c->sel].ax - c->scroll + item_w(c, &c->items[c->sel]) * 0.5f : 0;
                int best = t->sel; float bd = 1e9f;
                for (int i = 0; i < t->n; i++) {
                    if (skipped(&t->items[i])) continue;
                    float cxx = t->items[i].ax - t->scroll + item_w(t, &t->items[i]) * 0.5f;
                    if (fabsf(cxx - fx) < bd) { bd = fabsf(cxx - fx); best = i; }
                }
                t->sel = best;
            }
            cur_row = nr;
        }
    }
    Row *r = &rows[cur_row];
    if (dx && r->n) {
        int ns = r->sel + dx;
        while (ns >= 0 && ns < r->n && skipped(&r->items[ns])) ns += dx;
        if (ns >= 0 && ns < r->n) {
            r->sel = ns; /* a new column: the other rows pick by position again */
            for (int ri = 0; ri <= LAST_PAGE_ROW; ri++) rows[ri].back_ok = 0;
        }
    }
}

static Item *pointer_hit(float x, float y, int *ri_out, int *i_out) {
    for (int ri = 0; ri <= LAST_PAGE_ROW; ri++) {
        Row *r = &rows[ri];
        float ry = r->y - page, h = row_h(r);
        if (y < ry - 8 || y > ry + h + 8) continue;
        for (int i = 0; i < r->n; i++) {
            if (skipped(&r->items[i])) continue;
            float ix = r->items[i].ax - r->scroll;
            if (x >= ix && x <= ix + item_w(r, &r->items[i])) { *ri_out = ri; *i_out = i; return &r->items[i]; }
        }
    }
    return NULL;
}

/* the pointer over the Recents panel: 0 a card (*i), 1 the focused card's trash, 2 Close All,
 * -1 the panel but nothing on it, -2 above the panel */
static int recents_hit(float x, float y, int *i) {
    if (y < rec_py) return -2;
    if (in_rect(rec_close_r, x, y)) return 2;
    if (in_rect(rec_trash_r, x, y)) return 1;
    Row *r = &rows[ROW_RECENTS];
    float cy = rec_py + 180;
    for (int k = 0; k < r->n; k++) {
        if (r->items[k].dying) continue;
        float cx = r->items[k].ax - r->scroll + 168 * slot_of(&r->items[k]);
        if (x >= cx - 168 && x <= cx + 168 && y >= cy - 14 && y <= cy + 189 + 60) { *i = k; return 0; }
    }
    return -1;
}

/* hovering in Recents moves the focus there, as on the page */
static void recents_pointer(float x, float y) {
    int i = 0, part = recents_hit(x, y, &i);
    if (part == 0) { rows[ROW_RECENTS].sel = i; rec_part = 0; }
    else if (part == 1 || part == 2) rec_part = part;
}

/* the pointer in edit mode: 1 the eye, 2 the trash, 3 a tile (*i), 4 Delete, 5 Cancel, 0 nothing */
static int edit_hit(float x, float y, int *i) {
    if (edit_mode == ED_CONFIRM) return in_rect(ed_conf_r[0], x, y) ? 4 : in_rect(ed_conf_r[1], x, y) ? 5 : 0;
    if (in_rect(ed_eye_r, x, y)) return 1;
    if (in_rect(ed_trash_r, x, y)) return 2;
    int ri;
    return pointer_hit(x, y, &ri, i) && ri == ROW_APPS ? 3 : 0;
}
/* hovering moves the focus, as the arrows would; a carried tile goes only with the arrows */
static void edit_pointer(float x, float y) {
    int i = 0, part = edit_hit(x, y, &i);
    if (part == 4 || part == 5) confirm_delete = part == 4;
    else if (edit_mode == ED_MOVE || edit_mode == ED_CONFIRM) return;
    else if (part == 1) edit_mode = ED_HIDE;
    else if (part == 2) edit_mode = ED_TRASH;
    else if (part == 3) { rows[ROW_APPS].sel = i; edit_mode = ED_BROWSE; }
}

static void pointer_motion(float x, float y, float dx, float dy) {
    if (dpad_mode) {
        motion_since_dpad += fabsf(dx) + fabsf(dy);
        if (motion_since_dpad < DPAD_MOTION_GATE) return;
        dpad_mode = 0;
        if (cursor_visibility) cursor_visibility(SDL_TRUE);
    }
    if (recents_open) { if (!closing_all) recents_pointer(x, y); return; }
    if (edit_mode) { edit_pointer(x, y); return; }
    int ri, i;
    if (pointer_hit(x, y, &ri, &i) && (ri != cur_row || i != rows[ri].sel)) {
        cur_row = ri; rows[ri].sel = i;
        for (int k = 0; k <= LAST_PAGE_ROW; k++) rows[k].back_ok = 0;
    }
}

/* OK (key or click): a short press launches on release, a long press on an app starts moving it */
static int ok_down, ok_long, ok_is_click;
static double ok_down_ms;
static float ok_x, ok_y;

static void ok_press(int is_click, float x, float y) {
    if (is_click && dpad_mode) return; /* the pointer is hidden: OK arrives as a key */
    ok_down = 1; ok_long = 0; ok_is_click = is_click; ok_down_ms = now_ms(); ok_x = x; ok_y = y;
}

static void ok_poll(void) {
    if (!ok_down || ok_long || now_ms() - ok_down_ms < LONG_PRESS_MS) return;
    ok_long = 1;
    if (edit_mode || recents_open) return;
    int ri = cur_row, i = rows[cur_row].sel;
    if (ok_is_click && !pointer_hit(ok_x, ok_y, &ri, &i)) return;
    if (ri != ROW_APPS || !rows[ri].n) return;
    cur_row = ri; rows[ri].sel = i;
    edit_mode = ED_MOVE; move_from = i; /* a long press picks the tile straight up, as on LG's */
    logf_("edit: moving %s", rows[ri].items[i].id);
}

static int is_back(const SDL_Event *ev) {
    /* the remote's Back arrives from LG's SDL as scancode 482 with no keysym */
    return ev->key.keysym.sym == SDLK_ESCAPE || ev->key.keysym.scancode == SDL_SCANCODE_AC_BACK || ev->key.keysym.scancode == 482;
}
/* (a held Back never reaches an app: LG's surface manager keeps it, and closes the app in front;
 * Home presses never reach it while Home is in front) */
static void back_short(void) {
    if (recents_open) recents_key(0, 0, 0, 1);
    else if (edit_key(0, 0, 0, 1)) {}
    else if (cur_row > ROW_APPS) { cur_row = ROW_APPS; enter_dpad_mode(); } /* back up to the apps */
    else { enter_dpad_mode(); recents_show(1); } /* LG: Back on the home screen opens Recents */
}
/* Ask the power service, as the TV's own idle timer does: it enters its "Screen Saver" state,
 * shows LG's screen saver, and any key wakes the TV. (Launching the screen saver app directly
 * leaves the state "Active", and then only a few keys close it.) */
static void on_saver(cJSON *root, const char *raw) {
    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "returnValue"))) return;
    logf_("screen saver: %.200s; launching the app instead", raw ? raw : "");
    call("luna://com.webos.applicationManager/launch", "{\"id\":\"com.webos.app.screensaver\"}", on_launched);
}
static void screensaver(void) {
    logf_("screen saver");
    edit_exit(); recents_show(0);
    call("luna://com.webos.service.tvpower/power/turnOnScreenSaver", "{}", on_saver);
}
/* The blue button held: LG's screen saver. The remote sends blue as one press and one release
 * (no repeats); LG's SDL gives it scancode 489. It starts on the release of a hold past
 * LONG_PRESS_MS, so the release can't wake the screen saver straight away. */
#define SCAN_BLUE 489
static double blue_down_ms;
static void blue_press(void) { blue_down_ms = now_ms(); }
static void blue_release(void) {
    double held = blue_down_ms > 0 ? now_ms() - blue_down_ms : 0;
    blue_down_ms = 0;
    logf_("blue: held %.0f ms", held);
    if (held >= LONG_PRESS_MS) screensaver();
}

static void ok_release(int is_click, float x, float y) {
    if (!ok_down) return;
    ok_down = 0;
    if (ok_long) return;
    if (recents_open) {
        if (!is_click) { recents_key(0, 0, 1, 0); return; }
        /* a click: what is under the pointer, or a click above the panel closes it */
        int i = 0, part = recents_hit(x, y, &i);
        if (part == -2) recents_key(0, 0, 0, 1);
        else if (part >= 0) { if (part == 0) rows[ROW_RECENTS].sel = i; rec_part = part; recents_key(0, 0, 1, 0); }
        return;
    }
    if (edit_mode && is_click) {
        /* a click acts on what is under the pointer: a tile picks it up (or puts the carried one
         * down), the eye and trash as OK on them; in the dialog, outside its buttons, cancels */
        int i = 0, part = edit_hit(x, y, &i);
        if (edit_mode == ED_CONFIRM) { confirm_delete = part == 4; if (part) edit_key(0, 0, 1, 0); else edit_key(0, 0, 0, 1); return; }
        if (part == 0 || (edit_mode == ED_MOVE && (part != 3 || i != rows[ROW_APPS].sel))) return;
        edit_pointer(x, y);
        edit_key(0, 0, 1, 0);
        return;
    }
    if (edit_mode) { edit_key(0, 0, 1, 0); return; }
    if (is_click) {
        int ri, i;
        Item *it = pointer_hit(x, y, &ri, &i);
        if (it) { cur_row = ri; rows[ri].sel = i; launch(it); }
    } else {
        Row *r = &rows[cur_row];
        if (r->n && !skipped(&r->items[r->sel])) launch(&r->items[r->sel]);
    }
}

static int prof_gpu; /* dev: glFinish after render, so the GPU's share of a frame can be measured */
/* dev only: /tmp/nh-cmd holds key names ("down down ok"), so a check over ssh needs no remote.
 * Anyone can write /tmp, and these keys can delete an app, so as Home it is off unless root
 * created /var/lib/homenative/dev. */
static int dev_enabled;
static void dev_commands(void) {
    static double last;
    if (!dev_enabled) return;
    if (now_ms() - last < 200) return;
    last = now_ms();
    /* act once per write: the app may not delete root's file (sticky /tmp), so watch its mtime */
    static struct timespec seen;
    struct stat st;
    if (stat("/tmp/nh-cmd", &st) || (st.st_mtim.tv_sec == seen.tv_sec && st.st_mtim.tv_nsec == seen.tv_nsec)) return;
    int first = seen.tv_sec == 0 && seen.tv_nsec == 0;
    seen = st.st_mtim;
    if (first && time(NULL) - st.st_mtim.tv_sec > 2) return; /* a stale file from an earlier run */
    FILE *f = fopen("/tmp/nh-cmd", "r");
    if (!f) return;
    char w[32];
    float px, py;
    last_input_ms = now_ms(); dim_f = 0;
    hint_input();
    while (fscanf(f, "%31s", w) == 1) {
        if (!strcmp(w, "left")) move(-1, 0);
        else if (!strcmp(w, "right")) move(1, 0);
        else if (!strcmp(w, "up")) move(0, -1);
        else if (!strcmp(w, "down")) move(0, 1);
        else if (!strcmp(w, "where")) {
            Row *r = recents_open ? &rows[ROW_RECENTS] : &rows[cur_row];
            if (recents_open) logf_("where: recents item %d part %d %s", r->sel, rec_part, r->n ? r->items[r->sel].id : "-");
            else logf_("where: row %d item %d %s edit %d hidden %d eye %.0f,%.0f trash %.0f,%.0f", cur_row, r->sel, r->n ? r->items[r->sel].id : "-",
                       edit_mode, r->n ? r->items[r->sel].hidden : 0, ed_eye_r[0] + 30, ed_eye_r[1] + 30, ed_trash_r[0] + 30, ed_trash_r[1] + 30);
        }
        else if (sscanf(w, "point:%f,%f", &px, &py) == 2) { dpad_mode = 0; pointer_motion(px, py, 0, 0); }
        else if (sscanf(w, "click:%f,%f", &px, &py) == 2) { dpad_mode = 0; ok_press(1, px, py); ok_release(1, px, py); }
        else if (!strcmp(w, "ok")) { ok_press(0, 0, 0); ok_release(0, 0, 0); }
        else if (!strcmp(w, "back")) { if (!edit_key(0, 0, 0, 1)) cur_row = ROW_APPS; }
        else if (!strcmp(w, "saver")) screensaver();
        else if (!strcmp(w, "idle")) last_input_ms = now_ms() - IDLE_DIM_MS; /* dim now */
        /* edit states set directly, so a scripted check never sends OK (which could launch) */
        else if (!strcmp(w, "edit")) { cur_row = ROW_APPS; edit_mode = ED_BROWSE; }
        else if (!strcmp(w, "trash")) { cur_row = ROW_APPS; edit_mode = ED_TRASH; }
        else if (!strcmp(w, "hide")) { cur_row = ROW_APPS; edit_mode = ED_HIDE; }
        else if (!strcmp(w, "confirm")) { cur_row = ROW_APPS; edit_mode = ED_CONFIRM; confirm_delete = 0; }
        else if (!strcmp(w, "pick")) { cur_row = ROW_APPS; edit_mode = ED_MOVE; move_from = rows[ROW_APPS].sel; }
        else if (!strcmp(w, "noedit")) { edit_mode = ED_OFF; } /* leaves without telling SAM */
        else if (!strcmp(w, "recents")) recents_show(1);
        else if (!strcmp(w, "norecents")) recents_show(0);
        else if (!strcmp(w, "rtrash")) rec_part = 1;
        else if (!strcmp(w, "sweep")) tcache_sweep(1);
        else if (!strcmp(w, "prof")) { prof_gpu = !prof_gpu; logf_("prof: glFinish timing %s", prof_gpu ? "on" : "off"); }
        dirty = 1;
    }
    fclose(f);
}

/* ---------- safety net ----------
 * This binary is the TV's Home: if it can't run, the TV has none, and nobody watching it has a
 * shell. Each start as Home is counted in /tmp (a reboot starts over); a run past STABLE_MS clears
 * the count. Reaching START_LIMIT means it keeps dying, so it steps aside: the bind mounts go, and
 * this process becomes LG's Home in place. FAILED_PATH, holding the binary's md5, keeps the boot
 * hook from mounting that same build again; a new build gets a new chance. A clean exit (a plain
 * kill arrives as SDL_QUIT) clears the count too: only dying counts. A hang gets no such
 * luck by itself, hence the watchdog: the main loop re-arms an alarm, and if it stops doing so
 * for WATCHDOG_S the process dies and SAM restarts it, which counts as a start like any other. */
#define STARTS_PATH "/tmp/homenative-starts"
#define FAILED_PATH "/var/lib/homenative/failed"
#define START_LIMIT 4
#define STABLE_MS 600000.0
#define WATCHDOG_S 90
#define HOME_BIN "/usr/bin/com.webos.app.home"
#define HOME_CKPT "/mnt/lg/cmn_data/checkpoint/com.webos.app.home"

static void safety_check(char **argv) {
    if (!as_home || dev_enabled) return;
    int n = 0;
    FILE *f = fopen(STARTS_PATH, "r");
    if (f) { if (fscanf(f, "%d", &n) != 1) n = 0; fclose(f); }
    if (++n < START_LIMIT) {
        if ((f = fopen(STARTS_PATH, "w"))) { fprintf(f, "%d\n", n); fclose(f); }
        return;
    }
    logf_("safety: %d starts with no stable run: stepping aside for LG's Home", n);
    remove(STARTS_PATH);
    mkdir("/var/lib/homenative", 0755);
    /* the md5 of what is running, which an upgrade may already have replaced on disk; then the
     * same lifting of the mounts as scripts/restore.sh */
    char sh[400];
    snprintf(sh, sizeof sh, "md5sum < /proc/%d/exe > " FAILED_PATH "; umount -l " HOME_BIN "; "
             "while grep ' " HOME_CKPT " ' /proc/mounts | tail -1 | grep -q '^tmpfs'; do umount -l " HOME_CKPT " || break; done", (int)getpid());
    if (system(sh)) {}
    execv(HOME_BIN, argv);
    logf_("safety: cannot start LG's Home: %s", strerror(errno));
}
static void safety_tick(void) {
    static double armed = -1e9;
    static int stable;
    if (!as_home) return;
    if (now_ms() - armed > 5000) { armed = now_ms(); alarm(WATCHDOG_S); }
    if (!stable && now_ms() > STABLE_MS) { stable = 1; remove(STARTS_PATH); logf_("safety: stable"); }
}

/* ---------- waiting ----------
 * Hidden, the loop sleeps until there is something to do. LG's SDL can't: its
 * SDL_WaitEventTimeout polls every millisecond, all day, under whatever app is in front. GLib can:
 * it sleeps in poll() on the luna socket (where the Home key arrives, so that wakes it at once),
 * and on the Wayland socket too (the remote, window changes), with a timeout for the clock.
 * If the socket stays readable with nothing for SDL to read (another thread reading it), the
 * sleep would become a busy loop: spotted here, it is dropped for the plain wait. */
static guint wake_timer, wl_source;
static gboolean wake_up(gpointer unused) { (void)unused; wake_timer = 0; return G_SOURCE_REMOVE; }
static gboolean on_wayland(gint fd, GIOCondition c, gpointer unused) {
    (void)fd; (void)unused;
    if (c & (G_IO_HUP | G_IO_ERR)) {
        /* the compositor went away: a window never comes back on this connection, and SAM
         * restarts Home. The dev app just stops listening. */
        logf_("Wayland connection lost");
        if (as_home) _exit(1);
        wl_source = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}
static void watch_wayland(SDL_Window *win) {
    SDL_SysWMinfo info;
    memset(&info, 0, sizeof info);
    SDL_GetVersion(&info.version); /* the TV's SDL, not the NDK's, which it would turn down */
    int (*get_fd)(struct wl_display *) = (int (*)(struct wl_display *))dlsym(RTLD_DEFAULT, "wl_display_get_fd");
    if (get_fd && SDL_GetWindowWMInfo(win, &info) && info.subsystem == SDL_SYSWM_WAYLAND)
        wl_source = g_unix_fd_add(get_fd(info.info.wl.display), G_IO_IN, on_wayland, NULL);
    logf_("sleeping on %s", wl_source ? "the Wayland socket" : "a timer");
}
static void sleep_ms(int ms) {
    static int spins;
    double a = now_ms();
    wake_timer = g_timeout_add((guint)ms, wake_up, NULL);
    g_main_context_iteration(NULL, TRUE);
    if (wake_timer) { g_source_remove(wake_timer); wake_timer = 0; }
    if (now_ms() - a >= 0.2) spins = 0;
    else if (++spins > 2000 && wl_source) { logf_("Wayland socket stays readable: back to a timer"); g_source_remove(wl_source); wl_source = 0; }
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    rename(LOG_PATH, LOG_PREV);
    if (!freopen(log_path, "w", stderr) && freopen(DEV_LOG_PATH, "w", stderr)) log_path = DEV_LOG_PATH;
    /* (no log at all is no reason not to run) */
    fatal_handlers();
    t0 = SDL_GetPerformanceCounter();
    logf_("start argv1=%.300s", argc > 1 ? argv[1] : "");

    ssize_t n = readlink("/proc/self/exe", app_dir, sizeof app_dir - 1);
    if (n > 0) { app_dir[n] = 0; char *sl = strrchr(app_dir, '/'); if (sl) *sl = 0; } else scpy(app_dir, sizeof app_dir, ".");
    /* SAM passes the launch as JSON; as Home its appId is com.webos.app.home */
    cJSON *launch_args = argc > 1 ? cJSON_Parse(argv[1]) : NULL;
    as_home = !strcmp(jstr(launch_args, "appId"), "com.webos.app.home");
    cJSON_Delete(launch_args);
    verbose = !as_home || access("/var/lib/homenative/verbose", F_OK) == 0;
    dev_enabled = !as_home || access("/var/lib/homenative/dev", F_OK) == 0;
    /* as Home the executable lives in /usr/bin: the wallpaper and icon copies stay in the dev app's folder */
    char probe[PATH_MAX + 32]; snprintf(probe, sizeof probe, "%s/assets/wallpaper.png", app_dir);
    if (access(probe, R_OK)) scpy(app_dir, sizeof app_dir, DEV_DIR);
    logf_("running as %s, assets in %s", as_home ? "Home (com.webos.app.home)" : "dev app", app_dir);
    safety_check(argv);
    safety_tick(); /* armed from here on: a window that never comes up is a hang too */

    setenv("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true", 1);
    /* LG's SDL names the window's app from APPID; SAM sets it for dev apps, not for Home */
    if (as_home) setenv("APPID", "com.webos.app.home", 1);
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { logf_("SDL_Init: %s", SDL_GetError()); return 1; }
    /* LG's own extension, looked up at run time so a missing one is not a load failure */
    cursor_visibility = (SDL_bool (*)(SDL_bool))dlsym(RTLD_DEFAULT, "SDL_webOSCursorVisibility");
    logf_("SDL_webOSCursorVisibility %s", cursor_visibility ? "found" : "MISSING");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0); SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_Window *win = SDL_CreateWindow(APP_ID, 0, 0, (int)W, (int)H, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!win) { logf_("SDL_CreateWindow: %s", SDL_GetError()); return 1; }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { logf_("GL context: %s", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
    int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
    logf_("window %dx%d drawable %dx%d GL %s / %s", ww, wh, dw, dh, glGetString(GL_VERSION), glGetString(GL_RENDERER));
    gl_init();
    watch_wayland(win);

    TTF_Init();
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    const char *fl = "/usr/share/fonts/LG_Smart_UI-Light.ttf";
    f_clock = TTF_OpenFont(fl, 96); f_date = TTF_OpenFont(fl, 30); /* larger and lighter than LG's 72 / Regular 28 */
    if (!f_clock || !font(F_PILL)) logf_("font: %s", TTF_GetError());

    /* five 60 px buttons, 20 apart, ending 38 px from the right edge, as LG places them */
    rows[ROW_TOP] = (Row){.kind = K_TOP, .y = 22, .gap = 20, .x0 = W - 38 - (5 * 60 + 4 * 20)};
    rows[ROW_INPUTS] = (Row){.kind = K_PILL, .y = 790, .gap = 14, .x0 = 36};
    rows[ROW_APPS] = (Row){.kind = K_APP, .y = 900, .gap = 18, .x0 = 36};
    rows[ROW_DASH] = (Row){.kind = K_WHITE, .gap = 20, .x0 = 36};
    rows[ROW_CHANNELS] = (Row){.kind = K_CHANNEL, .gap = 20, .x0 = 36};
    rows[ROW_RECENTS] = (Row){.kind = K_RECENT, .gap = 32, .x0 = 48};

    decoder_start();
    hidden_load();
    if (access(HINTED_PATH, F_OK) == 0) hint_state = HINT_DONE;
    build_top();
    luna_init();
    start_subscriptions();
    refresh();

    /* first frame before anything heavy, so the window appears at once */
    update(0); render(); SDL_GL_SwapWindow(win);
    logf_("first frame");
    /* your own wallpaper, when there is one, over the one the app ships */
    char wp[PATH_MAX + 32] = "";
    static const char *const own[] = {"/var/lib/homenative/wallpaper.png", "/var/lib/homenative/wallpaper.jpg"};
    for (unsigned i = 0; i < 2 && !wp[0]; i++) if (access(own[i], R_OK) == 0) scpy(wp, sizeof wp, own[i]);
    snprintf(wallpaper_shipped, sizeof wallpaper_shipped, "%s/assets/wallpaper.png", app_dir);
    if (!wp[0]) scpy(wp, sizeof wp, wallpaper_shipped);
    logf_("wallpaper: %s", wp);
    decoder_push("@wallpaper", wp, 1, 0);
    dirty = 1;

    /* fps is measured over back-to-back rendered frames only, so idle time doesn't dilute it */
    int running = 1, frames = 0, rendered_last = 0;
    double last = now_ms(), anim_ms = 0, worst_ms = 0, last_swap = 0;
    float sx = W / (float)ww, sy = H / (float)wh;
    /* where an animated frame's time goes: luna callbacks, update, building the draws, GPU, swap */
    double p_glib = 0, p_upd = 0, p_draw = 0, p_gpu = 0, p_swap = 0, p_worst_part = 0, p_cpu = 0;
    int p_janks = 0;
    SDL_DisplayMode dm;
    double vsync_ms = 1000.0 / 60;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.refresh_rate > 0) vsync_ms = 1000.0 / dm.refresh_rate;
    logf_("display %dx%d @ %d Hz", dm.w, dm.h, dm.refresh_rate);
    const char *p_worst_name = "";
    while (running) {
        double pg0 = now_ms();
        for (int k = 0; k < 16 && g_main_context_iteration(NULL, FALSE); k++) {}
        double t = now_ms(), d_glib = t - pg0;
        float dt = (float)((t - last) / 1000.0); last = t;
        if (dt > 0.05f) dt = 0.05f;
        rows[ROW_CHANNELS].label = row_live(&rows[ROW_CHANNELS]) ? S(S_CHANNELS) : NULL;
        ok_poll();
        dev_commands();
        safety_tick();
        if (relaunch_pending) { /* Home pressed: come to the front on the home screen, as LG's does */
            relaunch_pending = 0;
            edit_exit(); recents_show(0); ok_down = 0;
            cur_row = ROW_APPS;
            if (!on_screen) { page = page_target = 0; recents_t = 0; } /* already there, not sliding back to it */
            SDL_RaiseWindow(win);
            on_screen = 1; dirty = 1; last_input_ms = now_ms(); dim_f = 0;
        }
        double pu0 = now_ms();
        int moving = update(dt) || ok_down;
        double d_upd = now_ms() - pu0;
        SDL_Event ev;
        /* animating and on screen: don't wait. On screen and still, SDL's own wait: someone is
         * there, the cost is small, and remote input goes through it. Hidden, which is most of the
         * day, a real sleep: see "waiting" above. Work still going on (images decoding, a preload
         * filling the page) only pauses for a frame. */
        int have = SDL_PollEvent(&ev);
        if (!have && !(on_screen && (moving || dirty))) {
            if (on_screen) have = SDL_WaitEventTimeout(&ev, 100);
            else { sleep_ms(moving || npend ? 16 : wl_source && !dev_enabled ? 1000 : 100); have = SDL_PollEvent(&ev); }
        }
        while (have) {
            switch (ev.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_KEYDOWN: {
                SDL_Keycode k = ev.key.keysym.sym;
                last_input_ms = now_ms();
                hint_input();
                /* blue only ever sends one press: it counts even when it is the one that wakes Home */
                if (ev.key.keysym.scancode == SCAN_BLUE && !ev.key.repeat) blue_press();
                /* dimmed: the first key only wakes Home */
                if (dim_f > 0.3f) { dirty = 1; break; }
                if (k == SDLK_LEFT) move(-1, 0);
                else if (k == SDLK_RIGHT) move(1, 0);
                else if (k == SDLK_UP) move(0, -1);
                else if (k == SDLK_DOWN) move(0, 1);
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { if (!ev.key.repeat) ok_press(0, 0, 0); }
                else if (is_back(&ev)) back_short();
                else if (ev.key.keysym.scancode == SCAN_BLUE) {}
                else logf_("key sym=%d scan=%d", k, ev.key.keysym.scancode);
                dirty = 1; break;
            }
            case SDL_KEYUP:
                last_input_ms = now_ms();
                if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER) ok_release(0, 0, 0);
                else if (ev.key.keysym.scancode == SCAN_BLUE) blue_release();
                dirty = 1; break;
            case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
                last_input_ms = now_ms();
                dirty = 1;
                if (dim_f > 0.3f && ev.type != SDL_MOUSEMOTION) break; /* dimmed: a click only wakes Home */
                if (ev.type == SDL_MOUSEMOTION) pointer_motion(ev.motion.x * sx, ev.motion.y * sy, ev.motion.xrel * sx, ev.motion.yrel * sy);
                else if (ev.type == SDL_MOUSEBUTTONDOWN) { if (ev.button.button == SDL_BUTTON_LEFT) ok_press(1, ev.button.x * sx, ev.button.y * sy); }
                else if (ev.type == SDL_MOUSEBUTTONUP) { if (ev.button.button == SDL_BUTTON_LEFT) ok_release(1, ev.button.x * sx, ev.button.y * sy); }
                else move(0, ev.wheel.y > 0 ? -1 : 1);
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_HIDDEN || ev.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    /* leaving: edit mode and Recents end, as on LG's home */
                    on_screen = 0; logf_("hidden"); edit_exit(); recents_show(0); ok_down = 0;
                }
                if (ev.window.event == SDL_WINDOWEVENT_SHOWN || ev.window.event == SDL_WINDOWEVENT_RESTORED || ev.window.event == SDL_WINDOWEVENT_EXPOSED) { on_screen = 1; dirty = 1; last_input_ms = now_ms(); dim_f = 0; }
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    logf_("focus gained"); edit_exit(); recents_show(0); ok_down = 0; refresh(); dirty = 1;
                }
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { edit_exit(); recents_show(0); }
                break;
            }
            have = SDL_PollEvent(&ev);
        }
        if (!on_screen) continue;
        if (moving || dirty) {
            double r0 = now_ms(), c0 = cpu_ms();
            render();
            double r1 = now_ms();
            p_cpu += cpu_ms() - c0;
            if (prof_gpu) glFinish();
            double r2 = now_ms();
            SDL_GL_SwapWindow(win); dirty = 0;
            double s = now_ms();
            if (rendered_last) {
                double gap = s - last_swap;
                anim_ms += gap; frames++;
                p_glib += d_glib; p_upd += d_upd; p_draw += r1 - r0; p_gpu += r2 - r1; p_swap += s - r2;
                if (gap > vsync_ms * 1.5) p_janks++; /* a refresh missed */
                if (gap > worst_ms) {
                    worst_ms = gap;
                    /* name the part that made the worst frame worst */
                    double parts[5] = {d_glib, d_upd, r1 - r0, r2 - r1, s - r2};
                    static const char *names[5] = {"luna", "update", "draw", "gpu", "swap"};
                    p_worst_part = 0;
                    for (int q = 0; q < 5; q++) if (parts[q] > p_worst_part) { p_worst_part = parts[q]; p_worst_name = names[q]; }
                }
            }
            last_swap = s; rendered_last = 1;
            if (anim_ms > 3000) {
                vlogf_("fps %.1f over %d frames, %d missed refreshes, worst %.1fms (%s %.1f) | avg ms: luna %.2f update %.2f draw %.2f (cpu %.2f) gpu %.2f swap %.2f | %d draw calls",
                      frames * 1000.0 / anim_ms, frames, p_janks, worst_ms, p_worst_name, p_worst_part,
                      p_glib / frames, p_upd / frames, p_draw / frames, p_cpu / frames, p_gpu / frames, p_swap / frames, draw_calls);
                p_cpu = 0; p_janks = 0;
                anim_ms = 0; frames = 0; worst_ms = 0;
                p_glib = p_upd = p_draw = p_gpu = p_swap = 0;
            }
        } else rendered_last = 0;
    }
    logf_("exit");
    if (as_home) remove(STARTS_PATH);
    SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
