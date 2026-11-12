/* The fix page: copying disks, repairing what doesn't boot, checking disks,
 * wiping them, and updating lifeboat. Each tool here only picks what to work
 * on; the work runs as root in a terminal of its own (tools.c), which asks
 * once more before anything destructive.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "boat.h"
#include "extras.h"
#include "ui.h"

enum { F_ITEM = 500, F_ROW, F_BACK, F_CLOSE };

enum { T_CLONE_DISK, T_CLONE_PART, T_TO_FILE, T_FROM_FILE, T_BOOT, T_FSCK, T_PASSWD, T_CHROOT,
       T_SCAN, T_BENCH, T_SURFACE, T_WIPE, T_UPDATE, T_DRIVERS };

static const struct { int col; const char *group, *name, *help; } T[] = {
    {0, "COPY", "clone a disk", "a whole disk onto another, partitions and all"},
    {0, "COPY", "clone a partition", "one partition onto another"},
    {0, "COPY", "save to a file", "a disk or partition into an image file on another disk"},
    {0, "COPY", "put a file back", "an image file back onto a disk or partition"},
    {1, "REPAIR", "fix booting", "reinstall the boot loader of an installed linux or windows"},
    {1, "REPAIR", "check and repair", "the right fsck for whatever filesystem it is"},
    {1, "REPAIR", "reset a password", "on a linux installed on one of the disks"},
    {1, "REPAIR", "chroot into it", "a root shell inside an installed linux"},
    {2, "CHECK", "scan for viruses", "clamav, downloads first (180M)"},
    {2, "CHECK", "speed test", "how fast a disk reads. reads only"},
    {2, "CHECK", "surface scan", "maps slow and broken areas. reads only"},
    {2, "WIPE", "wipe a disk", "erase it for good, before it goes to someone else"},
    {2, "LIFEBOAT", "update lifeboat", "from a newer iso or patch, a link, or the latest gitlab release. used next boot"},
    {2, "LIFEBOAT", "find drivers", "loads a driver for every device and leaves a report on the stick"},
};
#define NT ((int)(sizeof T / sizeof *T))

/* --- disks ------------------------------------------------------------------ */

struct dev { char path[64], label[200], right[48], fs[24], mount[160]; long long size; int disk; };
static struct dev devs[64];
static int ndevs;

static void read_devs(void)
{
    static char out[16384];
    ndevs = 0;
    if (run(out, sizeof out, "lsblk", "-P", "-b", "-p", "-o", "NAME,TYPE,SIZE,FSTYPE,LABEL,MOUNTPOINT,MODEL,TRAN"))
        return;
    for (char *line = strtok(out, "\n"); line && ndevs < 64; line = strtok(NULL, "\n")) {
        char k[8][160] = {{0}};
        const char *keys[] = {"NAME", "TYPE", "SIZE", "FSTYPE", "LABEL", "MOUNTPOINT", "MODEL", "TRAN"};
        for (int i = 0; i < 8; i++) {
            char want[24], *p;
            snprintf(want, sizeof want, "%s=\"", keys[i]);
            if ((p = strstr(line, want))) {
                p += strlen(want);
                snprintf(k[i], sizeof k[i], "%.*s", (int)strcspn(p, "\""), p);
            }
        }
        if (strcmp(k[1], "disk") && strcmp(k[1], "part") && strcmp(k[1], "crypt"))
            continue;
        if (strstr(k[0], "zram") || strstr(k[0], "/loop") || !strcmp(k[3], "squashfs"))
            continue;
        struct dev *d = &devs[ndevs++];
        snprintf(d->path, sizeof d->path, "%s", k[0]);
        snprintf(d->fs, sizeof d->fs, "%s", k[3]);
        snprintf(d->mount, sizeof d->mount, "%s", k[5]);
        d->size = atoll(k[2]);
        d->disk = !strcmp(k[1], "disk");
        if (d->disk)
            snprintf(d->label, sizeof d->label, "%s  %s", k[0] + 5, k[6][0] ? k[6] : k[7]);
        else
            snprintf(d->label, sizeof d->label, "  %s  %s%s%s", k[0] + 5, k[3], k[4][0] ? " " : "", k[4]);
        snprintf(d->right, sizeof d->right, "%s%s", human(d->size), d->mount[0] ? " mounted" : "");
    }
}

