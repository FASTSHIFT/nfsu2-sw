/* perf_hud.c -- see perf_hud.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "perf_hud.h"

#if defined(_WIN32) || defined(__SWITCH__)
void perf_hud_update(const PerfFrame *f) { (void)f; }
void perf_hud_draw(int w, int h, int t, float ax, float ay) { (void)w; (void)h; (void)t; (void)ax; (void)ay; }
int perf_hud_enabled(void) { return 0; }
#else

#include "gl_api.h"
#include "platform/knobs.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── telemetry (the /proc and /sys files stay open; lseek+read per sample) ── */

typedef unsigned long long ull;
static struct {
    int fd_stat, fd_statm, fd_mem, fd_temp, fd_arm, fd_gpu;
    ull prev_idle, prev_total;
    int tick, arm_mhz, gpu_mhz, temp_mc, rss_mb, mem_pct, cpu_pct;
    int ready;
} T;

static int slurp(int fd, char *buf, int cap)
{
    int n;
    if (fd < 0 || lseek(fd, 0, SEEK_SET) < 0)
        return -1;
    n = (int)read(fd, buf, (size_t)cap - 1);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return n;
}

static void tele_init(void)
{
    T.fd_stat  = open("/proc/stat", O_RDONLY | O_CLOEXEC);
    T.fd_statm = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    T.fd_mem   = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    T.fd_temp  = open("/sys/class/thermal/thermal_zone0/temp", O_RDONLY | O_CLOEXEC);
    T.fd_arm   = open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", O_RDONLY | O_CLOEXEC);
    T.fd_gpu   = open("/sys/class/devfreq/ff400000.gpu/cur_freq", O_RDONLY | O_CLOEXEC);
    if (T.fd_gpu < 0)
        T.fd_gpu = open("/sys/class/devfreq/gpu/cur_freq", O_RDONLY | O_CLOEXEC);
    T.temp_mc = T.rss_mb = T.mem_pct = T.cpu_pct = -1;
    T.ready = 1;
}

static void tele_sample(void)
{
    char buf[2048];
    ull u, n, s, id, io, ir, si, st;

    if (!T.ready)
        tele_init();
    if (slurp(T.fd_stat, buf, 256) > 0 &&
        sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &id, &io, &ir, &si, &st) == 8) {
        ull idle = id + io, total = u + n + s + id + io + ir + si + st;
        ull dt = total - T.prev_total, di = idle - T.prev_idle;
        T.cpu_pct = dt ? (int)(100 * (dt - di) / dt) : 0;
        T.prev_idle = idle;
        T.prev_total = total;
    }
    if ((T.tick++ % 60) != 0)               /* the rest about once a second */
        return;
    if (slurp(T.fd_arm, buf, 64) > 0) {
        int mhz = atoi(buf) / 1000;
        T.arm_mhz = (mhz >= 100 && mhz <= 3000) ? mhz : 0;
    }
    if (slurp(T.fd_gpu, buf, 64) > 0) {
        long hz = atol(buf);
        T.gpu_mhz = (hz >= 1000000L && hz <= 2000000000L) ? (int)(hz / 1000000) : 0;
    }
    if (slurp(T.fd_temp, buf, 32) > 0)
        T.temp_mc = atoi(buf);
    if (slurp(T.fd_statm, buf, 128) > 0) {
        long tot, rss;
        if (sscanf(buf, "%ld %ld", &tot, &rss) == 2)
            T.rss_mb = (int)((rss * sysconf(_SC_PAGESIZE)) / (1024 * 1024));
    }
    if (slurp(T.fd_mem, buf, sizeof buf) > 0) {
        long total = 0, avail = 0;
        const char *p = buf;
        while (*p) {
            if (!strncmp(p, "MemTotal:", 9)) total = atol(p + 9);
            else if (!strncmp(p, "MemAvailable:", 13)) { avail = atol(p + 13); break; }
            p = strchr(p, '\n');
            if (!p) break;
            p++;
        }
        if (total > 0)
            T.mem_pct = (int)(100 * (total - avail) / total);
    }
}

/* ── lines ──────────────────────────────────────────────────────────── */

