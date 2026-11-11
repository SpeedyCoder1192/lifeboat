/* cell canvas, pixel font, keys and the frame loop */
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "ui.h"

int ui_quit;
int ui_hover = -1, ui_hover_arg;
double ui_word_t;

static struct termios saved;
static double t0, touched, flash_until;
static int flash_action = -1;
static hit hits[512];
static int nhits;

static struct { char text[160]; rgb color; double born; } toasts[8];
static int ntoasts;
static struct { double x, y, vx, vy, life; char ch; rgb c; } sparks[160];
static int nsparks;

static const char *FONT[][6] = {
    {"A", "01110", "10001", "11111", "10001", "10001"},
    {"B", "11110", "10001", "11110", "10001", "11110"},
    {"C", "01111", "10000", "10000", "10000", "01111"},
    {"D", "11110", "10001", "10001", "10001", "11110"},
    {"E", "11111", "10000", "11110", "10000", "11111"},
    {"F", "11111", "10000", "11110", "10000", "10000"},
    {"G", "01111", "10000", "10011", "10001", "01111"},
    {"H", "10001", "10001", "11111", "10001", "10001"},
    {"I", "11111", "00100", "00100", "00100", "11111"},
    {"J", "00111", "00010", "00010", "10010", "01100"},
    {"K", "10001", "10010", "11100", "10010", "10001"},
    {"L", "10000", "10000", "10000", "10000", "11111"},
    {"M", "10001", "11011", "10101", "10001", "10001"},
    {"N", "10001", "11001", "10101", "10011", "10001"},
    {"O", "01110", "10001", "10001", "10001", "01110"},
    {"P", "11110", "10001", "11110", "10000", "10000"},
    {"Q", "01110", "10001", "10101", "10010", "01101"},
    {"R", "11110", "10001", "11110", "10100", "10011"},
    {"S", "01111", "10000", "01110", "00001", "11110"},
    {"T", "11111", "00100", "00100", "00100", "00100"},
    {"U", "10001", "10001", "10001", "10001", "01110"},
    {"V", "10001", "10001", "10001", "01010", "00100"},
    {"W", "10001", "10001", "10101", "11011", "10001"},
    {"X", "10001", "01010", "00100", "01010", "10001"},
    {"Y", "10001", "01010", "00100", "00100", "00100"},
    {"Z", "11111", "00010", "00100", "01000", "11111"},
    {" ", "00", "00", "00", "00", "00"},
    {"0", "111", "101", "101", "101", "111"},
    {"1", "010", "110", "010", "010", "111"},
    {"2", "111", "001", "111", "100", "111"},
    {"3", "111", "001", "111", "001", "111"},
    {"4", "101", "101", "111", "001", "001"},
    {"5", "111", "100", "111", "001", "111"},
    {"6", "111", "100", "111", "101", "111"},
    {"7", "111", "001", "010", "010", "010"},
    {"8", "111", "101", "111", "101", "111"},
    {"9", "111", "101", "111", "001", "111"},
    {":", "0", "1", "0", "1", "0"},
    {"?", "111", "001", "010", "000", "010"},
};

