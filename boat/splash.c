/* lifeboat-splash: the boot and shutdown animation on the text console.
 *
 *   lifeboat-splash boot [resume]      while the system comes up
 *   lifeboat-splash down [reboot]      while it goes down
 *
 * A lifeboat bobbing on scrolling waves, the word in the same pixel font as
 * the home screen, and a line of what's going on. It reads /run/lifeboat/splash
 * every frame, written by init, rcS and lifeboat itself:
 *   text=loading the system into ram
 *   pct=40                 or -1 for "busy, no number"
 *   watch=SRC|DST          percentage from how much of SRC has been copied to DST
 *   end=1                  finish nicely (boot: flash and fade, down: sail off)
 *   quit=1                 stop at once and leave the screen as it is
 * Libc only and static, so it runs in the initramfs before anything exists.
 */
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CTL "/run/lifeboat/splash"

/* The 16 console colours are redefined to the lifeboat palette. Slots 0-7 can
   be backgrounds, so everything that can be the lower half of a cell is there. */
enum { BG, DEEP, SEA, FOAM, HULL, KEEL, BONE, LINE, MUTED, HOT, AMBER, GREEN, RED, DIM, SIG2, WHITE };
static const unsigned PAL[16] = {
    0x0c0c0b, 0x14262a, 0x1f4a4f, 0x8fb3ad, 0xff8018, 0x8a3c0c, 0xe8e2d4, 0x36342f,
    0x989387, 0xffc894, 0xe2b23a, 0x88c060, 0xe23e2e, 0x635f57, 0xff9a40, 0xfff6e8,
};

static int out = 1, W = 80, H = 25, dump;
static double fade = 0;          /* 0 black .. 1 full colour */
static unsigned char *px;        /* W x 2H pixels */
static char **prev;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void say(const char *s)
{
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write(out, s, n);
        if (w <= 0)
            return;
        s += w;
        n -= w;
    }
}

static void palette(double k)
{
    char buf[32 * 16 + 1], *p = buf;
    for (int i = 0; i < 16; i++) {
        unsigned r = (PAL[i] >> 16 & 255) * k, g = (PAL[i] >> 8 & 255) * k, b = (PAL[i] & 255) * k;
        p += sprintf(p, "\033]P%X%02x%02x%02x", i, r, g, b);
    }
    say(buf);
}

static void pset(int x, int y, int c)
{
    if (x >= 0 && x < W && y >= 0 && y < 2 * H)
        px[y * W + x] = c;
}

/* --- the pixel font, the same letters as the home screen ----------------- */

static const char *FONT[][6] = {
    {"A", "01110", "10001", "11111", "10001", "10001"}, {"B", "11110", "10001", "11110", "10001", "11110"},
    {"D", "11110", "10001", "10001", "10001", "11110"}, {"E", "11111", "10000", "11110", "10000", "11111"},
    {"F", "11111", "10000", "11110", "10000", "10000"}, {"I", "11111", "00100", "00100", "00100", "11111"},
    {"L", "10000", "10000", "10000", "10000", "11111"}, {"O", "01110", "10001", "10001", "10001", "01110"},
    {"T", "11111", "00100", "00100", "00100", "00100"}, {"Y", "10001", "01010", "00100", "00100", "00100"},
    {"S", "01111", "10000", "01110", "00001", "11110"}, {"R", "11110", "10001", "11110", "10100", "10011"},
    {"N", "10001", "11001", "10101", "10011", "10001"}, {"G", "01111", "10000", "10011", "10001", "01111"},
    {"V", "10001", "10001", "10001", "01010", "00100"}, {" ", "000", "000", "000", "000", "000"},
};

static const char **glyph(char ch)
{
    for (size_t i = 0; i < sizeof FONT / sizeof *FONT; i++)
        if (FONT[i][0][0] == ch)
            return FONT[i] + 1;
    return FONT[15] + 1;
}