/* --- the wizard: a sheet with a list, one step at a time ----------------------- */

struct row { char label[200], right[64], value[300]; int ok; };
static struct row rows[96];
static int nrows, sel_row, scroll_row;
static int tool = -1, step;
static char pick1[300], pick2[300], pick3[32];
static char ask[160], note[200];
static int typing;
static struct field input;
static double sheet_t;

static int sel;
static const char *word = "";

static void add_row(const char *label, const char *right, const char *value, int ok)
{
    if (nrows < 96) {
        struct row *r = &rows[nrows++];
        snprintf(r->label, sizeof r->label, "%s", label);
        snprintf(r->right, sizeof r->right, "%s", right);
        snprintf(r->value, sizeof r->value, "%s", value);
        r->ok = ok;
    }
}

static void list_devs(int disks, int parts, const char *skip)
{
    read_devs();
    for (int i = 0; i < ndevs; i++) {
        struct dev *d = &devs[i];
        if ((d->disk && !disks) || (!d->disk && !parts) || (skip && !strcmp(skip, d->path)))
            continue;
        add_row(d->label, d->right, d->path, 1);
    }
}

/* writable mounted places, for an image file to go */
static void list_places(void)
{
    read_devs();
    for (int i = 0; i < ndevs; i++)
        if (devs[i].mount[0] && strncmp(devs[i].mount, "/run/lifeboat", 13) && strcmp(devs[i].mount, "/")) {
            char right[48];
            snprintf(right, sizeof right, "%s free", human(free_bytes(devs[i].mount)));
            add_row(devs[i].mount, right, devs[i].mount, !access(devs[i].mount, W_OK) || demo());
        }
    add_row("~ (this session, in ram)", human(free_bytes("/home/" USER)), "/home/" USER, 1);
}

/* image files lying around on mounted disks */
static void find_files(const char *dir, int depth, const char *const *ext)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && nrows < 96) {
        char path[600];
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR && depth > 0)
            find_files(path, depth - 1, ext);
        else if (e->d_type == DT_REG)
            for (int i = 0; ext[i]; i++) {
                size_t l = strlen(e->d_name), el = strlen(ext[i]);
                if (l > el && !strcasecmp(e->d_name + l - el, ext[i])) {
                    long long sz = 0;
                    FILE *f = fopen(path, "r");
                    if (f) {
                        fseek(f, 0, SEEK_END);
                        sz = ftell(f);
                        fclose(f);
                    }
                    add_row(path, human(sz), path, 1);
                }
            }
    }
    if (d)
        closedir(d);
}

static void list_files(const char *const *ext)
{
    read_devs();
    find_files("/home/" USER, 2, ext);
    for (int i = 0; i < ndevs; i++)
        if (devs[i].mount[0] && strncmp(devs[i].mount, "/run/lifeboat", 13) && strcmp(devs[i].mount, "/"))
            find_files(devs[i].mount, 1, ext);
}

static void term(const char *cmd)
{
    char line[1200];
    if (demo()) {
        toast(MUTED, "demo: %s", clip(cmd, 70));
        return;
    }
    snprintf(line, sizeof line, "%s; echo; printf 'enter closes this '; read x", cmd);
    spawn((char *const[]){"foot", "-e", "sh", "-c", line, NULL});
}

static void go_step(void);

static void start(int t)
{
    tool = t;
    step = 0;
    pick1[0] = pick2[0] = pick3[0] = 0;
    sheet_t = ui_t();
    go_step();
}

static void finish(const char *cmd)
{
    term(cmd);
    tool = -1;
}

