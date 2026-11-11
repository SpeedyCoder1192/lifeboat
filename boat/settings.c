/* Settings: the everyday things a desktop normally has a panel for. Display
 * size, scale and rotation go through wlr-randr. Keyboard, touchpad and cursor
 * go into labwc's own config files, then labwc reloads. Text size is foot's.
 * Everything lives in ~/.config, so it is kept when the session is saved, and
 * "lifeboat apply" (run from labwc's autostart) puts it back after a boot.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "boat.h"
#include "extras.h"

enum { T_ROW = SET_ACTIONS, T_LESS, T_MORE, T_BACK, T_ZONE };

/* --- the settings file --------------------------------------------------- */

static char keys[64][48], vals[64][64];
static int nkv;

static const char *home_dir(void)
{
    return getenv("HOME") ? getenv("HOME") : "/home/" USER;
}

static void conf_path(char *out, size_t n, const char *rel)
{
    snprintf(out, n, "%s/.config/%s", home_dir(), rel);
}

static void mkdirs(const char *rel)
{
    char path[400], *p;
    conf_path(path, sizeof path, rel);
    for (p = path + strlen(home_dir()) + 1; (p = strchr(p, '/')); p++) {
        *p = 0;
        mkdir(path, 0755);
        *p = '/';
    }
}

static void load_kv(void)
{
    char path[400], line[160];
    FILE *f;

    nkv = 0;
    conf_path(path, sizeof path, "lifeboat/settings");
    f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f) && nkv < 64) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        eq[strcspn(eq + 1, "\n") + 1] = 0;
        snprintf(keys[nkv], sizeof keys[0], "%s", line);
        snprintf(vals[nkv], sizeof vals[0], "%s", eq + 1);
        nkv++;
    }
    if (f)
        fclose(f);
}

static const char *get(const char *k, const char *fallback)
{
    for (int i = 0; i < nkv; i++)
        if (!strcmp(keys[i], k))
            return vals[i];
    return fallback;
}

static void set(const char *k, const char *v)
{
    char path[400];
    FILE *f;
    int i;

    for (i = 0; i < nkv && strcmp(keys[i], k); i++)
        ;
    if (i == nkv && nkv < 64)
        snprintf(keys[nkv++], sizeof keys[0], "%s", k);
    if (i < 64)
        snprintf(vals[i], sizeof vals[0], "%s", v);
    mkdirs("lifeboat/settings");
    conf_path(path, sizeof path, "lifeboat/settings");
    if ((f = fopen(path, "w"))) {
        for (i = 0; i < nkv; i++)
            fprintf(f, "%s=%s\n", keys[i], vals[i]);
        fclose(f);
    }
}

/* --- screens --------------------------------------------------------------- */

struct output {
    char name[32], desc[64], mode[32], transform[16];
    char modes[24][32];
    int nmodes;
    double scale;
};
static struct output outs[4];
static int nouts;

