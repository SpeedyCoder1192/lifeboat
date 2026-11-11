/* lifeboat surface DISK: read every byte of a disk and draw what it was like,
 * one cell per slice of the disk. Green reads fast, amber slow, red didn't
 * read at all. Nothing is written. Runs as root in its own terminal.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "boat.h"
#include "ui.h"

#define CHUNK (1 << 20)
#define MAXC  20000

enum { S_ACTIONS = 400, S_QUIT, S_PAUSE };

static const char *dev;
static long long size;
static int fd, ncells;
static float speed[MAXC];       /* MB/s per cell, 0 = not read yet, -1 = errors */
static int errors[MAXC];
static volatile int at, paused, finished, quitting;
static volatile long long done_bytes;
static double t_start, t_paused;
static int total_errors, slow_cells;
static double fastest;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *reader(void *arg)
{
    void *buf;
    (void)arg;
    if (posix_memalign(&buf, 4096, CHUNK))
        return NULL;
    for (int c = 0; c < ncells && !quitting; c++) {
        long long from = size * c / ncells, to = size * (c + 1) / ncells, off;
        double t0 = now_s(), paused_for = 0;
        int errs = 0;
        from &= ~4095LL;
        for (off = from; off < to && !quitting; ) {
            while (paused && !quitting) {
                double p0 = now_s();
                usleep(100000);
                paused_for += now_s() - p0;
            }
            size_t n = to - off < CHUNK ? (size_t)((to - off + 4095) & ~4095LL) : CHUNK;
            ssize_t r = pread(fd, buf, n, off);
            if (r <= 0) {
                /* step over the bad spot in small reads */
                errs++;
                off += 4096;
                continue;
            }
            off += r;
            done_bytes += r;
        }
        double secs = now_s() - t0 - paused_for;
        errors[c] = errs;
        speed[c] = errs ? -1 : (float)((to - from) / (secs > 1e-6 ? secs : 1e-6) / 1e6);
        at = c + 1;
    }
    finished = 1;
    free(buf);
    return NULL;
}

static rgb cell_color(int c)
{
    if (c >= at)
        return c == at && !finished ? (((int)(ui_t() * 4)) % 2 ? SIGNAL : EMBER) : PANEL2;
    if (speed[c] < 0)
        return RED;
    double rel = fastest > 0 ? speed[c] / fastest : 1;
    if (rel < 0.15)
        return mix(AMBER, RED, 0.4);
    if (rel < 0.4)
        return AMBER;
    return mix(GREEN, (rgb){0x5c, 0x80, 0x40}, 1 - rel);
}