static int word_w(const char *w)
{
    int n = 0;
    for (; *w; w++)
        n += strlen(glyph(*w)[0]) + 1;
    return n - 1;
}

/* letters drop in one after another, white hot, and cool to their colour */
static void word(const char *w, int x, int y, int color, double age)
{
    for (int i = 0; w[i]; i++) {
        const char **g = glyph(w[i]);
        int gw = strlen(g[0]);
        double a = age - i * 0.06;
        if (a > 0) {
            int drop = a < 0.25 ? (int)((0.25 - a) * 16) : 0;
            int c = a < 0.18 ? WHITE : a < 0.4 ? HOT : color;
            for (int r = 0; r < 5; r++)
                for (int k = 0; k < gw; k++)
                    if (g[r][k] == '1')
                        pset(x + k, y + r - drop, c);
        }
        x += gw + 1;
    }
}

/* --- the boat --------------------------------------------------------------- */

/* an enclosed lifeboat, bow to the right. F is the flag, waved in code */
static const char *BOAT[] = {
    "...........................m............",
    "...........................m............",
    "...........................m............",
    "..............HHHHHHHHHHHHHmHHH.........",
    "...........HHHHHHHHHHHHHHHHHHHHHHH......",
    ".........HHHHwwHHHwwHHHwwHHHwwHHHHHH....",
    "........HHHHHwwHHHwwHHHwwHHHwwHHHHHHH...",
    ".......HHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHH.",
    "....bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    ".....ooooooooooooooooooooooooooooooooooo",
    "......ooooooooooooooooooooooooooooooooo.",
    ".......kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk..",
    ".........kkkkkkkkkkkkkkkkkkkkkkkkkkk....",
};
#define BOAT_W 41
#define BOAT_H 13

static double surface(int x, double t)
{
    return 1.6 * sin(x * 0.12 + t * 1.9) + 0.8 * sin(x * 0.045 - t * 1.2) + 0.45 * sin(x * 0.29 + t * 3.1);
}