static void read_outputs(void)
{
    static char text[16384];
    struct output *o = NULL;
    int in_modes = 0;

    nouts = 0;
    if (demo()) {
        static const struct output fake = {"eDP-1", "built-in screen", "1920x1200@60", "normal",
            {"1920x1200@60", "1680x1050@60", "1280x800@60", "1024x768@60"}, 4, 1.25};
        outs[0] = fake;
        nouts = 1;
        return;
    }
    if (run(text, sizeof text, "wlr-randr"))
        return;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] != ' ') {
            if (nouts == 4)
                break;
            o = &outs[nouts++];
            memset(o, 0, sizeof *o);
            sscanf(line, "%31s", o->name);
            char *q = strchr(line, '"');
            if (q)
                snprintf(o->desc, sizeof o->desc, "%.*s", (int)strcspn(q + 1, "\""), q + 1);
            o->scale = 1;
            strcpy(o->transform, "normal");
            in_modes = 0;
            continue;
        }
        if (!o)
            continue;
        while (*line == ' ')
            line++;
        if (!strncmp(line, "Modes:", 6)) {
            in_modes = 1;
        } else if (in_modes && isdigit((unsigned char)line[0])) {
            int w, h;
            double hz;
            if (sscanf(line, "%dx%d px, %lf", &w, &h, &hz) == 3 && o->nmodes < 24) {
                /* exact, or wlr-randr may not find the mode (119.998 isn't 120) */
                char m[32];
                snprintf(m, sizeof m, "%dx%d@%.3fHz", w, h, hz);
                int dup = 0;
                for (int i = 0; i < o->nmodes; i++)
                    dup |= !strcmp(o->modes[i], m);
                if (!dup)
                    strcpy(o->modes[o->nmodes++], m);
                if (strstr(line, "current"))
                    strcpy(o->mode, m);
            }
        } else {
            in_modes = 0;
            if (!strncmp(line, "Transform:", 10))
                sscanf(line + 10, "%15s", o->transform);
            else if (!strncmp(line, "Scale:", 6))
                o->scale = atof(line + 6);
        }
    }
}

static void randr(const char *out, const char *flag, const char *value)
{
    if (!demo())
        run(NULL, 0, "wlr-randr", "--output", out, flag, value);
}

/* --- labwc, foot and the idle timer --------------------------------------- */

static const char *LAYOUTS[] = {"us", "gb", "de", "fr", "es", "it", "pt", "br", "latam", "ca", "ch", "be",
    "nl", "se", "no", "dk", "fi", "pl", "cz", "hu", "ro", "ru", "ua", "tr", "gr", "il", "ara", "ir", "in",
    "jp", "kr", NULL};
static const char *TURNS[] = {"normal", "90", "180", "270", NULL};
static const char *IDLES[] = {"never", "2", "5", "10", "20", "30", NULL};
static const char *CURSORS[] = {"24", "32", "48", "64", NULL};

static void write_labwc(void)
{
    char path[400], line[512];
    FILE *in, *out;

    /* keyboard layout and cursor size: the environment file */
    mkdirs("labwc/environment");
    conf_path(path, sizeof path, "labwc/environment");
    if ((out = fopen(path, "w"))) {
        fprintf(out, "XKB_DEFAULT_LAYOUT=%s\n", get("layout", "us"));
        if (strcmp(get("layout2", "none"), "none"))
            fprintf(out, "XKB_DEFAULT_LAYOUT=%s,%s\nXKB_DEFAULT_OPTIONS=grp:alt_shift_toggle\n",
                    get("layout", "us"), get("layout2", "none"));
        fprintf(out, "XCURSOR_SIZE=%s\nXCURSOR_THEME=Adwaita\n", get("cursor", "24"));
        fclose(out);
    }

    /* touchpad: the system rc.xml with a libinput block added */
    conf_path(path, sizeof path, "labwc/rc.xml");
    in = fopen("/etc/xdg/labwc/rc.xml", "r");
    out = fopen(path, "w");
    while (in && out && fgets(line, sizeof line, in)) {
        if (strstr(line, "</labwc_config>"))
            fprintf(out, "  <libinput>\n    <device category=\"touchpad\">\n"
                         "      <tap>%s</tap>\n      <naturalScroll>%s</naturalScroll>\n"
                         "      <pointerSpeed>%s</pointerSpeed>\n"
                         "      <disableWhileTyping>yes</disableWhileTyping>\n"
                         "    </device>\n  </libinput>\n",
                    strcmp(get("tap", "yes"), "yes") ? "no" : "yes",
                    strcmp(get("natural", "no"), "yes") ? "no" : "yes", get("speed", "0.0"));
        fputs(line, out);
    }
    if (in)
        fclose(in);
    if (out)
        fclose(out);
    if (!demo() && getenv("LABWC_PID"))
        spawn((char *const[]){"labwc", "--reconfigure", NULL});
}