static char s_lines[10][48];
static int  s_nlines;
static int  s_on = -1, s_log = -1;

int perf_hud_enabled(void)
{
    if (s_on < 0) {
        const char *e = getenv("RECOMP_HUD");
        s_on = e && *e == '1';
    }
    return s_on || knob(KN_HUD) > 0;
}

void perf_hud_update(const PerfFrame *f)
{
    static double a_f, a_g, a_w, a_s, a_d, a_c, a_m, min_f = 1e9, max_f;
    static int n;
    int hud = perf_hud_enabled();

    if (s_log < 0) {
        const char *e = getenv("RECOMP_PERF_LOG");
        s_log = e && *e == '1';
    }
    if (!hud && !s_log)
        return;
    tele_sample();

    if (hud) {
        double fps = f->frame_ms > 0 ? 1000.0 / f->frame_ms : 0;
        s_nlines = 0;
        snprintf(s_lines[s_nlines++], sizeof s_lines[0], "FPS:%.0f %.1fMS", fps, f->frame_ms);
        snprintf(s_lines[s_nlines++], sizeof s_lines[0], "GL:%.1f WAIT:%.1f SWAP:%.1f", f->gl_ms, f->wait_ms, f->swap_ms);
        snprintf(s_lines[s_nlines++], sizeof s_lines[0], "DC:%u CALLS:%u", f->draws, f->gl_calls);
        if (f->merged)
            snprintf(s_lines[s_nlines++], sizeof s_lines[0], "MERGED:%u", f->merged);
        if (T.cpu_pct >= 0)
            snprintf(s_lines[s_nlines++], sizeof s_lines[0], "SYS:%d%%", T.cpu_pct);
        if (T.rss_mb >= 0 && T.mem_pct >= 0)
            snprintf(s_lines[s_nlines++], sizeof s_lines[0], "MEM:%dM %d%%", T.rss_mb, T.mem_pct);
        if (T.arm_mhz > 0 && T.gpu_mhz > 0)
            snprintf(s_lines[s_nlines++], sizeof s_lines[0], "A:%dM G:%dM", T.arm_mhz, T.gpu_mhz);
        else if (T.arm_mhz > 0)
            snprintf(s_lines[s_nlines++], sizeof s_lines[0], "A:%dM", T.arm_mhz);
        if (T.temp_mc >= 0)
            snprintf(s_lines[s_nlines++], sizeof s_lines[0], "T:%.1fC", T.temp_mc / 1000.0);
    }

    a_f += f->frame_ms; a_g += f->gl_ms; a_w += f->wait_ms; a_s += f->swap_ms;
    a_d += f->draws; a_c += f->gl_calls; a_m += f->merged;
    if (f->frame_ms > 0) {
        if (f->frame_ms < min_f) min_f = f->frame_ms;
        if (f->frame_ms > max_f) max_f = f->frame_ms;
    }
    if (++n >= 120) {
        if (s_log)
            fprintf(stderr, "[perf] frame=%.2fms(min %.1f max %.1f) fps=%.0f | gl=%.2f wait=%.2f swap=%.2f | "
                    "dc=%.0f calls=%.0f merged=%.0f | sys=%d%% mem=%dM/%d%% a=%dM g=%dM t=%.1fC\n",
                    a_f / n, min_f, max_f, a_f > 0 ? 1000.0 * n / a_f : 0.0, a_g / n, a_w / n, a_s / n,
                    a_d / n, a_c / n, a_m / n, T.cpu_pct, T.rss_mb, T.mem_pct, T.arm_mhz, T.gpu_mhz,
                    T.temp_mc >= 0 ? T.temp_mc / 1000.0 : 0.0);
        a_f = a_g = a_w = a_s = a_d = a_c = a_m = 0;
        min_f = 1e9;
        max_f = 0;
        n = 0;
    }
}

/* ── 5x7 font: digits, upper case, a few symbols; columns, bit 0 = top ── */