static const char **glyph(char ch)
{
    for (size_t i = 0; i < sizeof FONT / sizeof *FONT; i++)
        if (FONT[i][0][0] == ch)
            return FONT[i] + 1;
    return FONT[26] + 1;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

double ui_t(void)
{
    if (!t0)
        t0 = now();
    return now() - t0;
}

rgb mix(rgb a, rgb b, double t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (rgb){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

int same(rgb a, rgb b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

/* --- text ------------------------------------------------------------- */

static uint32_t next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp = *p++;
    int more = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
    if (more)
        cp &= 0x3f >> more;
    while (more-- && (*p & 0xc0) == 0x80)
        cp = cp << 6 | (*p++ & 0x3f);
    *s = (const char *)p;
    return cp;
}

static int enc_cp(char *out, uint32_t cp)
{
    if (cp < 0x80) { out[0] = cp; return 1; }
    if (cp < 0x800) { out[0] = 0xc0 | cp >> 6; out[1] = 0x80 | (cp & 0x3f); return 2; }
    if (cp < 0x10000) {
        out[0] = 0xe0 | cp >> 12; out[1] = 0x80 | (cp >> 6 & 0x3f); out[2] = 0x80 | (cp & 0x3f);
        return 3;
    }
    out[0] = 0xf0 | cp >> 18; out[1] = 0x80 | (cp >> 12 & 0x3f);
    out[2] = 0x80 | (cp >> 6 & 0x3f); out[3] = 0x80 | (cp & 0x3f);
    return 4;
}

int ulen(const char *s)
{
    int n = 0;
    while (*s) { next_cp(&s); n++; }
    return n;
}

/* cut to n cells, with an ellipsis. the result lives until four calls later */
char *clip(const char *s, int n)
{
    static char bufs[4][512];
    static int which;
    char *out = bufs[which++ & 3];
    int len = ulen(s), i = 0;
    size_t o = 0;

    if (n <= 0) { out[0] = 0; return out; }
    while (*s && o < sizeof bufs[0] - 8) {
        if (len > n && i == n - 1) {
            o += enc_cp(out + o, 0x2026);
            break;
        }
        o += enc_cp(out + o, next_cp(&s));
        i++;
    }
    out[o] = 0;
    return out;
}

char *human(long long bytes)
{
    static char bufs[4][16];
    static int which;
    static const char units[] = "BKMGT";
    char *out = bufs[which++ & 3];
    double n = bytes;
    int u = 0;

    while (n >= 1024 && u < 4) { n /= 1024; u++; }
    if (u < 2 || n >= 100)
        snprintf(out, 16, "%d%c", (int)n, units[u]);
    else
        snprintf(out, 16, "%.1f%c", n, units[u]);
    return out;
}

static int put_cells(canvas *c, int x, int y, const char *s, rgb fg, const rgb *bg, int bold)
{
    while (*s) {
        uint32_t cp = next_cp(&s);
        if (x >= 0 && x < c->w && y >= 0 && y < c->h) {
            cell *k = &c->g[y * c->w + x];
            k->ch = cp;
            k->fg = fg;
            if (bg)
                k->bg = *bg;
            k->bold = bold;
        }
        x++;
    }
    return x;
}

int put(canvas *c, int x, int y, const char *s, rgb fg, rgb bg, int bold)
{
    return put_cells(c, x, y, s, fg, &bg, bold);
}

/* keeps whatever background is already there */
int puton(canvas *c, int x, int y, const char *s, rgb fg, int bold)
{
    return put_cells(c, x, y, s, fg, NULL, bold);
}

int putf(canvas *c, int x, int y, rgb fg, rgb bg, int bold, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return put(c, x, y, buf, fg, bg, bold);
}

void fill(canvas *c, int x, int y, int w, int h, rgb bg)
{
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < c->h; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < c->w; xx++)
            c->g[yy * c->w + xx] = (cell){' ', TEXT, bg, 0};
}

void dim(canvas *c, int x, int y, int w, int h, double amount)
{
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < c->h; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < c->w; xx++) {
            cell *k = &c->g[yy * c->w + xx];
            k->fg = mix(k->fg, BG, amount);
            k->bg = mix(k->bg, BG, amount);
        }
}

/* word wrap, returns how many lines it took */
int wrap(canvas *c, int x, int y, int w, const char *s, rgb fg, rgb bg)
{
    char line[256];
    int lines = 0;

    while (*s) {
        int n = 0, cut = -1;
        const char *p = s, *brk = NULL;
        while (*p && *p != '\n' && n < w) {
            if (*p == ' ') { brk = p; cut = n; }
            next_cp(&p);
            n++;
        }
        if (*p && *p != '\n' && brk) { p = brk; n = cut; }
        size_t len = p - s;
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, s, len);
        line[len] = 0;
        put(c, x, y + lines++, line, fg, bg, 0);
        s = p;
        while (*s == ' ') s++;
        if (*s == '\n') s++;
    }
    return lines;
}

/* --- small moving things ---------------------------------------------- */