static void write_foot(void)
{
    char path[400];
    FILE *f;

    mkdirs("foot/foot.ini");
    conf_path(path, sizeof path, "foot/foot.ini");
    if ((f = fopen(path, "w"))) {
        fprintf(f, "include=/etc/xdg/foot/foot.ini\n\n[main]\nfont=DejaVu Sans Mono:size=%s\n",
                get("text", "11"));
        fclose(f);
    }
}

static void start_idle(void)
{
    const char *idle = get("idle", "10");
    char secs[16];

    if (demo())
        return;
    run(NULL, 0, "pkill", "-x", "swayidle");
    if (!strcmp(idle, "never"))
        return;
    snprintf(secs, sizeof secs, "%d", atoi(idle) * 60);
    spawn((char *const[]){"swayidle", "-w", "timeout", secs, "wlopm --off '*'",
                          "resume", "wlopm --on '*'", NULL});
}

/* labwc's autostart runs this once the screen is up */
int cmd_apply(void)
{
    load_kv();
    read_outputs();
    for (int i = 0; i < nouts; i++) {
        char k[64];
        const char *v;
        snprintf(k, sizeof k, "mode.%s", outs[i].name);
        if ((v = get(k, NULL)))
            randr(outs[i].name, "--mode", v);
        snprintf(k, sizeof k, "turn.%s", outs[i].name);
        if ((v = get(k, NULL)))
            randr(outs[i].name, "--transform", v);
        snprintf(k, sizeof k, "scale.%s", outs[i].name);
        if ((v = get(k, NULL)))
            randr(outs[i].name, "--scale", v);
    }
    start_idle();
    return 0;
}

/* --- the rows -------------------------------------------------------------- */

enum { R_MODE, R_SCALE, R_TURN, R_TEXT, R_CURSOR, R_LAYOUT, R_LAYOUT2, R_TAP, R_NATURAL, R_SPEED,
       R_ZONE, R_CLOCK, R_IDLE, R_LID, R_ENCRYPT };

static int encrypted(void);

struct row { int kind, out, col; const char *group, *name, *help; };
static struct row rows[32];
static int nrows, sel;

static char zones[700][40];
static int nzones, zone_pick, zone_scroll, choosing;
static struct field zq;
static char zone_now[64] = "UTC", lid_now[16] = "suspend";
static const char *word = "";

static void add_row(int kind, int out, int col, const char *group, const char *name, const char *help)
{
    if (nrows < 32)
        rows[nrows++] = (struct row){kind, out, col, group, name, help};
}

static void read_zone(void)
{
    char link[256];
    ssize_t n = readlink("/etc/localtime", link, sizeof link - 1);
    if (n > 0) {
        link[n] = 0;
        char *p = strstr(link, "zoneinfo/");
        snprintf(zone_now, sizeof zone_now, "%s", p ? p + 9 : link);
    }
    struct conf c;
    load_conf(&c);
    snprintf(lid_now, sizeof lid_now, "%s", c.lid);
}

static void list_zones(void)
{
    static const char *regions[] = {"Africa", "America", "Antarctica", "Asia", "Atlantic", "Australia",
                                    "Europe", "Indian", "Pacific", NULL};
    nzones = 0;
    strcpy(zones[nzones++], "UTC");
    for (const char **r = regions; *r; r++) {
        char dir[128];
        DIR *d;
        struct dirent *e;
        snprintf(dir, sizeof dir, "/usr/share/zoneinfo/%s", *r);
        if (!(d = opendir(dir)))
            continue;
        while ((e = readdir(d)) && nzones < 700)
            if (e->d_name[0] != '.' && e->d_type != DT_DIR)
                snprintf(zones[nzones++], sizeof zones[0], "%s/%s", *r, e->d_name);
        closedir(d);
    }
    qsort(zones + 1, nzones - 1, sizeof zones[0], (int (*)(const void *, const void *))strcmp);
}