static const struct { char c; uint8_t col[5]; } FONT[] = {
    {'0',{0x3E,0x51,0x49,0x45,0x3E}}, {'1',{0x00,0x42,0x7F,0x40,0x00}}, {'2',{0x42,0x61,0x51,0x49,0x46}},
    {'3',{0x21,0x41,0x45,0x4B,0x31}}, {'4',{0x18,0x14,0x12,0x7F,0x10}}, {'5',{0x27,0x45,0x45,0x45,0x39}},
    {'6',{0x3C,0x4A,0x49,0x49,0x30}}, {'7',{0x01,0x71,0x09,0x05,0x03}}, {'8',{0x36,0x49,0x49,0x49,0x36}},
    {'9',{0x06,0x49,0x49,0x29,0x1E}}, {'A',{0x7E,0x11,0x11,0x11,0x7E}}, {'B',{0x7F,0x49,0x49,0x49,0x36}},
    {'C',{0x3E,0x41,0x41,0x41,0x22}}, {'D',{0x7F,0x41,0x41,0x22,0x1C}}, {'E',{0x7F,0x49,0x49,0x49,0x41}},
    {'F',{0x7F,0x09,0x09,0x09,0x01}}, {'G',{0x3E,0x41,0x49,0x49,0x7A}}, {'H',{0x7F,0x08,0x08,0x08,0x7F}},
    {'I',{0x00,0x41,0x7F,0x41,0x00}}, {'K',{0x7F,0x08,0x14,0x22,0x41}}, {'L',{0x7F,0x40,0x40,0x40,0x40}},
    {'M',{0x7F,0x02,0x0C,0x02,0x7F}}, {'N',{0x7F,0x04,0x08,0x10,0x7F}}, {'O',{0x3E,0x41,0x41,0x41,0x3E}},
    {'P',{0x7F,0x09,0x09,0x09,0x06}}, {'R',{0x7F,0x09,0x19,0x29,0x46}}, {'S',{0x46,0x49,0x49,0x49,0x31}},
    {'T',{0x01,0x01,0x7F,0x01,0x01}}, {'U',{0x3F,0x40,0x40,0x40,0x3F}}, {'W',{0x3F,0x40,0x38,0x40,0x3F}},
    {'Y',{0x07,0x08,0x70,0x08,0x07}}, {'.',{0x00,0x60,0x60,0x00,0x00}}, {':',{0x00,0x36,0x36,0x00,0x00}},
    {'%',{0x23,0x13,0x08,0x64,0x62}}, {'-',{0x08,0x08,0x08,0x08,0x08}},
};
#define FONT_N ((int)(sizeof FONT / sizeof FONT[0]))
#define ATLAS_W 256                       /* 6 px per glyph cell, 40 cells */
#define ATLAS_H 8

static GLuint s_tex, s_prog, s_vao, s_vbo;
static GLint  s_u_scr, s_u_col, s_u_yf;
static int    s_gl_ready;

static int glyph_cell(char c)
{
    int i;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (i = 0; i < FONT_N; i++)
        if (FONT[i].c == c) return i;
    return -1;
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[HUD] shader failed: %s\n", log);
        return 0;
    }
    return s;
}

static int gl_setup(void)
{
    const char *hdr = nv2a_gl_api_es ? "#version 300 es\nprecision mediump float;\n"
                                     : "#version 330 core\n";
    char vs[512], fs[512];
    uint8_t atlas[ATLAS_W * ATLAS_H];
    GLuint v, f;
    int i, x, y;

    snprintf(vs, sizeof vs,
        "%slayout(location=0) in vec2 p; layout(location=1) in vec2 t;\n"
        "uniform vec2 scr; uniform float yf; out vec2 uv;\n"
        "void main(){ uv=t; gl_Position=vec4(p.x/scr.x*2.0-1.0, yf*(p.y/scr.y*2.0-1.0), 0.0, 1.0); }\n", hdr);
    snprintf(fs, sizeof fs,
        "%suniform sampler2D tex; uniform vec4 col; in vec2 uv; out vec4 o;\n"
        "void main(){ float a=texture(tex,uv).r; o=vec4(col.rgb, col.a*a); }\n", hdr);
    v = compile(GL_VERTEX_SHADER, vs);
    f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f)
        return 0;
    s_prog = glCreateProgram();
    glAttachShader(s_prog, v);
    glAttachShader(s_prog, f);
    glLinkProgram(s_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    s_u_scr = glGetUniformLocation(s_prog, "scr");
    s_u_col = glGetUniformLocation(s_prog, "col");
    s_u_yf  = glGetUniformLocation(s_prog, "yf");

    memset(atlas, 0, sizeof atlas);
    for (i = 0; i < FONT_N; i++)
        for (x = 0; x < 5; x++)
            for (y = 0; y < 7; y++)
                if (FONT[i].col[x] & (1u << y))
                    atlas[y * ATLAS_W + i * 6 + x] = 255;
    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, nv2a_gl_api_es ? GL_R8 : GL_RED, ATLAS_W, ATLAS_H, 0,
                 GL_RED, GL_UNSIGNED_BYTE, atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);

    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 4096 * 6 * 4 * (GLsizeiptr)sizeof(float), NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (const void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (const void *)8);
    return 1;
}