static unsigned hash(unsigned x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

static void scene(double t, double boat_x, int sea_top, double calm)
{
    int base = sea_top * 2 + 4;

    /* a few stars, twinkling slowly, over the top part */
    for (int i = 0; i < W * H / 90; i++) {
        unsigned h = hash(i * 2654435761u + 7);
        int x = h % W, y = (h >> 12) % (unsigned)(base - 30 > 4 ? base - 30 : 4);
        double tw = sin(t * (0.6 + (h >> 24) % 7 * 0.15) + i);
        /* not inside or right next to the letters */
        int clear = 1;
        for (int dy = -2; dy <= 2 && clear; dy++)
            for (int dx = -2; dx <= 2 && clear; dx++)
                if (x + dx >= 0 && x + dx < W && y + dy >= 0 && y + dy < 2 * H && px[(y + dy) * W + x + dx] != BG)
                    clear = 0;
        if (tw > 0.2 && clear)
            pset(x, y, tw > 0.85 ? BONE : tw > 0.55 ? MUTED : DIM);
    }

    /* the boat rides the water under its middle, leaning with the slope */
    int bx = (int)boat_x - BOAT_W / 2;
    double s = surface((int)boat_x, t) * calm;
    double lean = (surface((int)boat_x + 12, t) - surface((int)boat_x - 12, t)) * calm * 0.5;
    int by = base + (int)lround(s) - BOAT_H + 1;
    for (int r = 0; r < BOAT_H; r++)
        for (int k = 0; k < BOAT_W; k++) {
            int c = -1;
            switch (BOAT[r][k]) {
            case 'H': c = HULL; break;
            case 'w': c = ((int)(t * 0.8) + k / 5) % 9 == 0 ? HOT : AMBER; break;
            case 'b': c = BONE; break;
            case 'o': c = HULL; break;
            case 'k': c = KEEL; break;
            case 'm': c = MUTED; break;
            }
            if (c >= 0)
                pset(bx + k, by + r + (int)lround(lean * (k - BOAT_W / 2) / BOAT_W), c);
        }
    /* the flag, waving off the top of the mast */
    for (int k = 1; k <= 6; k++) {
        int wave = (int)lround(sin(t * 6 - k * 0.9) * (k / 4.0));
        int fx = bx + 27 - k, fy = by + wave + (int)lround(lean * (27 - BOAT_W / 2) / BOAT_W);
        pset(fx, fy, SIG2);
        if (k < 5)
            pset(fx, fy + 1, SIG2);
    }

    /* the sea, all the way down, drawn over the bottom of the hull */
    for (int x = 0; x < W; x++) {
        int top = base + (int)lround(surface(x, t) * calm);
        for (int y = top; y < 2 * H; y++) {
            int d = y - top, c;
            if (d == 0)
                c = (hash(x * 31 + (int)(t * 3)) % 9 == 0) ? WHITE : FOAM;
            else if (d < 3)
                c = SEA;
            else
                c = DEEP;
            if (d == 0 && px[y * W + x] == HULL)
                continue;
            pset(x, y, c);
        }
        /* the boat's light on the water, broken up by the waves */
        if (x > bx + 6 && x < bx + BOAT_W - 4) {
            for (int y = top + 3; y < top + 10; y += 2) {
                /* short streaks that drift, like light on moving water */
                double g = sin((x / 3) * 1.7 + y * 0.9 + t * 2.4);
                if (g > 0.6)
                    pset(x, y, (y - top) < 6 ? KEEL : SEA);
            }
        }
    }
}

/* --- drawing the cells -------------------------------------------------------- */

static void text(char *cells, int *fg, int *bg, int y, const char *s, int color)
{
    int n = strlen(s), x = (W - n) / 2;
    for (int i = 0; i < n && x + i < W; i++)
        if (x + i >= 0) {
            cells[(y * W + x + i) * 4] = s[i];
            cells[(y * W + x + i) * 4 + 1] = 0;
            fg[y * W + x + i] = color;
            bg[y * W + x + i] = DEEP;
        }
}

static void flush(char *cells, int *fg, int *bg)
{
    char line[64 * 1024];
    for (int y = 0; y < H; y++) {
        int o = sprintf(line, "\033[%d;1H", y + 1), last_f = -1, last_b = -1;
        for (int x = 0; x < W; x++) {
            int f = fg[y * W + x], b = bg[y * W + x];
            if (f != last_f || b != last_b) {
                if (dump)       /* for looking at a frame without a console */
                    o += sprintf(line + o, "\033[0;38;2;%d;%d;%d;48;2;%d;%d;%dm", PAL[f] >> 16, PAL[f] >> 8 & 255,
                                 PAL[f] & 255, PAL[b] >> 16, PAL[b] >> 8 & 255, PAL[b] & 255);
                else
                    /* the console does bright colours with a bold flag that sticks,
                       so every change starts from a reset */
                    o += sprintf(line + o, "\033[0;%s%d;%dm", f < 8 ? "" : "1;", 30 + (f & 7), 40 + b);
                last_f = f;
                last_b = b;
            }
            o += sprintf(line + o, "%s", cells + (y * W + x) * 4);
        }
        if (dump) {
            printf("%s\033[0m\n", line + strcspn(line, "H") + 1);
            continue;
        }
        if (!prev[y] || strcmp(prev[y], line)) {
            say(line);
            free(prev[y]);
            prev[y] = strdup(line);
        }
    }
}

static void render_cells(const char *status, double pct, double t, int bar, const char *l2, const char *l3, int c3);

static void render(const char *status, double pct, double t, int bar)
{
    render_cells(status, pct, t, bar, NULL, NULL, 0);
}

/* the status line, then two more lines in place of the bar */
static void render2(const char *status, const char *l2, const char *l3, int c3, double t)
{
    render_cells(status, -1, t, 0, l2, l3, c3);
}

static void render_cells(const char *status, double pct, double t, int bar, const char *l2, const char *l3, int c3)
{
    char *cells = calloc((size_t)W * H, 4);
    int *fg = calloc((size_t)W * H, sizeof(int)), *bg = calloc((size_t)W * H, sizeof(int));

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int a = px[(2 * y) * W + x], b = px[(2 * y + 1) * W + x], i = y * W + x;
            const char *ch = " ";
            int f = BG, k = BG;
            if (a == b) {
                if (a < 8) k = a; else { ch = "█"; f = a; }
            } else if (b == BG) {
                ch = "▀"; f = a;
            } else if (a == BG) {
                ch = "▄"; f = b;
            } else if (b < 8) {
                ch = "▀"; f = a; k = b;
            } else if (a < 8) {
                ch = "▄"; f = b; k = a;
            } else {
                ch = "▀"; f = a;
            }
            strcpy(cells + i * 4, ch);
            fg[i] = f;
            bg[i] = k;
        }

    int ty = H * 3 / 4 + 2;
    if (status && *status)
        text(cells, fg, bg, ty, status, FOAM);
    if (l2 && ty + 2 < H) {
        /* utf-8 dots: count characters, not bytes, to centre them */
        int n = 0, x;
        for (const char *p = l2; *p; p++)
            n += (*p & 0xc0) != 0x80;
        x = (W - n) / 2;
        for (const char *p = l2; *p && x < W; x++) {
            int len = (*p & 0x80) ? ((*p & 0xe0) == 0xc0 ? 2 : (*p & 0xf0) == 0xe0 ? 3 : 4) : 1;
            if (x >= 0) {
                memcpy(cells + ((ty + 2) * W + x) * 4, p, len);
                cells[((ty + 2) * W + x) * 4 + len] = 0;
                fg[(ty + 2) * W + x] = BONE;
                bg[(ty + 2) * W + x] = DEEP;
            }
            p += len;
        }
    }
    if (l3 && ty + 4 < H)
        text(cells, fg, bg, ty + 4, l3, c3);
    if (bar && ty + 2 < H) {
        int bw = W / 3 > 20 ? W / 3 : 20, x0 = (W - bw) / 2, y = ty + 2, shift = (int)(t * 10);
        double head = (sin(t * 2.2) * 0.5 + 0.5) * (bw - 8);
        for (int i = 0; i < bw; i++) {
            int on = pct >= 0 ? i < bw * pct / 100 : (i >= head && i < head + 8);
            int c = on ? ((i + shift) % 4 < 2 ? HULL : KEEL) : SEA;
            strcpy(cells + (y * W + x0 + i) * 4, "▀");
            fg[y * W + x0 + i] = c;
            bg[y * W + x0 + i] = DEEP;
        }
        if (pct >= 0) {
            char num[8];
            snprintf(num, sizeof num, "%d%%", (int)pct);
            for (int i = 0; num[i]; i++) {
                int x = x0 + bw + 2 + i;
                if (x < W) {
                    cells[(y * W + x) * 4] = num[i];
                    cells[(y * W + x) * 4 + 1] = 0;
                    fg[y * W + x] = BONE;
                    bg[y * W + x] = DEEP;
                }
            }
        }
    }
    flush(cells, fg, bg);
    free(cells);
    free(fg);
    free(bg);
}