void set_open(void)
{
    load_kv();
    read_outputs();
    read_zone();
    nrows = 0;
    for (int i = 0; i < nouts; i++) {
        add_row(R_MODE, i, 0, "SCREEN", "size", "resolution");
        add_row(R_SCALE, i, 0, "SCREEN", "scale", "makes everything bigger or smaller");
        add_row(R_TURN, i, 0, "SCREEN", "rotation", "for screens on their side");
    }
    add_row(R_TEXT, 0, 0, "LOOK", "text size", "this screen and every terminal");
    add_row(R_CURSOR, 0, 0, "LOOK", "pointer size", "how big the mouse pointer is");
    add_row(R_LAYOUT, 0, 1, "KEYBOARD", "layout", "what the keys type");
    add_row(R_LAYOUT2, 0, 1, "KEYBOARD", "second layout", "alt+shift switches between the two");
    add_row(R_TAP, 0, 1, "TOUCHPAD", "tap to click", "a light tap counts as a click");
    add_row(R_NATURAL, 0, 1, "TOUCHPAD", "natural scroll", "content follows the fingers, like a phone");
    add_row(R_SPEED, 0, 1, "TOUCHPAD", "speed", "how fast the pointer moves");
    add_row(R_ZONE, 0, 2, "TIME", "time zone", "enter picks one. typing narrows the list");
    add_row(R_CLOCK, 0, 2, "TIME", "clock", "24 hour or 12 hour");
    add_row(R_IDLE, 0, 2, "POWER", "screen off after", "minutes without touching anything");
    add_row(R_LID, 0, 2, "POWER", "closing the lid", "sleep, or keep running");
    add_row(R_ENCRYPT, 0, 2, "SAVES", "encrypt saves", "a passphrase at boot, or the saved session can't be read");
    if (sel >= nrows)
        sel = 0;
}

static int index_of(const char **list, const char *v)
{
    for (int i = 0; list[i]; i++)
        if (!strcmp(list[i], v))
            return i;
    return 0;
}

static const char *step(const char **list, const char *v, int d)
{
    int n = 0, i = index_of(list, v);
    while (list[n])
        n++;
    return list[(i + d + n) % n];
}

static void value(struct row *r, char *out, size_t n)
{
    struct output *o = &outs[r->out];
    switch (r->kind) {
    case R_MODE: {
        int w, h;
        double hz;
        if (sscanf(o->mode, "%dx%d@%lf", &w, &h, &hz) == 3)
            snprintf(out, n, "%dx%d@%d", w, h, (int)(hz + 0.5));
        else
            snprintf(out, n, "%s", o->mode[0] ? o->mode : "?");
        break;
    }
    case R_SCALE: snprintf(out, n, "%g×", o->scale); break;
    case R_TURN: snprintf(out, n, "%s", !strcmp(o->transform, "normal") ? "none" : o->transform); break;
    case R_TEXT: snprintf(out, n, "%s", get("text", "11")); break;
    case R_CURSOR: snprintf(out, n, "%s", get("cursor", "24")); break;
    case R_LAYOUT: snprintf(out, n, "%s", get("layout", "us")); break;
    case R_LAYOUT2: snprintf(out, n, "%s", get("layout2", "none")); break;
    case R_TAP: snprintf(out, n, "%s", get("tap", "yes")); break;
    case R_NATURAL: snprintf(out, n, "%s", get("natural", "no")); break;
    case R_SPEED: snprintf(out, n, "%+.2f", atof(get("speed", "0.0"))); break;
    case R_ZONE: snprintf(out, n, "%s", zone_now); break;
    case R_CLOCK: snprintf(out, n, "%s", strcmp(get("clock", "24"), "12") ? "24 hour" : "12 hour"); break;
    case R_IDLE: {
        const char *v = get("idle", "10");
        snprintf(out, n, "%s%s", v, strcmp(v, "never") ? " min" : "");
        break;
    }
    case R_LID: snprintf(out, n, "%s", strcmp(lid_now, "suspend") ? "keep running" : "sleep"); break;
    case R_ENCRYPT:
        snprintf(out, n, "%s", !encrypted() ? "no" : access(RUN_DIR "/key", F_OK) && !demo() ? "yes, locked" : "yes");
        break;
    }
}