/* fill the sheet for the current step of the current tool */
static void go_step(void)
{
    static const char *const imgs[] = {".img", ".pcl.zst", NULL};
    static const char *const isos[] = {".iso", ".patch", NULL};
    char cmd[900];

    nrows = sel_row = scroll_row = 0;
    typing = 0;
    note[0] = 0;
    memset(&input, 0, sizeof input);

    switch (tool) {
    case T_CLONE_DISK:
    case T_CLONE_PART: {
        int disk = tool == T_CLONE_DISK;
        if (step == 0) {
            snprintf(ask, sizeof ask, "copy from which %s?", disk ? "disk" : "partition");
            list_devs(disk, !disk, NULL);
        } else if (step == 1) {
            snprintf(ask, sizeof ask, "onto which %s? everything on it gets replaced", disk ? "disk" : "partition");
            list_devs(disk, !disk, pick1);
        } else if (step == 2) {
            snprintf(ask, sizeof ask, "how?");
            add_row("only the used space", "quick, healthy disks", "used", 1);
            add_row("every byte", "slow, failing disks", "all", 1);
        } else {
            snprintf(cmd, sizeof cmd, "doas " SELF " clone '%s' '%s' %s", pick1, pick2, pick3);
            finish(cmd);
        }
        break;
    }
    case T_TO_FILE:
        if (step == 0) {
            snprintf(ask, sizeof ask, "save which disk or partition?");
            list_devs(1, 1, NULL);
        } else if (step == 1) {
            snprintf(ask, sizeof ask, "where does the file go?");
            list_places();
            snprintf(note, sizeof note, "a whole disk becomes a raw .img, a partition a compressed .pcl.zst");
        } else {
            char stamp[32], file[400];
            time_t now = time(NULL);
            int disk = 0;
            for (int i = 0; i < ndevs; i++)
                if (!strcmp(devs[i].path, pick1))
                    disk = devs[i].disk;
            strftime(stamp, sizeof stamp, "%Y%m%d-%H%M", localtime(&now));
            snprintf(file, sizeof file, "%s/%s-%s.%s", pick2, strrchr(pick1, '/') + 1, stamp,
                     disk ? "img" : "pcl.zst");
            snprintf(cmd, sizeof cmd, "doas " SELF " image '%s' '%s'", pick1, file);
            finish(cmd);
        }
        break;
    case T_FROM_FILE:
        if (step == 0) {
            snprintf(ask, sizeof ask, "which image file?");
            list_files(imgs);
            if (!nrows)
                snprintf(note, sizeof note, "no .img or .pcl.zst files on any mounted disk. mount the one with it first");
        } else if (step == 1) {
            snprintf(ask, sizeof ask, "onto what? everything on it gets replaced");
            list_devs(1, 1, NULL);
        } else {
            snprintf(cmd, sizeof cmd, "doas " SELF " restore '%s' '%s'", pick1, pick2);
            finish(cmd);
        }
        break;
    case T_BOOT:
        if (step == 0) {
            static char out[4096];
            snprintf(ask, sizeof ask, "which system doesn't boot?");
            if (demo())
                snprintf(out, sizeof out, "linux /dev/nvme0n1p5 Arch Linux\nwindows /dev/nvme0n1p1 Windows\n");
            else if (run(out, sizeof out, "doas", SELF, "systems"))
                out[0] = 0;
            for (char *p = strtok(out, "\n"); p; p = strtok(NULL, "\n")) {
                char kind[16], dev[64], name[160] = "";
                if (sscanf(p, "%15s %63s %159[^\n]", kind, dev, name) >= 2) {
                    char label[220];
                    snprintf(label, sizeof label, "%s", name);
                    add_row(label, dev + 5, p, 1);
                }
            }
            if (!nrows)
                snprintf(note, sizeof note, "no installed linux or windows found on these disks");
        } else {
            char kind[16], dev[64];
            sscanf(pick1, "%15s %63s", kind, dev);
            if (!strcmp(kind, "windows"))
                snprintf(cmd, sizeof cmd, "doas " SELF " fixboot windows '%s'", dev);
            else
                snprintf(cmd, sizeof cmd, "doas " SELF " fixboot '%s'", dev);
            finish(cmd);
        }
        break;
    case T_FSCK:
    case T_PASSWD:
    case T_CHROOT:
    case T_SCAN:
        if (step == 0) {
            snprintf(ask, sizeof ask, "%s", tool == T_FSCK ? "check which partition?" :
                     tool == T_SCAN ? "scan which partition?" : "which partition has the system?");
            list_devs(0, 1, NULL);
        } else {
            snprintf(cmd, sizeof cmd, "doas " SELF " %s '%s'%s",
                     tool == T_FSCK ? "fsck" : tool == T_SCAN ? "scan" : "chroot", pick1,
                     tool == T_PASSWD ? " passwd" : "");
            finish(cmd);
        }
        break;
    case T_BENCH:
    case T_SURFACE:
        if (step == 0) {
            snprintf(ask, sizeof ask, "which disk?");
            list_devs(1, 0, NULL);
        } else {
            snprintf(cmd, sizeof cmd, "doas " SELF " %s '%s'", tool == T_BENCH ? "bench" : "surface", pick1);
            if (tool == T_SURFACE && !demo()) {
                spawn((char *const[]){"foot", "--app-id", "lifeboat-surface", "doas", SELF, "surface", pick1, NULL});
                tool = -1;
            } else {
                finish(cmd);
            }
        }
        break;
    case T_WIPE:
        if (step == 0) {
            snprintf(ask, sizeof ask, "wipe which disk?");
            list_devs(1, 0, NULL);
        } else if (step == 1) {
            snprintf(ask, sizeof ask, "how?");
            add_row("quick", "ssds: tell it everything's free", "quick", 1);
            add_row("secure erase", "the drive erases itself", "secure", 1);
            add_row("zeros", "overwrite all of it, any disk", "zeros", 1);
        } else {
            snprintf(cmd, sizeof cmd, "doas " SELF " wipe '%s' %s", pick1, pick2);
            finish(cmd);
        }
        break;
    case T_UPDATE:
        if (step == 0) {
            snprintf(ask, sizeof ask, "a lifeboat iso or patch, type a link, or the newest build");
            add_row("latest from gitlab", "downloads the newest build, no file needed", "gitlab", 1);
            list_files(isos);
            typing = 1;
            snprintf(note, sizeof note, "typing a link (https://...) and enter downloads it");
        } else {
            snprintf(cmd, sizeof cmd, "doas " SELF " update '%s'", pick1);
            finish(cmd);
        }
        break;
    case T_DRIVERS:
        finish(SELF " drivers");
        break;
    }
}