/* --- what to show -------------------------------------------------------------- */

struct ctl { char text[160], watch[300]; double pct; int end, quit, ask, wrong; };

static void read_ctl(struct ctl *c)
{
    char line[512];
    FILE *f = fopen(CTL, "r");
    c->end = c->quit = c->ask = c->wrong = 0;
    c->watch[0] = 0;
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "text=", 5)) snprintf(c->text, sizeof c->text, "%s", line + 5);
        else if (!strncmp(line, "pct=", 4)) c->pct = atof(line + 4);
        else if (!strncmp(line, "watch=", 6)) snprintf(c->watch, sizeof c->watch, "%s", line + 6);
        else if (!strcmp(line, "end=1")) c->end = 1;
        else if (!strcmp(line, "quit=1")) c->quit = 1;
        else if (!strcmp(line, "ask=1")) c->ask = 1;
        else if (!strcmp(line, "wrong=1")) c->wrong = 1;
    }
    if (f)
        fclose(f);
    if (c->watch[0]) {
        char *bar = strchr(c->watch, '|');
        struct stat a, b;
        if (bar) {
            *bar = 0;
            if (!stat(c->watch, &a) && !stat(bar + 1, &b) && a.st_size > 0)
                c->pct = 100.0 * b.st_size / a.st_size;
            *bar = '|';
        }
    }
}