static int text_changed;
static int pass_step;               /* 1 typing it, 2 typing it again */
static struct field pass1, pass2;

static int encrypted(void)
{
    char line[64];
    FILE *f = fopen(RUN_DIR "/state", "r");
    int on = 0;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "encrypt=", 8))
            on = atoi(line + 8);
    if (f)
        fclose(f);
    return on;
}

static void change(int d)
{
    struct row *r = &rows[sel];
    struct output *o = &outs[r->out];
    char k[64], v[64];

    switch (r->kind) {
    case R_MODE: {
        int i = 0;
        if (!o->nmodes)
            return;
        for (; i < o->nmodes && strcmp(o->modes[i], o->mode); i++)
            ;
        i = ((i < o->nmodes ? i : 0) + d + o->nmodes) % o->nmodes;
        strcpy(o->mode, o->modes[i]);
        snprintf(k, sizeof k, "mode.%s", o->name);
        set(k, o->mode);
        randr(o->name, "--mode", o->mode);
        break;
    }
    case R_SCALE:
        o->scale += d * 0.25;
        if (o->scale < 0.5) o->scale = 0.5;
        if (o->scale > 3) o->scale = 3;
        snprintf(k, sizeof k, "scale.%s", o->name);
        snprintf(v, sizeof v, "%g", o->scale);
        set(k, v);
        randr(o->name, "--scale", v);
        break;
    case R_TURN:
        snprintf(o->transform, sizeof o->transform, "%s", step(TURNS, o->transform, d));
        snprintf(k, sizeof k, "turn.%s", o->name);
        set(k, o->transform);
        randr(o->name, "--transform", o->transform);
        break;
    case R_TEXT: {
        int t = atoi(get("text", "11")) + d;
        if (t < 7) t = 7;
        if (t > 24) t = 24;
        snprintf(v, sizeof v, "%d", t);
        set("text", v);
        write_foot();
        text_changed = 1;
        break;
    }
    case R_CURSOR: set("cursor", step(CURSORS, get("cursor", "24"), d)); write_labwc(); break;
    case R_LAYOUT: set("layout", step(LAYOUTS, get("layout", "us"), d)); write_labwc(); break;
    case R_LAYOUT2: {
        const char *cur = get("layout2", "none");
        int i = strcmp(cur, "none") ? index_of(LAYOUTS, cur) + 1 : 0, n = 0;
        while (LAYOUTS[n])
            n++;
        i = (i + d + n + 1) % (n + 1);
        set("layout2", i ? LAYOUTS[i - 1] : "none");
        write_labwc();
        break;
    }
    case R_TAP: set("tap", strcmp(get("tap", "yes"), "yes") ? "yes" : "no"); write_labwc(); break;
    case R_NATURAL: set("natural", strcmp(get("natural", "no"), "yes") ? "yes" : "no"); write_labwc(); break;
    case R_SPEED: {
        double s = atof(get("speed", "0.0")) + d * 0.25;
        if (s < -1) s = -1;
        if (s > 1) s = 1;
        snprintf(v, sizeof v, "%.2f", s);
        set("speed", v);
        write_labwc();
        break;
    }
    case R_ZONE:
        choosing = 1;
        zq.text[0] = 0;
        zone_pick = zone_scroll = 0;
        if (!nzones)
            list_zones();
        break;
    case R_CLOCK: set("clock", strcmp(get("clock", "24"), "12") ? "12" : "24"); break;
    case R_IDLE: set("idle", step(IDLES, get("idle", "10"), d)); start_idle(); break;
    case R_ENCRYPT:
        if (encrypted() && !access(RUN_DIR "/key", F_OK)) {
            char out[160];
            if (demo() || !run(out, sizeof out, "doas", SELF, "encrypt", "off"))
                toast(MUTED, "saves won't be encrypted from now on");
            else
                toast(RED, "%s", clip(out, 70));
        } else {
            memset(&pass1, 0, sizeof pass1);
            memset(&pass2, 0, sizeof pass2);
            pass1.secret = pass2.secret = 1;
            pass_step = 1;
        }
        break;
    case R_LID: {
        const char *want = strcmp(lid_now, "suspend") ? "suspend" : "ignore";
        if (demo() || !run(NULL, 0, "doas", SELF, "lid-mode", want))
            snprintf(lid_now, sizeof lid_now, "%s", want);
        else
            toast(RED, "couldn't change it");
        break;
    }
    }
    flash(T_ROW);
}

