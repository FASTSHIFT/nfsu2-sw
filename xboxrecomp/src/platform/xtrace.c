/*
 * xtrace.c -- see xtrace.h.
 *
 * Each thread records into its own fixed buffer of 16-byte events; the
 * writer (the signal thread, after recording stops) walks all buffers and
 * emits a Perfetto protobuf trace:
 *
 *   Trace { repeated TracePacket packet = 1; }
 *   TracePacket: timestamp 8, trusted_packet_sequence_id 10, track_event 11,
 *                interned_data 12, sequence_flags 13, track_descriptor 60
 *   TrackDescriptor: uuid 1, name 2, thread 4 {pid 1, tid 2, thread_name 5},
 *                    parent_uuid 5, counter 8 {}
 *   TrackEvent: name_iid 10, type 9 (1 begin, 2 end, 3 instant, 4 counter),
 *               track_uuid 11, counter_value 30
 *   InternedData: event_names 2 {iid 1, name 2}
 *
 * One packet sequence per thread (sequence id = thread index + 1), names
 * interned per sequence, timestamps absolute (CLOCK_MONOTONIC ns -- the
 * default trace clock is BOOTTIME, which on an awake device differs from it
 * only by a constant).
 */
#include "xtrace.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

volatile int g_xtrace_on;

enum { EV_BEGIN = 1, EV_END = 2, EV_INSTANT = 3, EV_COUNTER = 4 };

/* 16 bytes: the type in the top 3 bits of the timestamp (ns since start of
 * recording fits the rest for 9 years); the payload is the name pointer, or
 * for a counter its id (top 8 bits) and value (low 56, signed). */
typedef struct {
    uint64_t ts_type;
    uint64_t payload;
} XEv;
#define EV_TS(e)   ((e)->ts_type & 0x1FFFFFFFFFFFFFFFull)
#define EV_TYPE(e) ((uint32_t)((e)->ts_type >> 61))

#define XT_MAX_THREADS 64
#define XT_DEFAULT_EVENTS (1u << 18)    /* per thread: 256k x 16 B = 4 MB */

/* Counter names, process-wide (ids fit the payload's top byte). */
#define XT_COUNTERS 255
static const char *s_counter_name[XT_COUNTERS];
static int s_ncounters;

typedef struct {
    XEv *ev;
    uint32_t n, cap;
    uint64_t dropped;
    int tid;
    char name[32];
    int depth_at_start;         /* spans open when recording began */
} XThread;

static XThread s_threads[XT_MAX_THREADS];
static int s_nthreads;
static pthread_mutex_t s_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread XThread *t_xt;
static __thread char t_pending_name[32];

static char s_path[512];
static uint32_t s_cap = XT_DEFAULT_EVENTS;
static int s_secs = 10;
static int s_armed;
static uint64_t s_start_ns;
static int s_generation;                /* recording number, for file names */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static XThread *self(void)
{
    XThread *t = t_xt;
    if (t)
        return t;
    pthread_mutex_lock(&s_reg_lock);
    if (s_nthreads < XT_MAX_THREADS) {
        t = &s_threads[s_nthreads];
        t->ev = (XEv *)malloc((size_t)s_cap * sizeof(XEv));
        if (t->ev) {
            t->cap = s_cap;
            t->tid = (int)syscall(SYS_gettid);
            if (t_pending_name[0])
                memcpy(t->name, t_pending_name, sizeof t->name);
            else {
                FILE *f;
                char p[64];
                snprintf(p, sizeof p, "/proc/self/task/%d/comm", t->tid);
                f = fopen(p, "r");
                if (f) {
                    if (fgets(t->name, sizeof t->name, f))
                        t->name[strcspn(t->name, "\n")] = 0;
                    fclose(f);
                }
            }
            s_nthreads++;
        } else
            t = NULL;
    }
    pthread_mutex_unlock(&s_reg_lock);
    t_xt = t;
    return t;
}

static void put_at(uint32_t type, uint64_t payload, uint64_t ts)
{
    XThread *t = self();
    XEv *e;
    if (!t)
        return;
    if (t->n >= t->cap) {
        t->dropped++;
        return;
    }
    if (ts < s_start_ns)
        ts = s_start_ns;
    e = &t->ev[t->n++];
    e->ts_type = ((ts - s_start_ns) & 0x1FFFFFFFFFFFFFFFull) | ((uint64_t)type << 61);
    e->payload = payload;
}
static void put(uint32_t type, uint64_t payload) { put_at(type, payload, now_ns()); }