/* the passphrase, typed on tty1 while ask=1 */
static char typed[200];
static int ntyped, sent, raw_on;
static struct termios saved_tio;

static void keys(int fd, struct ctl *c)
{
    char buf[64];
    ssize_t n;
    if (!c->ask) {
        sent = 0;
        if (raw_on) {
            tcsetattr(fd, TCSANOW, &saved_tio);
            raw_on = 0;
        }
        return;
    }
    if (!raw_on) {
        struct termios t;
        tcgetattr(fd, &saved_tio);
        t = saved_tio;
        cfmakeraw(&t);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &t);
        raw_on = 1;
        ntyped = 0;
    }
    while ((n = read(fd, buf, sizeof buf)) > 0)
        for (int i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\r' || ch == '\n' || ch == 27) {
                FILE *f = fopen("/run/lifeboat/pass.new", "w");
                if (f) {
                    fchmod(fileno(f), 0600);
                    fprintf(f, "%s\n", ch == 27 || !ntyped ? "skip" : typed);
                    fclose(f);
                    rename("/run/lifeboat/pass.new", "/run/lifeboat/pass");
                }
                memset(typed, 0, sizeof typed);
                ntyped = 0;
                sent = 1;
            } else if ((ch == 127 || ch == 8) && ntyped) {
                typed[--ntyped] = 0;
            } else if (ch == 21) {
                memset(typed, 0, sizeof typed);
                ntyped = 0;
            } else if ((unsigned char)ch >= 32 && ntyped < (int)sizeof typed - 1) {
                typed[ntyped++] = ch;
                sent = 0;
            }
        }
}

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
    int down = argc > 1 && !strcmp(argv[1], "down");
    int resume = argc > 2 && !strcmp(argv[2], "resume");
    const char *big = down ? (argc > 2 && !strcmp(argv[2], "reboot") ? "SEE YA" : "BYE") : "LIFEBOAT";
    struct winsize ws;
    struct ctl c = {"", "", -1, 0, 0, 0, 0};
    double t0 = now(), ended = 0, boat_x;

    /* LIFEBOAT_SPLASH_DUMP=120x40@2.5 prints the frame at 2.5s and exits */
    const char *dump_at = getenv("LIFEBOAT_SPLASH_DUMP");
    double dump_t = 0;
    if (dump_at && sscanf(dump_at, "%dx%d@%lf", &W, &H, &dump_t) == 3) {
        dump = 1;
        px = malloc((size_t)W * H * 2);
        prev = calloc(H, sizeof *prev);
        boat_x = W / 2.0;
        snprintf(c.text, sizeof c.text, "%s", getenv("LIFEBOAT_SPLASH_TEXT") ? getenv("LIFEBOAT_SPLASH_TEXT") : "");
        c.pct = getenv("LIFEBOAT_SPLASH_PCT") ? atof(getenv("LIFEBOAT_SPLASH_PCT")) : -1;
        memset(px, BG, (size_t)W * H * 2);
        int sea_top = H / 2;
        ended = getenv("LIFEBOAT_SPLASH_END") ? dump_t - atof(getenv("LIFEBOAT_SPLASH_END")) : 0;
        int wx = (W - word_w(big)) / 2, wy = sea_top * 2 - 24;
        if (ended && !down)
            word(big, wx, wy, GREEN, 9);
        else
            word(big, wx, wy, down ? MUTED : BONE, dump_t - 0.3);
        if (down && ended)
            boat_x = W / 2.0 + pow((dump_t - ended) * 1.4, 2) * 12;
        scene(dump_t, boat_x, sea_top, down && ended ? 1 - (dump_t - ended) * 0.3 : 1);
        render(c.text, c.pct, dump_t, !ended);
        return 0;
    }
    out = open("/dev/tty1", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (out < 0)
        return 0;
    if (!ioctl(out, TIOCGWINSZ, &ws) && ws.ws_col && ws.ws_row) {
        W = ws.ws_col;
        H = ws.ws_row;
    }
    if (W < 40 || H < 16)
        return 0;
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    px = malloc((size_t)W * H * 2);
    prev = calloc(H, sizeof *prev);
    say("\033[?25l\033[0m\033[2J");
    fade = resume ? 1 : 0;
    palette(fade);
    boat_x = W / 2.0;
    if (down)
        snprintf(c.text, sizeof c.text, "%s", argc > 2 && !strcmp(argv[2], "reboot") ? "restarting" : "powering off");

    for (;;) {
        double t = now() - t0;
        read_ctl(&c);
        if (c.quit || stop)
            break;
        keys(out, &c);
        /* going down, show the boat for a moment even when there's nothing to wait for */
        if (c.end && !ended && (!down || t > 1.4))
            ended = t;

        /* fade in, and out at the end */
        double f = resume ? 1 : t / (down ? 0.3 : 0.6);
        if (ended)
            f = 1 - (t - ended - (down ? 1.6 : 0.5)) / 0.5;
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        if (fabs(f - fade) > 0.01 || (f == 0 && fade != 0)) {
            fade = f;
            palette(fade);
            /* a truecolor console only recolours what gets drawn after the
               palette changes, so everything gets drawn again */
            for (int y = 0; y < H; y++) {
                free(prev[y]);
                prev[y] = NULL;
            }
        }
        if (ended && f <= 0)
            break;

        memset(px, BG, (size_t)W * H * 2);
        int sea_top = H / 2;
        double age = resume ? 9 : t - 0.3;
        int wx = (W - word_w(big)) / 2, wy = sea_top * 2 - 24;
        if (ended && !down)
            word(big, wx, wy, (int)((t - ended) * 10) % 2 ? GREEN : HOT, 9);
        else
            word(big, wx, wy, down ? MUTED : BONE, age);

        /* going down: the boat sails off to the right once the work is done */
        if (down && ended)
            boat_x = W / 2.0 + pow((t - ended) * 1.4, 2) * 12;
        else if (down)
            boat_x = W / 2.0 + sin(t * 0.7) * 2;
        else
            boat_x = W / 2.0 + sin(t * 0.5) * 3;
        scene(t, boat_x, sea_top, down && ended ? 1 - (t - ended) * 0.3 : 1);
        if (c.ask) {
            /* a row of dots where the bar was, and a caret */
            char shown[120];
            int n = ntyped < 40 ? ntyped : 40;
            for (int i = 0; i < n; i++)
                memcpy(shown + i * 3, "•", 3);
            shown[n * 3] = 0;
            strcat(shown, (int)(t * 2) % 2 ? "▌" : " ");
            char line[200];
            snprintf(line, sizeof line, "%s", c.wrong && !ntyped ? "that's not it. again, or esc to start fresh"
                                                                   : sent ? "checking" : "passphrase, then enter. esc starts fresh");
            render2(c.text, shown, line, c.wrong && !ntyped ? RED : FOAM, t);
        } else {
            render(c.text, c.pct, t, !ended);
        }
        usleep(33000);
    }
    if (!c.quit) {
        say("\033[0m\033[2J\033[H");
        palette(1);
        say("\033]R");
    }
    if (raw_on)
        tcsetattr(out, TCSANOW, &saved_tio);
    say("\033[?25h");
    return 0;
}