/* the clock style, for the home screen */
int set_clock12(void)
{
    if (!nkv)
        load_kv();
    return !strcmp(get("clock", "24"), "12");
}

static int matches(int i)
{
    return !zq.text[0] || strcasestr(zones[i], zq.text);
}

static void pick_zone(void)
{
    int seen = 0;
    for (int i = 0; i < nzones; i++)
        if (matches(i) && seen++ == zone_pick) {
            if (demo() || !run(NULL, 0, "doas", SELF, "timezone", zones[i])) {
                snprintf(zone_now, sizeof zone_now, "%s", zones[i]);
                setenv("TZ", zones[i], 1);
                tzset();
                toast(GREEN, "time zone is %s now", zones[i]);
            } else {
                toast(RED, "couldn't set it");
            }
            break;
        }
    choosing = 0;
}

static void move(int d)
{
    int col = rows[sel].col;
    for (int i = sel + d; i >= 0 && i < nrows; i += d)
        if (rows[i].col == col) {
            sel = i;
            return;
        }
}

static void sideways(int d)
{
    int col = rows[sel].col + d, pos = 0, seen = 0;
    if (col < 0 || col > 2)
        return;
    for (int i = 0; i < sel; i++)
        pos += rows[i].col == rows[sel].col;
    for (int i = 0; i < nrows; i++)
        if (rows[i].col == col) {
            sel = i;
            if (seen++ >= pos)
                break;
        }
}

int set_key(key k)
{
    if (pass_step) {
        struct field *f = pass_step == 1 ? &pass1 : &pass2;
        if (k.kind == K_ESC) {
            pass_step = 0;
        } else if (k.kind == K_ENTER) {
            if (pass_step == 1) {
                if (strlen(pass1.text) < 6)
                    toast(AMBER, "at least 6 characters");
                else
                    pass_step = 2;
            } else if (strcmp(pass1.text, pass2.text)) {
                toast(RED, "those two don't match");
                memset(&pass2, 0, sizeof pass2);
                pass2.secret = 1;
            } else {
                char line[200], out[160];
                snprintf(line, sizeof line, "%s\n", pass1.text);
                if (demo() || !runv(out, sizeof out, line, (const char *const[]){"doas", SELF, "encrypt", "on", NULL}))
                    toast(GREEN, "saves are encrypted from the next move to the stick on");
                else
                    toast(RED, "%s", clip(out, 70));
                pass_step = 0;
                memset(&pass1, 0, sizeof pass1);
                memset(&pass2, 0, sizeof pass2);
            }
        } else if (k.kind == K_TAB) {
            pass1.show = pass2.show = !pass1.show;
        } else {
            field_key(f, k);
        }
        return 1;
    }
    if (choosing) {
        int n = 0;
        for (int i = 0; i < nzones; i++)
            n += matches(i);
        if (k.kind == K_ESC) choosing = 0;
        else if (k.kind == K_ENTER) pick_zone();
        else if (k.kind == K_DOWN) zone_pick = zone_pick + 1 < n ? zone_pick + 1 : zone_pick;
        else if (k.kind == K_UP) zone_pick = zone_pick > 0 ? zone_pick - 1 : 0;
        else if (k.kind == K_CLICK) {
            hit *h = find_hit(k.x, k.y);
            if (h && h->action == T_ZONE) {
                zone_pick = h->arg;
                pick_zone();
            }
        } else if (field_key(&zq, k))
            zone_pick = zone_scroll = 0;
        return 1;
    }
    if (k.kind == K_CLICK) {
        hit *h = find_hit(k.x, k.y);
        if (!h)
            return 1;
        if (h->action == T_BACK)
            return text_changed ? 2 : 0;
        if (h->action == T_ROW)
            sel = h->arg;
        else if (h->action == T_LESS || h->action == T_MORE) {
            sel = h->arg;
            change(h->action == T_MORE ? 1 : -1);
        }
        return 1;
    }
    switch (k.kind) {
    case K_CTRLC: ui_quit = 1; return 1;
    case K_ESC: return text_changed ? 2 : 0;
    case K_UP: move(-1); return 1;
    case K_DOWN: move(1); return 1;
    case K_LEFT: change(-1); return 1;
    case K_RIGHT: case K_ENTER: change(1); return 1;
    case K_TAB: sideways(1); return 1;
    case K_STAB: sideways(-1); return 1;
    case K_WHEEL: move(k.d); return 1;
    }
    if (k.kind == K_CHAR)
        switch (k.ch) {
        case 'q': return text_changed ? 2 : 0;
        case 'k': move(-1); break;
        case 'j': move(1); break;
        case 'h': case '-': change(-1); break;
        case 'l': case '=': case '+': change(1); break;
        }
    return 1;
}