static void draw(canvas *c)
{
    char line[200];
    int gw = c->w - 4, gh = c->h - 11, x0 = 2, y0 = 6;
    double el = (finished ? t_paused : now_s()) - t_start;
    double mbs = el > 0.5 ? done_bytes / el / 1e6 : 0;

    total_errors = slow_cells = 0;
    fastest = 0;
    for (int i = 0; i < at; i++)
        if (speed[i] > fastest)
            fastest = speed[i];
    for (int i = 0; i < at; i++) {
        total_errors += errors[i];
        if (speed[i] >= 0 && fastest > 0 && speed[i] / fastest < 0.15)
            slow_cells++;
    }

    const char *w = finished ? (total_errors ? "BAD" : "DONE") : paused ? "PAUSE" : "SCAN";
    rgb wc = finished ? (total_errors ? RED : GREEN) : paused ? MUTED : AMBER;
    int end = pixel_word(c, 2, 0, w, wc, BG, ui_t() - ui_word_t);
    snprintf(line, sizeof line, "%s, %s of %s read", dev, human(done_bytes), human(size));
    put(c, end + 4, 0, clip(line, c->w - end - 6), TEXT, BG, 1);
    if (!finished && mbs > 0) {
        long left = (long)((size - done_bytes) / 1e6 / mbs);
        snprintf(line, sizeof line, "%.0f MB/s, about %ldh %02ldm left", mbs, left / 3600, left / 60 % 60);
    } else {
        snprintf(line, sizeof line, "took %ldm %02lds, %.0f MB/s on average", (long)el / 60, (long)el % 60, mbs);
    }
    put(c, end + 4, 1, clip(line, c->w - end - 6), MUTED, BG, 0);
    snprintf(line, sizeof line, "%d unreadable spot%s, %d slow area%s", total_errors, total_errors == 1 ? "" : "s",
             slow_cells, slow_cells == 1 ? "" : "s");
    put(c, end + 4, 2, clip(line, c->w - end - 6), total_errors ? RED : slow_cells ? AMBER : DIM, BG, 0);

    int x = ui_link(c, 1, 4, "p", paused ? "go on" : "pause", S_PAUSE, SIGNAL, BG);
    ui_link(c, x, 4, "q", "stop", S_QUIT, MUTED, BG);

    /* the map: each cell a slice of the disk, half blocks so two per character */
    for (int row = 0; row < gh; row++)
        for (int col = 0; col < gw; col++) {
            int top = (row * 2) * gw + col, bot = (row * 2 + 1) * gw + col;
            rgb a = top < ncells ? cell_color(top) : BG, b = bot < ncells ? cell_color(bot) : BG;
            put(c, x0 + col, y0 + row, "▀", a, b, 0);
        }

    int ly = c->h - 4;
    fill(c, 0, ly - 1, c->w, 5, PANEL);
    x = 2;
    struct { rgb c; const char *t; } key[] = {
        {GREEN, "fast"}, {AMBER, "slow"}, {mix(AMBER, RED, 0.4), "very slow"}, {RED, "didn't read"},
        {PANEL2, "not yet"}};
    for (int i = 0; i < 5; i++) {
        x = put(c, x, ly, "██", key[i].c, PANEL, 0) + 1;
        x = put(c, x, ly, key[i].t, MUTED, PANEL, 0) + 3;
    }
    snprintf(line, sizeof line, "each square is %s of the disk", human(size / (ncells ? ncells : 1)));
    put(c, x + 2, ly, clip(line, c->w - x - 4), DIM, PANEL, 0);
    put(c, 2, ly + 1, clip(finished ? (total_errors ? "it has spots that don't read. copy what matters off it now, "
                                                      "with d then 4 on the home screen"
                                                    : slow_cells ? "it all reads, but some areas are slow. a disk "
                                                                   "starting to wear"
                                                                 : "it all reads, at a steady speed. looks healthy")
                                    : "nothing gets written. q stops whenever", c->w - 4),
        finished ? (total_errors ? RED : slow_cells ? AMBER : GREEN) : DIM, PANEL, 0);
}

static void on_key(key k)
{
    hit *h;
    if (k.kind == K_CLICK && (h = find_hit(k.x, k.y))) {
        if (h->action == S_QUIT) ui_quit = 1;
        if (h->action == S_PAUSE) paused = !paused;
        return;
    }
    if (k.kind == K_CTRLC || k.kind == K_ESC || (k.kind == K_CHAR && k.ch == 'q'))
        ui_quit = 1;
    else if (k.kind == K_CHAR && (k.ch == 'p' || k.ch == ' '))
        paused = !paused;
}

static void tick(double dt)
{
    (void)dt;
    if (finished && !t_paused) {
        t_paused = now_s();
        ui_word_t = ui_t();
        burst(total_errors ? RED : GREEN, 10, 1.5);
    }
}

static int fps(void)
{
    return finished ? 2 : 8;
}

int cmd_surface(const char *d)
{
    struct app app = {draw, on_key, tick, fps, 60, 20};
    pthread_t th;
    char out[64];

    dev = d;
    if (geteuid()) {
        fprintf(stderr, "that needs root\n");
        return 1;
    }
    run(out, sizeof out, "lsblk", "-bndo", "SIZE", d);
    size = atoll(out);
    fd = open(d, O_RDONLY | O_DIRECT);
    if (fd < 0 || size <= 0) {
        perror(d);
        return 1;
    }
    /* as many cells as the window has room for, at most one per chunk */
    {
        char cols[16] = "120", rows[16] = "40";
        run(cols, sizeof cols, "tput", "cols");
        run(rows, sizeof rows, "tput", "lines");
        ncells = (atoi(cols) - 4) * (atoi(rows) - 11) * 2;
        if (ncells > MAXC) ncells = MAXC;
        if (ncells > size / CHUNK) ncells = size / CHUNK;
        if (ncells < 1) ncells = 1;
    }
    t_start = now_s();
    pthread_create(&th, NULL, reader, NULL);
    ui_word_t = 0;
    ui_run(&app);
    quitting = 1;
    pthread_join(th, NULL);
    close(fd);
    return 0;
}