uint64_t xtrace_now(void) { return now_ns(); }

/* An END whose BEGIN came before recording started is dropped by the writer
 * (it pairs ENDs with the BEGINs it has seen). Ends close the innermost open
 * span; their tag is not checked. */
static uint32_t ev_type(char type)
{
    return type == 'B' ? EV_BEGIN : type == 'E' ? EV_END : EV_INSTANT;
}
void xtrace_write(const char *tag, char type)
{
    put(ev_type(type), type == 'E' ? 0 : (uint64_t)(uintptr_t)tag);
}
void xtrace_write_at(const char *tag, char type, uint64_t ts)
{
    put_at(ev_type(type), type == 'E' ? 0 : (uint64_t)(uintptr_t)tag, ts);
}

__thread int g_xtrace_draw_on;
__thread uint64_t g_xtrace_draw_t;
static uint32_t s_draw_every = XTRACE_DRAW_SAMPLE;     /* RECOMP_TRACE_DRAW */
void xtrace_draw_start(void)
{
    static __thread uint32_t n;
    g_xtrace_draw_on = (++n % s_draw_every) == 0;
    if (g_xtrace_draw_on)
        g_xtrace_draw_t = now_ns();
}

void xtrace_counter_write(const char *name, int64_t v)
{
    int c;
    for (c = 0; c < s_ncounters && s_counter_name[c] != name; c++) { }
    if (c == s_ncounters) {
        pthread_mutex_lock(&s_reg_lock);
        for (c = 0; c < s_ncounters && s_counter_name[c] != name; c++) { }
        if (c == s_ncounters) {
            if (s_ncounters >= XT_COUNTERS) { pthread_mutex_unlock(&s_reg_lock); return; }
            s_counter_name[c] = name;
            __atomic_store_n(&s_ncounters, c + 1, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&s_reg_lock);
    }
    put(EV_COUNTER, ((uint64_t)c << 56) | ((uint64_t)v & 0x00FFFFFFFFFFFFFFull));
}

void xtrace_thread_name(const char *name)
{
    XThread *t = t_xt;
    snprintf(t_pending_name, sizeof t_pending_name, "%s", name);
    if (t)
        snprintf(t->name, sizeof t->name, "%s", name);
}

/* ── protobuf writer ─────────────────────────────────────────────── */

typedef struct { uint8_t *p; size_t n, cap; } Buf;

static void b_need(Buf *b, size_t k)
{
    if (b->n + k > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + k) c *= 2;
        b->p = (uint8_t *)realloc(b->p, c);
        b->cap = c;
    }
}
static void b_varint(Buf *b, uint64_t v)
{
    b_need(b, 10);
    while (v >= 0x80) { b->p[b->n++] = (uint8_t)(v | 0x80); v >>= 7; }
    b->p[b->n++] = (uint8_t)v;
}
static void b_tag(Buf *b, uint32_t field, uint32_t wt) { b_varint(b, ((uint64_t)field << 3) | wt); }
static void b_uint(Buf *b, uint32_t field, uint64_t v) { b_tag(b, field, 0); b_varint(b, v); }
static void b_sint64(Buf *b, uint32_t field, int64_t v) { b_tag(b, field, 0); b_varint(b, (uint64_t)v); }
static void b_bytes(Buf *b, uint32_t field, const void *d, size_t n)
{
    b_tag(b, field, 2);
    b_varint(b, n);
    b_need(b, n);
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void b_str(Buf *b, uint32_t field, const char *s) { b_bytes(b, field, s, strlen(s)); }
static void b_msg(Buf *b, uint32_t field, Buf *m) { b_bytes(b, field, m->p, m->n); m->n = 0; }

/* One TracePacket into the output file. */
static void emit_packet(FILE *f, Buf *pkt)
{
    uint8_t hdr[11];
    size_t n = 0;
    uint64_t v = pkt->n;
    hdr[n++] = (1 << 3) | 2;            /* Trace.packet, length-delimited */
    while (v >= 0x80) { hdr[n++] = (uint8_t)(v | 0x80); v >>= 7; }
    hdr[n++] = (uint8_t)v;
    fwrite(hdr, 1, n, f);
    fwrite(pkt->p, 1, pkt->n, f);
    pkt->n = 0;
}

/* Interned names of one sequence: pointer -> iid. */
#define XT_NAMES 1024
typedef struct { const char *k[XT_NAMES]; int n; } NameTab;
static int name_iid(NameTab *t, const char *name, int *is_new)
{
    int i;
    for (i = 0; i < t->n; i++)
        if (t->k[i] == name) { *is_new = 0; return i + 1; }
    if (t->n >= XT_NAMES) { *is_new = 0; return 1; }
    t->k[t->n++] = name;
    *is_new = 1;
    return t->n;
}

static uint64_t thread_uuid(int i) { return 0x1000u + (uint64_t)i; }
static uint64_t counter_uuid(int k) { return 0x100000u + (uint64_t)k; }

static void write_trace(const char *path)
{
    FILE *f = fopen(path, "wb");
    Buf pkt = { 0 }, m1 = { 0 }, m2 = { 0 }, m3 = { 0 };
    int pid = (int)getpid(), i;
    uint64_t total = 0, dropped = 0;

    if (!f) {
        fprintf(stderr, "[TRACE] cannot write %s\n", path);
        return;
    }
    {
        /* The process track, so the threads group under one name. */
        b_uint(&m1, 1, pid);
        b_str(&m1, 6, "nfsu2_recomp");          /* ProcessDescriptor.process_name */
        b_uint(&pkt, 1, 1);                     /* TrackDescriptor.uuid */
        b_msg(&pkt, 3, &m1);                    /* TrackDescriptor.process */
        b_msg(&m2, 60, &pkt);
        emit_packet(f, &m2);
    }
    for (i = 0; i < s_ncounters; i++) {
        b_uint(&pkt, 1, counter_uuid(i));
        b_uint(&pkt, 5, 1);                     /* under the process */
        b_str(&pkt, 2, s_counter_name[i]);
        b_msg(&pkt, 8, &m1);                    /* CounterDescriptor {} */
        b_msg(&m2, 60, &pkt);
        emit_packet(f, &m2);
    }
    for (i = 0; i < s_nthreads; i++) {
        XThread *t = &s_threads[i];
        NameTab names;
        int depth = 0;
        uint32_t k, seq = (uint32_t)i + 1;
        if (!t->n)
            continue;
        names.n = 0;

        /* Thread track. */
        b_uint(&m1, 1, pid);
        b_uint(&m1, 2, (uint64_t)t->tid);
        b_str(&m1, 5, t->name[0] ? t->name : "thread");
        b_uint(&pkt, 1, thread_uuid(i));
        b_msg(&pkt, 4, &m1);
        b_msg(&m2, 60, &pkt);
        b_uint(&m2, 10, seq);
        b_uint(&m2, 13, 1);                     /* SEQ_INCREMENTAL_STATE_CLEARED */
        emit_packet(f, &m2);

        for (k = 0; k < t->n; k++) {
            const XEv *e = &t->ev[k];
            uint32_t type = EV_TYPE(e);
            uint64_t track = thread_uuid(i);
            const char *name = type == EV_COUNTER || type == EV_END
                             ? NULL : (const char *)(uintptr_t)e->payload;
            if (type == EV_END) {
                if (depth == 0)
                    continue;                   /* its begin was not recorded */
                depth--;
            } else if (type == EV_BEGIN)
                depth++;
            if (type == EV_COUNTER)
                track = counter_uuid((int)(e->payload >> 56));
            if (name) {
                int is_new, iid = name_iid(&names, name, &is_new);
                if (is_new) {
                    b_uint(&m3, 1, iid);
                    b_str(&m3, 2, name);
                    b_msg(&m1, 2, &m3);         /* InternedData.event_names */
                    b_msg(&pkt, 12, &m1);
                }
                b_uint(&m1, 10, iid);           /* TrackEvent.name_iid */
            }
            b_uint(&m1, 9, type);
            b_uint(&m1, 11, track);
            if (type == EV_COUNTER)                /* 56-bit value, sign-extended */
                b_sint64(&m1, 30, (int64_t)(e->payload << 8) >> 8);
            b_uint(&pkt, 8, s_start_ns + EV_TS(e));
            b_msg(&pkt, 11, &m1);
            b_uint(&pkt, 10, seq);
            b_uint(&pkt, 13, 2);                /* SEQ_NEEDS_INCREMENTAL_STATE */
            emit_packet(f, &pkt);
        }
        /* Close spans still open at the stop, at the last timestamp. */
        while (depth-- > 0) {
            b_uint(&m1, 9, EV_END);
            b_uint(&m1, 11, thread_uuid(i));
            b_uint(&pkt, 8, s_start_ns + EV_TS(&t->ev[t->n - 1]));
            b_msg(&pkt, 11, &m1);
            b_uint(&pkt, 10, seq);
            emit_packet(f, &pkt);
        }
        total += t->n;
        dropped += t->dropped;
    }
    fclose(f);
    free(pkt.p); free(m1.p); free(m2.p); free(m3.p);
    fprintf(stderr, "[TRACE] wrote %s: %llu events, %d threads, %llu dropped (buffer full)\n",
            path, (unsigned long long)total, s_nthreads, (unsigned long long)dropped);
}

/* ── control ─────────────────────────────────────────────────────── */

static void start(void)
{
    int i;
    pthread_mutex_lock(&s_reg_lock);
    for (i = 0; i < s_nthreads; i++) {
        s_threads[i].n = 0;
        s_threads[i].dropped = 0;
    }
    pthread_mutex_unlock(&s_reg_lock);
    s_start_ns = now_ns();
    __atomic_store_n(&g_xtrace_on, 1, __ATOMIC_RELEASE);
    fprintf(stderr, "[TRACE] recording%s\n", s_secs > 0 ? "" : " until the next SIGUSR2");
}

/* Each recording replaces <path>, written to <path>.tmp and renamed: whoever
 * waits for it (r36s/perfetto_bridge.py) sees the new file only when it is
 * complete. */
static void stop_and_write(void)
{
    char tmp[600];
    __atomic_store_n(&g_xtrace_on, 0, __ATOMIC_RELEASE);
    usleep(20000);                      /* let in-flight puts land */
    s_generation++;
    fprintf(stderr, "[TRACE] recording %d stopped after %.1f s\n", s_generation,
            (now_ns() - s_start_ns) / 1e9);
    snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
    write_trace(tmp);
    if (rename(tmp, s_path) != 0)
        fprintf(stderr, "[TRACE] cannot rename %s to %s\n", tmp, s_path);
}

static void *control_thread(void *arg)
{
    sigset_t *set = (sigset_t *)arg;
    xtrace_thread_name("xtrace");
    for (;;) {
        /* Polled every 200 ms: recording can also start from another thread
         * (RECOMP_TRACE_START), so the stop deadline is checked each turn. */
        struct timespec to = { 0, 200000000 };
        int sig;
        if (g_xtrace_on && s_secs > 0
            && now_ns() >= s_start_ns + (uint64_t)s_secs * 1000000000ull) {
            stop_and_write();
            continue;
        }
        sig = sigtimedwait(set, NULL, &to);
        if (sig == SIGUSR2) {
            if (g_xtrace_on)
                stop_and_write();
            else
                start();
        }
    }
    return NULL;
}

void xtrace_init(void)
{
    static sigset_t set;
    const char *p = getenv("RECOMP_TRACE"), *e;
    pthread_t th;

    if (s_armed || !p || !*p)
        return;
    s_armed = 1;
    snprintf(s_path, sizeof s_path, "%s", p);
    if ((e = getenv("RECOMP_TRACE_SECS")) != NULL)
        s_secs = atoi(e);
    if ((e = getenv("RECOMP_TRACE_EVENTS")) != NULL && atoi(e) > 1024)
        s_cap = (uint32_t)atoi(e);
    if ((e = getenv("RECOMP_TRACE_DRAW")) != NULL && atoi(e) >= 1)
        s_draw_every = (uint32_t)atoi(e);
    /* Blocked here, before other threads exist, so every thread inherits it
     * and only the control thread takes SIGUSR2. */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    if (pthread_create(&th, NULL, control_thread, &set) == 0)
        pthread_detach(th);
    fprintf(stderr, "[TRACE] armed: kill -USR2 %d to record (%s, %u events a thread,"
            " draw phases 1 in %u)\n", (int)getpid(),
            s_secs > 0 ? "stops by itself" : "USR2 again to stop", s_cap, s_draw_every);
    if ((e = getenv("RECOMP_TRACE_START")) != NULL && *e == '1')
        start();
}