int set_fps(void)
{
    return choosing || pass_step ? 15 : 2;
}

static void zone_list(canvas *c, int top, int height)
{
    int colw = c->w - 4, n = 0, y = top + 2;

    put(c, 2, top, "TIME ZONE", TEXT, BG, 1);
    put(c, 12, top, "/", SIGNAL, BG, 1);
    field_draw(&zq, c, 13, top, 40, BG);
    for (int j = 0; j < colw; j++)
        put(c, 1 + j, top + 1, "▔", SIGNAL, BG, 0);
    int rowsn = height - 2, cols = c->w / 34 ? c->w / 34 : 1, per = rowsn * cols;
    if (zone_pick < zone_scroll) zone_scroll = zone_pick;
    if (zone_pick >= zone_scroll + per) zone_scroll = zone_pick - per + 1;
    for (int i = 0; i < nzones; i++) {
        if (!matches(i))
            continue;
        int idx = n++;
        if (idx < zone_scroll || idx >= zone_scroll + per)
            continue;
        int slot = idx - zone_scroll, x = 1 + (slot / rowsn) * 34;
        int yy = y + slot % rowsn, on = idx == zone_pick;
        rgb bg = on ? PANEL2 : BG;
        fill(c, x, yy, 32, 1, bg);
        put(c, x, yy, "┃", on ? SIGNAL : bg, bg, 0);
        put(c, x + 2, yy, clip(zones[i], 29), on ? TEXT : MUTED, bg, on);
        add_hit(x, yy, 32, T_ZONE, idx);
    }
    if (!n)
        put(c, 2, y, "nothing by that name", DIM, BG, 0);
}

