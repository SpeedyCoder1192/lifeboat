/* drawing side shared by every lifeboat screen */
#ifndef UI_H
#define UI_H
#include <stdint.h>
#include <stddef.h>

typedef struct { uint8_t r, g, b; } rgb;
typedef struct { uint32_t ch; rgb fg, bg; uint8_t bold; } cell;
typedef struct { int w, h; cell *g; } canvas;

/* warm black, bone text, safety orange. same numbers as homeguard */
#define BG     ((rgb){0x0c, 0x0c, 0x0b})
#define PANEL  ((rgb){0x13, 0x13, 0x11})
#define PANEL2 ((rgb){0x1e, 0x1d, 0x1a})
#define TEXT   ((rgb){0xe8, 0xe2, 0xd4})
#define MUTED  ((rgb){0x98, 0x93, 0x87})
#define DIM    ((rgb){0x63, 0x5f, 0x57})
#define LINE   ((rgb){0x36, 0x34, 0x2f})
#define SIGNAL ((rgb){0xff, 0x80, 0x18})
#define AMBER  ((rgb){0xe2, 0xb2, 0x3a})
#define GREEN  ((rgb){0x88, 0xc0, 0x60})
#define RED    ((rgb){0xe2, 0x3e, 0x2e})
#define HOT    ((rgb){255, 200, 148})
#define EMBER  ((rgb){97, 52, 15})

enum {
    K_NONE, K_CHAR, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_ESC, K_TAB, K_STAB,
    K_BACK, K_CTRLC, K_CTRLU, K_CLICK, K_MOVE, K_WHEEL
};
typedef struct { int kind, x, y, d; uint32_t ch; } key;
typedef struct { int x, y, w, action, arg; } hit;

struct app {
    void (*draw)(canvas *c);
    void (*key)(key k);
    void (*tick)(double dt);
    int (*fps)(void);
    int minw, minh;
};

struct field { char text[128]; int secret, show; };

extern int ui_quit;
extern int ui_hover, ui_hover_arg;
extern double ui_word_t;

double ui_t(void);
rgb mix(rgb a, rgb b, double t);
int same(rgb a, rgb b);

int put(canvas *c, int x, int y, const char *s, rgb fg, rgb bg, int bold);
int puton(canvas *c, int x, int y, const char *s, rgb fg, int bold);
int putf(canvas *c, int x, int y, rgb fg, rgb bg, int bold, const char *fmt, ...)
    __attribute__((format(printf, 7, 8)));
void fill(canvas *c, int x, int y, int w, int h, rgb bg);
void dim(canvas *c, int x, int y, int w, int h, double amount);
int wrap(canvas *c, int x, int y, int w, const char *s, rgb fg, rgb bg);

int ulen(const char *s);
char *clip(const char *s, int n);
char *human(long long bytes);

void toast(rgb color, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void burst(rgb color, double x, double y);
void flash(int action);
void add_hit(int x, int y, int w, int action, int arg);
hit *find_hit(int x, int y);

int word_width(const char *word);
int pixel_word(canvas *c, int x, int y, const char *word, rgb color, rgb bg, double since);
int ui_link(canvas *c, int x, int y, const char *k, const char *label, int action, rgb color, rgb bg);
void progress(canvas *c, int x, int y, int w, double frac, rgb bg);
void meter(canvas *c, int x, int y, int w, double frac, rgb color, rgb bg);
const char *spinner(void);

int field_key(struct field *f, key k);
void field_draw(struct field *f, canvas *c, int x, int y, int w, rgb bg);

void ui_run(struct app *a);
void ui_dump(struct app *a, int w, int h);
void ui_outside(char *const argv[], int pause);

#endif