void toast(rgb color, const char *fmt, ...)
{
    va_list ap;
    if (ntoasts == 8) {
        memmove(toasts, toasts + 1, sizeof *toasts * 7);
        ntoasts--;
    }
    va_start(ap, fmt);
    vsnprintf(toasts[ntoasts].text, sizeof toasts[0].text, fmt, ap);
    va_end(ap);
    toasts[ntoasts].color = color;
    toasts[ntoasts++].born = ui_t();
}

void burst(rgb color, double x, double y)
{
    static const char bits[] = ".'`,*";
    for (int i = 0; i < 40 && nsparks < 160; i++) {
        int pick = rand() % 3;
        sparks[nsparks].x = x + (rand() % 260) / 10.0 - 13;
        sparks[nsparks].y = y;
        sparks[nsparks].vx = (rand() % 280) / 10.0 - 14;
        sparks[nsparks].vy = -1 - (rand() % 90) / 10.0;
        sparks[nsparks].life = 0.5 + (rand() % 70) / 100.0;
        sparks[nsparks].ch = bits[rand() % 5];
        sparks[nsparks].c = pick == 0 ? color : pick == 1 ? HOT : TEXT;
        nsparks++;
    }
}

static void step_sparks(double dt)
{
    int keep = 0;
    for (int i = 0; i < nsparks; i++) {
        sparks[i].x += sparks[i].vx * dt;
        sparks[i].y += sparks[i].vy * dt;
        sparks[i].vy += 22 * dt;
        sparks[i].life -= dt;
        if (sparks[i].life > 0)
            sparks[keep++] = sparks[i];
    }
    nsparks = keep;
}

static void draw_sparks(canvas *c)
{
    for (int i = 0; i < nsparks; i++) {
        int x = sparks[i].x, y = sparks[i].y;
        if (x < 0 || x >= c->w || y < 0 || y >= c->h)
            continue;
        char s[2] = {sparks[i].ch, 0};
        double a = sparks[i].life * 2;
        puton(c, x, y, s, mix(c->g[y * c->w + x].bg, sparks[i].c, a > 1 ? 1 : a), 0);
    }
}

static void draw_toasts(canvas *c)
{
    int keep = 0, y = c->h - 2;
    for (int i = 0; i < ntoasts; i++)
        if (ui_t() - toasts[i].born < 3.5)
            toasts[keep++] = toasts[i];
    ntoasts = keep;
    for (int i = ntoasts - 1; i >= 0; i--, y--) {
        double age = ui_t() - toasts[i].born;
        double in = age / 0.15, fade = (3.5 - age) / 0.4;
        int slide = (1 - (in > 1 ? 1 : in)) * 6;
        int x = c->w - ulen(toasts[i].text) - 6 + slide;
        if (fade > 1) fade = 1;
        put(c, x, y, "▌", mix(PANEL2, toasts[i].color, fade), PANEL2, 0);
        putf(c, x + 1, y, mix(PANEL2, TEXT, fade), PANEL2, 0, " %s  ", toasts[i].text);
    }
}

void flash(int action)
{
    flash_action = action;
    flash_until = ui_t() + 0.15;
}

void add_hit(int x, int y, int w, int action, int arg)
{
    if (nhits < 512)
        hits[nhits++] = (hit){x, y, w, action, arg};
}

hit *find_hit(int x, int y)
{
    for (int i = nhits - 1; i >= 0; i--)
        if (hits[i].y == y && x >= hits[i].x && x < hits[i].x + hits[i].w)
            return &hits[i];
    return NULL;
}

/* --- widgets ---------------------------------------------------------- */

int word_width(const char *word)
{
    int w = 0;
    for (; *word; word++)
        w += strlen(glyph(*word)[0]) + 1;
    return w - 1;
}

/* three rows of half blocks per letter. orange, amber and red crawl with
   hazard stripes, and every letter starts hot and cools down */