static void choose(int i)
{
    const char *v;
    if (typing && input.text[0])
        v = input.text;
    else if (i < nrows && rows[i].ok)
        v = rows[i].value;
    else
        return;
    if (step == 0) snprintf(pick1, sizeof pick1, "%s", v);
    else if (step == 1) snprintf(pick2, sizeof pick2, "%s", v);
    else snprintf(pick3, sizeof pick3, "%s", v);
    step++;
    go_step();
}

/* --- keys ---------------------------------------------------------------------------- */

static void move(int d)
{
    int col = T[sel].col;
    for (int i = sel + d; i >= 0 && i < NT; i += d)
        if (T[i].col == col) {
            sel = i;
            return;
        }
}

static void sideways(int d)
{
    int col = T[sel].col + d, pos = 0, seen = 0;
    if (col < 0 || col > 2)
        return;
    for (int i = 0; i < sel; i++)
        pos += T[i].col == T[sel].col;
    for (int i = 0; i < NT; i++)
        if (T[i].col == col) {
            sel = i;
            if (seen++ >= pos)
                break;
        }
}

int fix_key(key k)
{
    if (tool >= 0) {
        if (k.kind == K_ESC || k.kind == K_CTRLC) {
            if (step > 0) {
                step--;
                go_step();
            } else {
                tool = -1;
            }
        } else if (k.kind == K_CLICK) {
            hit *h = find_hit(k.x, k.y);
            if (h && h->action == F_ROW) choose(h->arg);
            if (h && h->action == F_CLOSE) tool = -1;
        } else if (k.kind == K_ENTER) {
            choose(sel_row);
        } else if (k.kind == K_UP) {
            if (sel_row > 0) sel_row--;
        } else if (k.kind == K_DOWN) {
            if (sel_row + 1 < nrows) sel_row++;
        } else if (k.kind == K_WHEEL) {
            sel_row += k.d;
            if (sel_row < 0) sel_row = 0;
            if (sel_row >= nrows) sel_row = nrows ? nrows - 1 : 0;
        } else if (typing) {
            field_key(&input, k);
        } else if (k.kind == K_CHAR && k.ch >= '1' && k.ch <= '9' && (int)k.ch - '1' < nrows) {
            choose(k.ch - '1');
        } else if (k.kind == K_CHAR && (k.ch == 'j' || k.ch == 'k')) {
            sel_row += k.ch == 'j' ? 1 : -1;
            if (sel_row < 0) sel_row = 0;
            if (sel_row >= nrows) sel_row = nrows ? nrows - 1 : 0;
        }
        return 1;
    }
    if (k.kind == K_CLICK) {
        hit *h = find_hit(k.x, k.y);
        if (h && h->action == F_BACK)
            return 0;
        if (h && h->action == F_ITEM) {
            if (sel == h->arg)
                start(sel);
            sel = h->arg;
        }
        return 1;
    }
    switch (k.kind) {
    case K_CTRLC: ui_quit = 1; return 1;
    case K_ESC: return 0;
    case K_ENTER: start(sel); return 1;
    case K_UP: move(-1); return 1;
    case K_DOWN: move(1); return 1;
    case K_RIGHT: case K_TAB: sideways(1); return 1;
    case K_LEFT: case K_STAB: sideways(-1); return 1;
    case K_WHEEL: move(k.d); return 1;
    }
    if (k.kind == K_CHAR)
        switch (k.ch) {
        case 'q': case 'f': return 0;
        case 'j': move(1); break;
        case 'k': move(-1); break;
        case 'l': sideways(1); break;
        case 'h': sideways(-1); break;
        }
    return 1;
}