/* Emit one glyph (or a shadow of it) as two triangles. */
static int quad(float *v, int n, float x, float y, float sx, float sy, int cell)
{
    float u0 = (float)(cell * 6) / ATLAS_W, u1 = (float)(cell * 6 + 5) / ATLAS_W;
    float v1 = 7.0f / ATLAS_H, w = 5 * sx, h = 7 * sy;
    float q[6][4] = {
        {x, y, u0, 0}, {x + w, y, u1, 0}, {x, y + h, u0, v1},
        {x + w, y, u1, 0}, {x + w, y + h, u1, v1}, {x, y + h, u0, v1},
    };
    memcpy(v + n * 4, q, sizeof q);
    return n + 6;
}

void perf_hud_draw(int w, int h, int top_first, float aa_x, float aa_y)
{
    float *verts, scale = 2.0f, sx, sy, line_h, top;
    int i, nv = 0, pass;
    const char *c;

    if (!perf_hud_enabled() || !s_nlines)
        return;
    if (!s_gl_ready) {
        s_gl_ready = gl_setup() ? 1 : -1;
        if (s_gl_ready < 0)
            fprintf(stderr, "[HUD] disabled: GL setup failed\n");
    }
    if (s_gl_ready < 0)
        return;

    verts = (float *)malloc(4096 * 6 * 4 * sizeof(float));
    if (!verts)
        return;
    sx = scale * (aa_x > 0 ? aa_x : 1.0f);
    sy = scale * (aa_y > 0 ? aa_y : 1.0f);
    line_h = 7 * sy + 3 * sy / scale;
    top = ((float)h - s_nlines * line_h) / 2.0f;     /* left, vertically centred */
    /* The caller bound the surface to draw into; do not touch the binding. */
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glDisable(GL_STENCIL_TEST);
    glBlendEquation(GL_FUNC_ADD);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(s_prog);
    glUniform2fv(s_u_scr, 1, (float[2]){ (float)w, (float)h });
    glUniform1f(s_u_yf, top_first ? 1.0f : -1.0f);   /* GL row 0 = image top? */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

    for (pass = 0; pass < 2; pass++) {                /* 0: shadow, 1: text */
        nv = 0;
        for (i = 0; i < s_nlines; i++) {
            float x = 4.0f * sx / scale + (pass ? 0 : 1.5f * sx / scale), y = top + i * line_h + (pass ? 0 : 1.5f * sy / scale);
            for (c = s_lines[i]; *c && nv < 4096 * 6 - 6; c++, x += 6 * sx) {
                int cell = glyph_cell(*c);
                if (cell >= 0)
                    nv = quad(verts, nv, x, y, sx, sy, cell);
            }
        }
        if (!nv)
            continue;
        glUniform4fv(s_u_col, 1, pass ? (float[4]){ 1, 1, 1, 1 } : (float[4]){ 0, 0, 0, 0.85f });
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)nv * 16, verts);
        glDrawArrays(GL_TRIANGLES, 0, nv);
    }
    glDisable(GL_BLEND);
    free(verts);
}

#endif /* !_WIN32 && !__SWITCH__ */