int pixel_word(canvas *c, int x, int y, const char *word, rgb color, rgb bg, double since)
{
    int striped = same(color, SIGNAL) || same(color, AMBER) || same(color, RED);
    int shift = ui_t() * (same(color, AMBER) ? 7 : 3);
    rgb dark = mix(bg, color, 0.5);

    for (int li = 0; word[li]; li++) {
        const char **rows = glyph(word[li]);
        int gw = strlen(rows[0]);
        double age = since - li * 0.05;
        if (age >= 0) {
            double heat = 1.0 - age / 0.35;
            for (int cy = 0; cy < 3; cy++)
                for (int px = 0; px < gw; px++) {
                    int top = rows[cy * 2][px] == '1';
                    int bot = cy < 2 && rows[cy * 2 + 1][px] == '1';
                    rgb shade[2];
                    for (int h = 0; h < 2; h++) {
                        if (heat > 0)
                            shade[h] = mix(color, HOT, heat);
                        else if (striped && ((x + px + cy * 2 + h + shift) / 2) % 3 == 0)
                            shade[h] = dark;
                        else
                            shade[h] = color;
                    }
                    if (top && bot)
                        put(c, x + px, y + cy, "▀", shade[0], shade[1], 0);
                    else if (top)
                        put(c, x + px, y + cy, "▀", shade[0], bg, 0);
                    else if (bot)
                        put(c, x + px, y + cy, "▄", shade[1], bg, 0);
                }
        }
        x += gw + 1;
    }
    return x - 1;
}

int ui_link(canvas *c, int x, int y, const char *k, const char *label, int action, rgb color, rgb bg)
{
    int len = ulen(k) + 1 + ulen(label);
    if (flash_action == action && ui_t() < flash_until)
        putf(c, x, y, BG, HOT, 1, " %s %s ", k, label);
    else if (ui_hover == action)
        putf(c, x, y, BG, color, 1, " %s %s ", k, label);
    else {
        int e = put(c, x + 1, y, k, color, bg, 1);
        putf(c, e, y, MUTED, bg, 0, " %s ", label);
    }
    add_hit(x, y, len + 2, action, 0);
    return x + len + 3;
}

/* frac below zero means no numbers yet: a block slides back and forth */
void progress(canvas *c, int x, int y, int w, double frac, rgb bg)
{
    int shift = ui_t() * 10;
    if (frac < 0) {
        double head = (sin(ui_t() * 2.2) * 0.5 + 0.5) * (w - 8);
        for (int i = 0; i < w; i++) {
            int on = i >= head && i < head + 8;
            put(c, x + i, y, "▀", on ? ((i + shift) % 4 < 2 ? SIGNAL : EMBER) : PANEL2, bg, 0);
        }
        return;
    }
    int filled = w * frac;
    for (int i = 0; i < w; i++)
        put(c, x + i, y, "▀", i < filled ? ((i + shift) % 4 < 2 ? SIGNAL : EMBER) : PANEL2, bg, 0);
    if (filled > 0 && filled < w)
        put(c, x + filled, y, "▀", HOT, bg, 0);
}

void meter(canvas *c, int x, int y, int w, double frac, rgb color, rgb bg)
{
    int filled = w * (frac < 0 ? 0 : frac > 1 ? 1 : frac) + 0.5;
    for (int i = 0; i < w; i++)
        put(c, x + i, y, "▀", i < filled ? color : PANEL2, bg, 0);
}

const char *spinner(void)
{
    static const char *frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    return frames[(int)(ui_t() * 12) % 10];
}

int field_key(struct field *f, key k)
{
    size_t n = strlen(f->text);
    if (k.kind == K_BACK) {
        while (n && (f->text[n - 1] & 0xc0) == 0x80)
            n--;
        if (n)
            n--;
        f->text[n] = 0;
    } else if (k.kind == K_CTRLU) {
        f->text[0] = 0;
    } else if (k.kind == K_CHAR && k.ch >= 32 && n < sizeof f->text - 5) {
        n += enc_cp(f->text + n, k.ch);
        f->text[n] = 0;
    } else {
        return 0;
    }
    return 1;
}