int fix_fps(void)
{
    return tool >= 0 && (typing || ui_t() - sheet_t < 0.4) ? 30 : 2;
}

void fix_open(void)
{
    tool = -1;
}

/* --- drawing --------------------------------------------------------------------- */

static void draw_sheet(canvas *c)
{
    int w = c->w < 70 ? c->w : 66, x, y = 6;
    double in = (ui_t() - sheet_t) / 0.18;
    if (in > 1) in = 1;
    in = 1 - (1 - in) * (1 - in);
    x = c->w - (int)(w * in);
    dim(c, 0, 0, x, c->h, 0.55 * in);
    fill(c, x, 0, w, c->h, PANEL);
    for (int yy = 0; yy < c->h; yy++)
        put(c, x, yy, "▌", mix(BG, SIGNAL, 0.6), PANEL, 0);

    const char *big = tool <= T_FROM_FILE ? "COPY" : tool <= T_CHROOT ? "FIX" : tool == T_WIPE ? "WIPE" :
                      tool == T_UPDATE ? "UPDATE" : "CHECK";
    pixel_word(c, x + 3, 1, big, tool == T_WIPE ? RED : SIGNAL, PANEL, ui_t() - sheet_t);
    put(c, x + 3, y - 1, clip(T[tool].name, w - 6), MUTED, PANEL, 0);
    y += wrap(c, x + 3, y, w - 6, ask, TEXT, PANEL);
    if (pick1[0] && step > 0) {
        char was[200];
        snprintf(was, sizeof was, "from %s", pick1);
        put(c, x + 3, y++, clip(was, w - 6), DIM, PANEL, 0);
    }
    y++;
    if (typing) {
        field_draw(&input, c, x + 3, y, w - 6, PANEL2);
        y += 2;
    }
    int room = c->h - y - 5;
    if (sel_row < scroll_row) scroll_row = sel_row;
    if (sel_row >= scroll_row + room) scroll_row = sel_row - room + 1;
    for (int i = scroll_row; i < nrows && i < scroll_row + room; i++, y++) {
        int on = i == sel_row && !(typing && input.text[0]);
        rgb bg = on ? PANEL2 : PANEL;
        int rw = ulen(rows[i].right);
        fill(c, x + 2, y, w - 4, 1, bg);
        put(c, x + 2, y, "┃", on ? SIGNAL : LINE, bg, 0);
        if (i < 9)
            putf(c, x + 4, y, on ? SIGNAL : DIM, bg, 1, "%d", i + 1);
        put(c, x + 6, y, clip(rows[i].label, w - 12 - rw), !rows[i].ok ? DIM : on ? TEXT : MUTED, bg, on);
        put(c, x + w - 3 - rw, y, rows[i].right, DIM, bg, 0);
        add_hit(x + 2, y, w - 4, F_ROW, i);
    }
    if (note[0])
        wrap(c, x + 3, c->h - 4, w - 6, note, nrows ? DIM : AMBER, PANEL);
    int lx = ui_link(c, x + 2, c->h - 2, "enter", "next", F_ROW, SIGNAL, PANEL);
    ui_link(c, lx, c->h - 2, "esc", step ? "back a step" : "never mind", F_CLOSE, MUTED, PANEL);
}