void set_draw(canvas *c)
{
    int colw = (c->w - 2) / 3, top = 6, drawer_h = 6, end, tx, x;
    char line[160], v[64];
    time_t now = time(NULL);

    if (strcmp(word, "SETTINGS")) {
        word = "SETTINGS";
        ui_word_t = ui_t();
    }
    end = pixel_word(c, 2, 0, "SETTINGS", SIGNAL, BG, ui_t() - ui_word_t);
    tx = end + 4;
    snprintf(line, sizeof line, "%d screen%s, %s keyboard, %s", nouts, nouts == 1 ? "" : "s",
             get("layout", "us"), zone_now);
    put(c, tx, 0, clip(line, c->w - tx - 2), TEXT, BG, 1);
    strftime(line, sizeof line, "it's %H:%M here", localtime(&now));
    put(c, tx, 1, clip(line, c->w - tx - 2), MUTED, BG, 0);
    x = ui_link(c, 1, 4, "←→", "change", T_ROW, SIGNAL, BG);
    x = ui_link(c, x, 4, "tab", "next column", T_ROW, MUTED, BG);
    ui_link(c, x, 4, "esc", "back", T_BACK, MUTED, BG);

    if (choosing) {
        zone_list(c, top, c->h - top - 1);
        return;
    }

    for (int col = 0; col < 3; col++) {
        int cx = 1 + col * colw, cw = colw - 2, y = top;
        const char *group = "";
        int last_out = -1;
        for (int i = 0; i < nrows; i++) {
            struct row *r = &rows[i];
            int on = i == sel;
            rgb bg = on ? PANEL2 : BG;
            if (r->col != col)
                continue;
            if (strcmp(group, r->group) || (r->kind <= R_TURN && r->out != last_out)) {
                if (y > top)
                    y++;
                group = r->group;
                last_out = r->out;
                if (r->kind <= R_TURN && nouts) {
                    snprintf(line, sizeof line, "SCREEN %s", outs[r->out].name);
                    put(c, cx + 1, y, line, MUTED, BG, 1);
                } else {
                    put(c, cx + 1, y, group, MUTED, BG, 1);
                }
                y++;
            }
            if (y >= c->h - drawer_h - 1)
                break;
            value(r, v, sizeof v);
            fill(c, cx, y, cw, 1, bg);
            put(c, cx, y, "┃", on ? SIGNAL : bg, bg, 0);
            put(c, cx + 2, y, r->name, on ? TEXT : MUTED, bg, on);
            int vw = ulen(v) + 4;
            put(c, cx + cw - vw - 1, y, "‹", on ? SIGNAL : DIM, bg, 1);
            put(c, cx + cw - vw + 1, y, v, on ? TEXT : MUTED, bg, on);
            put(c, cx + cw - 2, y, "›", on ? SIGNAL : DIM, bg, 1);
            add_hit(cx, y, cw - vw - 1, T_ROW, i);
            add_hit(cx + cw - vw - 1, y, 2, T_LESS, i);
            add_hit(cx + cw - vw + 1, y, vw - 2, T_MORE, i);
            y++;
        }
        if (col == 0 && !nouts)
            put(c, cx + 1, top + 1, "no screens to set up (not on a wayland desktop)", DIM, BG, 0);
    }

    {
        int dtop = c->h - drawer_h, y = dtop + 1;
        struct row *r = nrows ? &rows[sel] : NULL;
        fill(c, 0, dtop, c->w, drawer_h, PANEL);
        for (int i = 0; i < c->w; i++)
            put(c, i, dtop, "▀", mix(BG, LINE, 0.8), BG, 0);
        if (r) {
            value(r, v, sizeof v);
            x = put(c, 2, y, r->name, TEXT, PANEL, 1);
            put(c, x + 2, y, v, SIGNAL, PANEL, 0);
            if (r->kind <= R_TURN)
                put(c, x + 4 + ulen(v), y, clip(outs[r->out].desc, c->w - x - 10), DIM, PANEL, 0);
            put(c, 2, y + 1, clip(r->help, c->w - 4), MUTED, PANEL, 0);
            if (pass_step) {
                put(c, 2, y + 3, pass_step == 1 ? "passphrase" : "once more", TEXT, PANEL, 1);
                field_draw(pass_step == 1 ? &pass1 : &pass2, c, 14, y + 3, 40, PANEL2);
                put(c, 56, y + 3, "tab shows it, esc stops", DIM, PANEL, 0);
            } else if (text_changed)
                put(c, 2, y + 3, "the home screen restarts with the new text size when you leave", AMBER, PANEL, 0);
        }
    }
}