void field_draw(struct field *f, canvas *c, int x, int y, int w, rgb bg)
{
    int n = ulen(f->text), skip = n > w - 3 ? n - (w - 3) : 0, e = x + 1;
    const char *p = f->text;

    fill(c, x, y, w, 1, bg);
    for (int i = 0; i < n; i++) {
        char one[5] = {0};
        enc_cp(one, next_cp(&p));
        if (i >= skip)
            e = put(c, e, y, f->secret && !f->show ? "•" : one, TEXT, bg, 0);
    }
    if ((int)(ui_t() * 2) % 2 == 0)
        put(c, e, y, "▌", SIGNAL, bg, 0);
}

/* --- keys ------------------------------------------------------------- */

static int parse_keys(const char *d, int len, key *out, int max)
{
    int n = 0, i = 0;
    while (i < len && n < max) {
        key k = {0};
        int btn, x, y, used = 0;
        char fin;

        if (d[i] == 27 && i + 2 < len && d[i + 1] == '[' && d[i + 2] == '<' &&
            sscanf(d + i + 3, "%d;%d;%d%c%n", &btn, &x, &y, &fin, &used) == 4) {
            k.x = x - 1;
            k.y = y - 1;
            if (btn & 64) { k.kind = K_WHEEL; k.d = btn & 1 ? 1 : -1; }
            else if (btn & 32) k.kind = K_MOVE;
            else if (fin == 'M') k.kind = K_CLICK;
            i += 3 + used;
            if (k.kind)
                out[n++] = k;
            continue;
        }
        if (d[i] == 27 && i + 1 < len && d[i + 1] == '[') {
            int j = i + 2;
            while (j < len && (d[j] == ';' || (d[j] >= '0' && d[j] <= '9')))
                j++;
            if (j < len) {
                switch (d[j]) {
                case 'A': k.kind = K_UP; break;
                case 'B': k.kind = K_DOWN; break;
                case 'C': k.kind = K_RIGHT; break;
                case 'D': k.kind = K_LEFT; break;
                case 'Z': k.kind = K_STAB; break;
                }
                i = j + 1;
                if (k.kind)
                    out[n++] = k;
                continue;
            }
        }
        switch (d[i]) {
        case 27: k.kind = K_ESC; i++; break;
        case '\r': case '\n': k.kind = K_ENTER; i++; break;
        case '\t': k.kind = K_TAB; i++; break;
        case 3: k.kind = K_CTRLC; i++; break;
        case 21: k.kind = K_CTRLU; i++; break;
        case 127: case 8: k.kind = K_BACK; i++; break;
        default: {
            const char *p = d + i;
            k.kind = K_CHAR;
            k.ch = next_cp(&p);
            i = p - d;
        }
        }
        out[n++] = k;
    }
    return n;
}

/* --- screen ----------------------------------------------------------- */

static void say(const char *s)
{
    if (write(1, s, strlen(s)) < 0)
        ui_quit = 1;
}

static void enter_screen(void)
{
    struct termios raw = saved;
    cfmakeraw(&raw);
    tcsetattr(0, TCSADRAIN, &raw);
    say("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[?1003h\x1b[?1006h");
}

static void leave_screen(void)
{
    say("\x1b[?1006l\x1b[?1003l\x1b[0m\x1b[?25h\x1b[?1049l");
    tcsetattr(0, TCSADRAIN, &saved);
}

static void on_signal(int sig)
{
    if (sig != SIGWINCH)
        ui_quit = 1;
}

static char **prev;
static int prev_n;

static void forget(void)
{
    for (int i = 0; i < prev_n; i++)
        free(prev[i]);
    free(prev);
    prev = NULL;
    prev_n = 0;
}

static char *row_text(canvas *c, int y, int plain)
{
    char *out = malloc(c->w * 44 + 16);
    size_t o = 0;
    cell *last = NULL;

    for (int x = 0; x < c->w; x++) {
        cell *k = &c->g[y * c->w + x];
        if (!plain && (!last || !same(k->fg, last->fg) || !same(k->bg, last->bg) || k->bold != last->bold))
            o += sprintf(out + o, "\x1b[0;%s38;2;%d;%d;%d;48;2;%d;%d;%dm", k->bold ? "1;" : "",
                         k->fg.r, k->fg.g, k->fg.b, k->bg.r, k->bg.g, k->bg.b);
        last = k;
        o += enc_cp(out + o, k->ch);
    }
    out[o] = 0;
    return out;
}