void fix_draw(canvas *c)
{
    int colw = (c->w - 2) / 3, top = 6, drawer_h = 6, end, x;

    if (strcmp(word, "FIX")) {
        word = "FIX";
        ui_word_t = ui_t();
    }
    end = pixel_word(c, 2, 0, "FIX", SIGNAL, BG, ui_t() - ui_word_t);
    put(c, end + 4, 0, "copy, repair, check and wipe disks", TEXT, BG, 1);
    x = ui_link(c, 1, 4, "enter", "start", F_ITEM, SIGNAL, BG);
    ui_link(c, x, 4, "esc", "back", F_BACK, MUTED, BG);

    for (int col = 0; col < 3; col++) {
        int cx = 1 + col * colw, cw = colw - 2, y = top;
        const char *group = "";
        for (int i = 0; i < NT; i++) {
            if (T[i].col != col)
                continue;
            if (strcmp(group, T[i].group)) {
                if (y > top)
                    y++;
                group = T[i].group;
                put(c, cx + 1, y, group, i == T_WIPE ? mix(BG, RED, 0.8) : MUTED, BG, 1);
                for (int j = ulen(group) + 2; j < cw; j++)
                    put(c, cx + j, y, "▔", LINE, BG, 0);
                y++;
            }
            int on = i == sel;
            rgb bg = on ? PANEL2 : BG;
            fill(c, cx, y, cw, 1, bg);
            put(c, cx, y, "┃", on ? (i == T_WIPE ? RED : SIGNAL) : bg, bg, 0);
            put(c, cx + 2, y, T[i].name, on ? TEXT : MUTED, bg, on);
            add_hit(cx, y, cw, F_ITEM, i);
            y++;
        }
    }

    int dtop = c->h - drawer_h;
    fill(c, 0, dtop, c->w, drawer_h, PANEL);
    for (int i = 0; i < c->w; i++)
        put(c, i, dtop, "▀", mix(BG, LINE, 0.8), BG, 0);
    put(c, 2, dtop + 1, T[sel].name, TEXT, PANEL, 1);
    wrap(c, 2, dtop + 2, c->w - 4, T[sel].help, MUTED, PANEL);

    if (tool >= 0)
        draw_sheet(c);
}