static canvas frame(struct app *a, int w, int h)
{
    canvas c = {w, h, malloc(sizeof(cell) * w * h)};
    fill(&c, 0, 0, w, h, BG);
    nhits = 0;
    if (w < a->minw || h < a->minh) {
        put(&c, 2, 1, "make the window bigger", TEXT, BG, 1);
        putf(&c, 2, 2, MUTED, BG, 0, "it's %dx%d, it needs %dx%d", w, h, a->minw, a->minh);
    } else {
        a->draw(&c);
        draw_sparks(&c);
        draw_toasts(&c);
    }
    return c;
}

/* nothing moving means two frames a second, so an idle screen costs nothing */
static int fps(struct app *a)
{
    double t = ui_t();
    int want = a->fps ? a->fps() : 2;
    if (nsparks || ntoasts || t < flash_until || t - ui_word_t < 1 || t - touched < 0.3)
        return 30;
    return want;
}

void ui_run(struct app *a)
{
    struct sigaction sa = {.sa_handler = on_signal};
    double last = now();

    tcgetattr(0, &saved);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);
    srand(time(NULL));
    enter_screen();

    while (!ui_quit) {
        struct winsize ws = {0};
        struct pollfd pfd = {0, POLLIN, 0};
        double t = now();

        if (a->tick)
            a->tick(t - last);
        step_sparks(t - last);
        last = t;

        ioctl(1, TIOCGWINSZ, &ws);
        if (!ws.ws_col || !ws.ws_row) { ws.ws_col = 80; ws.ws_row = 24; }
        canvas c = frame(a, ws.ws_col, ws.ws_row);
        if (prev_n != c.h) {
            forget();
            prev = calloc(c.h, sizeof *prev);
            prev_n = c.h;
        }
        for (int y = 0; y < c.h; y++) {
            char *line = row_text(&c, y, 0), pos[16];
            if (prev[y] && !strcmp(prev[y], line)) {
                free(line);
                continue;
            }
            snprintf(pos, sizeof pos, "\x1b[%d;1H", y + 1);
            say(pos);
            say(line);
            free(prev[y]);
            prev[y] = line;
        }
        say("\x1b[0m");
        free(c.g);

        if (poll(&pfd, 1, 1000 / fps(a)) > 0) {
            char buf[4096];
            key keys[256];
            int len = read(0, buf, sizeof buf - 1);
            if (len <= 0)
                break;
            buf[len] = 0;
            int n = parse_keys(buf, len, keys, 256);
            touched = ui_t();
            for (int i = 0; i < n; i++) {
                if (keys[i].kind == K_MOVE) {
                    hit *h = find_hit(keys[i].x, keys[i].y);
                    ui_hover = h ? h->action : -1;
                    ui_hover_arg = h ? h->arg : 0;
                }
                a->key(keys[i]);
            }
        }
    }
    leave_screen();
    forget();
}

/* one frame as plain text, for looking at layouts without a terminal */
void ui_dump(struct app *a, int w, int h)
{
    const char *colour = getenv("LIFEBOAT_DUMP_COLOUR");
    t0 = now() - 5;
    if (a->tick)
        a->tick(0);
    canvas c = frame(a, w, h);
    free(c.g);
    ui_word_t = -100;       /* skip the letters' entrance */
    c = frame(a, w, h);
    for (int y = 0; y < h; y++) {
        char *line = row_text(&c, y, !colour);
        printf("%s%s\n", line, colour ? "\x1b[0m" : "");
        free(line);
    }
    free(c.g);
}

/* drop out of the screen to run something interactive, then come back */
void ui_outside(char *const argv[], int pause)
{
    char line[8];
    pid_t pid;

    leave_screen();
    printf("\x1b[1m$");
    for (int i = 0; argv[i]; i++)
        printf(" %s", argv[i]);
    printf("\x1b[0m\n\n");
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
    if (pause) {
        printf("\n\x1b[2menter to go back\x1b[0m");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            clearerr(stdin);
    }
    enter_screen();
    forget();
}
