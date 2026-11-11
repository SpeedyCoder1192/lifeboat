/* The home screen. It is the whole desktop shell: launcher, wifi, disks,
 * volume, the clock, and where this session's changes live. Gui apps open on
 * top of it, super+space comes back here.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "extras.h"
#include "pkgs.h"
#include "probe.h"
#include "ui.h"
#include "wins.h"

enum {
    A_MOVE = 1, A_FIND, A_TERM, A_OFF, A_VOL_DOWN, A_VOL_UP, A_LIGHT_DOWN, A_LIGHT_UP,
    A_COL, A_ITEM, A_GO, A_WRITABLE, A_UNMOUNT, A_FORGET, A_SCAN, A_ROW, A_CLOSE, A_SHOW,
    A_PKGS, A_MUTE, A_VOL_SET, A_LIGHT_SET, A_TOOLS, A_TOOL, A_BT, A_AUDIO, A_WIN, A_WHOLE,
    A_EXTRAS, A_SETTINGS, A_CANCEL, A_FIX
};
enum { APPS, WIFI, DISKS };
enum { NO_SHEET, S_OFF, S_KEY, S_INSTALL, S_BT, S_LUKS, S_COPY, S_AUDIO };

static struct snap S;
static int col, sel[3], scroll[3], count[3];
static int shown[300], nshown;
static int finding;
static struct field query;

static int sheet, sheet_row, sheet_only, leaving;
static int page;            /* 0 home, 1 packages, 2 extras, 3 settings, 4 fix */
static int key_tried, key_failed;
static double knob_t[2];

static int tools_open;      /* the rescue tools, tucked into the bottom drawer */
static double tools_t;
static struct disk tool_disk;
static int copy_whole, cand[48], ncand;
static struct sink sinks[12];
static int nsinks;
static struct win wins[9];
static int nwins;

static const char *TOOLS[][2] = {
    {"chroot into it", "a root shell inside the system installed there"},
    {"reset a password", "pick a user on it, type a new one"},
    {"check and repair", "the right fsck for whatever is on it"},
    {"copy it somewhere safe", "ddrescue, skips bad spots"},
    {"find lost files", "testdisk, for deleted files and lost partitions"},
    {"full health report", "everything the disk says about itself"},
    {"edit partitions", "gparted, on the whole disk"},
    {"unlock it", "open an encrypted partition with its password"},
    {"make it the save stick", "adds a save partition, deletes nothing"},
};
static double sheet_t;
static struct field input;
static struct net key_for;

static const char *word = "";
static long seen_error, seen_note;
static int was_moving;

static const char *TITLES[] = {"APPS", "WIFI", "DISKS"};

/* --- doing things ------------------------------------------------------- */

static void root_bg(const char *a, const char *b, const char *c)
{
    if (demo()) {
        toast(MUTED, "demo: doas lifeboat %s %s %s", a, b ? b : "", c ? c : "");
        return;
    }
    spawn((char *const[]){"doas", SELF, "--bg", (char *)a, (char *)b, (char *)c, NULL});
}

static void open_sheet(int which)
{
    sheet = which;
    sheet_row = 0;
    sheet_t = ui_t();
    memset(&input, 0, sizeof input);
    input.secret = which == S_KEY || which == S_LUKS;
    if (which == S_BT)
        probe_bt_scan(1);
    if (which == S_AUDIO)
        nsinks = probe_sinks(sinks, 12);
}

/* something that wants watching: its own terminal, which waits before closing */
static void term(const char *cmd)
{
    char line[900];

    if (demo()) {
        toast(MUTED, "demo: %s", clip(cmd, 64));
        return;
    }
    snprintf(line, sizeof line, "%s; echo; printf 'enter closes this '; read x", cmd);
    spawn((char *const[]){"foot", "-e", "sh", "-c", line, NULL});
}

static int list_rows(void)
{
    switch (sheet) {
    case S_OFF: return 4;
    case S_BT: return S.bt && S.bt_on ? S.nbt : 1;
    case S_COPY: return ncand;
    case S_AUDIO: return nsinks;
    }
    return 0;
}

static void tool(int n)
{
    struct disk *d = col == DISKS && count[DISKS] ? &S.disks[sel[DISKS]] : NULL;
    char cmd[400];
    int locked;

    if (!d)
        return;
    locked = !strcmp(d->fs, "crypto_LUKS");
    if (n <= 2 && locked) {
        toast(AMBER, "it's encrypted. unlock it first, that's 8");
        return;
    }
    if (n <= 2 && d->mount[0]) {
        toast(AMBER, "unmount it first, that's u");
        return;
    }
    switch (n) {
    case 0: snprintf(cmd, sizeof cmd, "doas " SELF " chroot '%s'", d->dev); term(cmd); break;
    case 1: snprintf(cmd, sizeof cmd, "doas " SELF " chroot '%s' passwd", d->dev); term(cmd); break;
    case 2: snprintf(cmd, sizeof cmd, "doas " SELF " fsck '%s'", d->dev); term(cmd); break;
    case 3:
        /* anywhere mounted that isn't on the disk being copied */
        tool_disk = *d;
        copy_whole = 0;
        ncand = 0;
        for (int i = 0; i < S.ndisks; i++)
            if (S.disks[i].mount[0] && strcmp(S.disks[i].parent, d->parent))
                cand[ncand++] = i;
        open_sheet(S_COPY);
        break;
    case 4: snprintf(cmd, sizeof cmd, "doas testdisk '%s'", d->parent); term(cmd); break;
    case 5: snprintf(cmd, sizeof cmd, "doas smartctl -x '%s' | less", d->parent); term(cmd); break;
    case 6:
        if (demo()) {
            toast(MUTED, "demo: doas gparted %s", d->parent);
        } else {
            spawn((char *const[]){"doas", "gparted", d->parent, NULL});
            toast(MUTED, "starting gparted");
        }
        break;
    case 8:
        snprintf(cmd, sizeof cmd, "doas " SELF " persist '%s' && echo && echo done. "
                 "sessions can be saved on it now", d->parent);
        term(cmd);
        probe_poke();
        break;
    case 7:
        if (!locked) {
            toast(MUTED, "it isn't encrypted");
        } else {
            tool_disk = *d;
            key_failed = 0;
            open_sheet(S_LUKS);
        }
        break;
    }
}

static void launch(struct appent *a)
{
    if (!strcmp(a->exec, "@install")) {
        open_sheet(S_INSTALL);
        return;
    }
    if (!strcmp(a->exec, "@extras")) {
        extras_open();
        page = 2;
        return;
    }
    if (!strcmp(a->exec, "@fix")) {
        fix_open();
        page = 4;
        return;
    }
    if (!strcmp(a->exec, "@settings")) {
        set_open();
        page = 3;
        return;
    }
    if (!strcmp(a->exec, "@pkgs")) {
        pkgs_open();
        page = 1;
        return;
    }
    if (!strcmp(a->exec, "@bt") || !strcmp(a->exec, "@audio")) {
        open_sheet(a->exec[1] == 'b' ? S_BT : S_AUDIO);
        return;
    }
    if (a->term)
        spawn((char *const[]){"foot", "-e", "sh", "-c", a->exec, NULL});
    else
        spawn((char *const[]){"sh", "-c", a->exec, NULL});
    toast(MUTED, "starting %s", a->name);
}

static void knob(int action, int arg)
{
    char pct[16];

    snprintf(pct, sizeof pct, "%d%%", arg);
    if (demo()) {
        toast(MUTED, "demo: no knobs to turn");
        return;
    }
    switch (action) {
    case A_VOL_UP: case A_VOL_DOWN:
        run(NULL, 0, "wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@",
            action == A_VOL_UP ? "5%+" : "5%-");
        break;
    case A_VOL_SET:
        run(NULL, 0, "wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", pct);
        break;
    case A_MUTE:
        run(NULL, 0, "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle");
        break;
    case A_LIGHT_SET:
        run(NULL, 0, "brightnessctl", "-q", "-c", "backlight", "set", pct);
        break;
    default:
        run(NULL, 0, "brightnessctl", "-q", "-c", "backlight", "set", action == A_LIGHT_UP ? "5%+" : "5%-");
    }
    knob_t[action == A_LIGHT_UP || action == A_LIGHT_DOWN || action == A_LIGHT_SET] = ui_t();
    probe_poke();
}

static void move_changes(void)
{
    if (S.st.moving[0])
        toast(MUTED, "already moving");
    else if (!strcmp(S.st.where, "stick"))
        root_bg("unspill", NULL, NULL);
    else if (!S.stick)
        toast(RED, "no stick plugged in");
    else
        root_bg("spill", NULL, NULL);
}

static void disk_mount(struct disk *d, int rw)
{
    char out[256];
    if (d->mount[0]) {
        spawn((char *const[]){"foot", "-e", "nnn", "-de", d->mount, NULL});
        return;
    }
    if (d->stick) {
        toast(MUTED, "that's the stick. s moves the changes onto it");
        return;
    }
    if (!strcmp(d->fs, "crypto_LUKS")) {
        tool_disk = *d;
        key_failed = 0;
        open_sheet(S_LUKS);
        return;
    }
    if (run(out, sizeof out, "doas", SELF, "mount", d->dev, rw ? "rw" : NULL))
        toast(RED, "%s", clip(out, 70));
    else
        toast(GREEN, "it's at %s%s", out, rw ? "" : ", read only");
    probe_poke();
}

static void disk_unmount(struct disk *d)
{
    char out[256];
    if (!d->mount[0])
        toast(MUTED, "it isn't mounted");
    else if (run(out, sizeof out, "doas", SELF, "umount", d->dev))
        toast(RED, "%s", clip(out, 70));
    else
        toast(MUTED, "unmounted");
    probe_poke();
}

static void wifi_go(struct net *n)
{
    if (n->connected)
        toast(MUTED, "already on it");
    else if (!strcmp(n->type, "8021x"))
        toast(AMBER, "that one wants a login. set it up with iwctl");
    else if (!strcmp(n->type, "psk") && !n->known[0]) {
        key_for = *n;
        key_failed = 0;
        open_sheet(S_KEY);
    } else {
        probe_wifi("connect", n->path);
        toast(MUTED, "connecting to %s", n->name);
    }
}

static void wifi_forget(struct net *n)
{
    if (n->connected && !n->known[0])
        probe_wifi("disconnect", S.station);
    else if (n->known[0])
        probe_wifi("forget", n->known);
    else
        toast(MUTED, "nothing saved for it");
}

static void *picked(void)
{
    if (!count[col])
        return NULL;
    if (col == APPS)
        return &S.apps[shown[sel[APPS]]];
    if (col == WIFI)
        return &S.nets[sel[WIFI]];
    return &S.disks[sel[DISKS]];
}

static void go(void)
{
    void *p = picked();
    if (!p)
        return;
    if (col == APPS) {
        launch(p);
        finding = 0;
        query.text[0] = 0;
    } else if (col == WIFI) {
        wifi_go(p);
    } else {
        disk_mount(p, 0);
    }
}

static void sheet_done(void)
{
    char line[300], out[256], cmd[700];

    switch (sheet) {
    case S_OFF: {
        /* rows: save+off, save+reboot, drop+off, drop+reboot */
        int save = sheet_row < 2, reboot = sheet_row & 1;
        if (save && strcmp(S.st.where, "stick") && !S.stick) {
            toast(RED, "no stick, so there's nowhere to save to");
            return;
        }
        root_bg("off", save ? "save" : "drop", reboot ? "reboot" : NULL);
        leaving = 1;
        return;
    }
    case S_BT:
        if (!S.bt)
            return;
        if (!S.bt_on) {
            probe_bt("on", "");
        } else if (sheet_row < S.nbt) {
            struct bt *b = &S.bts[sheet_row];
            probe_bt(b->connected ? "disconnect" : "connect", b->path);
            toast(MUTED, "%s %s", b->connected ? "letting go of" : "connecting to", b->name);
        }
        return;
    case S_AUDIO:
        if (sheet_row < nsinks && !demo()) {
            snprintf(line, sizeof line, "%d", sinks[sheet_row].id);
            run(NULL, 0, "wpctl", "set-default", line);
            nsinks = probe_sinks(sinks, 12);
            probe_poke();
        }
        if (sheet_row < nsinks)
            toast(GREEN, "sound goes to %s", clip(sinks[sheet_row].name, 40));
        return;
    case S_COPY: {
        struct disk *to;
        const char *src = copy_whole ? tool_disk.parent : tool_disk.dev;
        long long need = copy_whole ? tool_disk.psize : tool_disk.size;

        if (sheet_row >= ncand)
            return;
        to = &S.disks[cand[sheet_row]];
        if (access(to->mount, W_OK) && !demo()) {
            toast(RED, "that one's mounted read only. unmount it, then w mounts it writable");
            return;
        }
        if (free_bytes(to->mount) < need && !demo()) {
            toast(RED, "not enough room there. it needs %s", human(need));
            return;
        }
        snprintf(cmd, sizeof cmd, "doas ddrescue -d -r1 '%s' '%s/%s.img' '%s/%s.map'", src,
                 to->mount, strrchr(src, '/') + 1, to->mount, strrchr(src, '/') + 1);
        term(cmd);
        break;
    }
    case S_KEY:
        if (!input.text[0])
            return;
        snprintf(line, sizeof line, "%s\n", input.text);
        if (runv(NULL, 0, line, (const char *const[]){"doas", SELF, "wifi-key", key_for.name, NULL}))
            toast(RED, "couldn't save the password");
        else {
            usleep(300000);
            key_tried = 1;
            probe_wifi("connect", key_for.path);
            toast(MUTED, "connecting to %s", key_for.name);
        }
        break;
    case S_LUKS:
        if (!input.text[0])
            return;
        snprintf(line, sizeof line, "%s\n", input.text);
        if (demo()) {
            toast(MUTED, "demo: nothing to unlock");
        } else if (runv(out, sizeof out, line,
                        (const char *const[]){"doas", SELF, "unlock", tool_disk.dev, NULL})) {
            key_failed = 1;
            input.text[0] = 0;
            return;
        } else {
            toast(GREEN, "unlocked. it's in the list now as %s", out + 5);
            probe_poke();
        }
        break;
    case S_INSTALL:
        if (!input.text[0])
            return;
        ui_outside((char *const[]){"sh", "-c",
            "flatpak remote-add --user --if-not-exists flathub "
            "https://dl.flathub.org/repo/flathub.flatpakrepo && "
            "flatpak install --user flathub \"$1\"", "flatpak", input.text, NULL}, 1);
        probe_poke();
        break;
    }
    sheet = NO_SHEET;
}

static void act(int action, int arg)
{
    flash(action);
    switch (action) {
    case A_MOVE: move_changes(); break;
    case A_FIND: finding = 1; col = APPS; query.text[0] = 0; break;
    case A_TERM: spawn((char *const[]){"foot", NULL}); break;
    case A_OFF: open_sheet(S_OFF); break;
    case A_VOL_DOWN: case A_VOL_UP: case A_LIGHT_DOWN: case A_LIGHT_UP:
    case A_MUTE: case A_VOL_SET: case A_LIGHT_SET:
        knob(action, arg);
        break;
    case A_PKGS: pkgs_open(); page = 1; break;
    case A_EXTRAS: extras_open(); page = 2; break;
    case A_FIX: fix_open(); page = 4; break;
    case A_CANCEL: {
        char out[160];
        if (demo())
            toast(MUTED, "demo: nothing to cancel");
        else if (run(out, sizeof out, "doas", SELF, "cancel"))
            toast(MUTED, "%s", out[0] ? out : "nothing is moving");
        else
            toast(AMBER, "going back. it's safe, nothing gets lost");
        break;
    }
    case A_SETTINGS: set_open(); page = 3; break;
    case A_BT: open_sheet(S_BT); break;
    case A_AUDIO: open_sheet(S_AUDIO); break;
    case A_WIN: wins_focus(arg); break;
    case A_WHOLE: copy_whole = !copy_whole; break;
    case A_TOOLS:
        col = DISKS;
        tools_open = !tools_open;
        tools_t = ui_t();
        break;
    case A_TOOL: tool(arg); break;
    case A_COL: col = arg; break;
    case A_ITEM:
        if (col == arg / 1000 && sel[col] == arg % 1000)
            go();
        col = arg / 1000;
        sel[col] = arg % 1000;
        break;
    case A_GO: go(); break;
    case A_WRITABLE: if (col == DISKS && picked()) disk_mount(picked(), 1); break;
    case A_UNMOUNT: if (col == DISKS && picked()) disk_unmount(picked()); break;
    case A_FORGET: if (col == WIFI && picked()) wifi_forget(picked()); break;
    case A_SCAN: probe_wifi("scan", S.station); toast(MUTED, "looking for networks"); break;
    case A_ROW: sheet_row = arg; sheet_done(); break;
    case A_CLOSE:
        if (sheet_only)
            ui_quit = 1;
        if (sheet == S_BT)
            probe_bt_scan(0);
        sheet = NO_SHEET;
        break;
    case A_SHOW: input.show = !input.show; break;
    }
}

/* --- keys --------------------------------------------------------------- */

static void sheet_key(key k)
{
    int rows = list_rows(), ch = k.kind == K_CHAR ? (int)k.ch : 0;

    if (leaving)
        return;
    if (k.kind == K_ESC || k.kind == K_CTRLC) {
        act(A_CLOSE, 0);
    } else if (sheet == S_KEY || sheet == S_LUKS || sheet == S_INSTALL) {
        if (k.kind == K_ENTER)
            sheet_done();
        else if (k.kind == K_TAB && sheet != S_INSTALL)
            act(A_SHOW, 0);
        else
            field_key(&input, k);
    } else if (k.kind == K_TAB && sheet == S_COPY) {
        act(A_WHOLE, 0);
    } else if (k.kind == K_UP || ch == 'k') {
        if (rows)
            sheet_row = (sheet_row + rows - 1) % rows;
    } else if (k.kind == K_DOWN || k.kind == K_TAB || ch == 'j') {
        if (rows)
            sheet_row = (sheet_row + 1) % rows;
    } else if (ch >= '1' && ch <= '9' && ch - '1' < rows) {
        act(A_ROW, ch - '1');
    } else if (k.kind == K_ENTER) {
        sheet_done();
    } else if (ch == 'x' && sheet == S_BT && sheet_row < S.nbt) {
        if (S.bts[sheet_row].paired)
            probe_bt("forget", S.bts[sheet_row].path);
        else
            toast(MUTED, "nothing saved for it");
    } else if (ch == 'q') {
        act(A_CLOSE, 0);
    }
}

static void on_key(key k)
{
    int n = count[col];

    if (page && !sheet) {
        int r = page == 1 ? pkgs_key(k) : page == 2 ? extras_key(k) : page == 4 ? fix_key(k) : set_key(k);
        if (page == 2 && r == 2) {
            page = 1;       /* the install runs on the package page, with its output */
        } else if (page == 3 && r == 2) {
            /* new text size: foot only picks it up in a new window */
            if (!demo())
                spawn((char *const[]){SELF, "home", NULL});
            page = 0;
        } else if (!r) {
            page = 0;
        }
        return;
    }
    if (k.kind == K_CLICK) {
        hit *h = find_hit(k.x, k.y);
        if (h && (!sheet || h->action == A_ROW || h->action == A_CLOSE || h->action == A_SHOW ||
                  h->action == A_WHOLE))
            act(h->action, h->arg);
        return;
    }
    if (k.kind == K_MOVE)
        return;
    if (k.kind == K_WHEEL) {
        if (!sheet && n) {
            sel[col] += k.d;
            if (sel[col] < 0) sel[col] = 0;
            if (sel[col] >= n) sel[col] = n - 1;
        }
        return;
    }
    if (sheet) {
        sheet_key(k);
        return;
    }

    switch (k.kind) {
    case K_CTRLC: ui_quit = 1; return;
    case K_ESC:
        if (tools_open) {
            tools_open = 0;
            tools_t = ui_t();
        } else {
            finding = 0;
            query.text[0] = 0;
        }
        return;
    case K_ENTER: act(A_GO, 0); return;
    case K_TAB: case K_RIGHT: col = (col + 1) % 3; finding = 0; return;
    case K_STAB: case K_LEFT: col = (col + 2) % 3; finding = 0; return;
    case K_DOWN: if (n) sel[col] = sel[col] + 1 < n ? sel[col] + 1 : n - 1; return;
    case K_UP: if (sel[col] > 0) sel[col]--; return;
    }
    if (finding) {
        if (field_key(&query, k))
            sel[APPS] = scroll[APPS] = 0;
        return;
    }
    if (k.kind != K_CHAR)
        return;
    switch (k.ch) {
    case 's': act(A_MOVE, 0); break;
    case '/': act(A_FIND, 0); break;
    case 't': act(A_TERM, 0); break;
    case 'o': act(A_OFF, 0); break;
    case 'p': act(A_PKGS, 0); break;
    case 'b': act(A_BT, 0); break;
    case 'e': act(A_EXTRAS, 0); break;
    case 'c': act(A_CANCEL, 0); break;
    case 'f': act(A_FIX, 0); break;
    case ',': act(A_SETTINGS, 0); break;
    case 'a': act(A_AUDIO, 0); break;
    case 'd': act(A_TOOLS, 0); break;
    case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
        if (tools_open)
            act(A_TOOL, k.ch - '1');
        else if ((int)k.ch - '1' < nwins)
            act(A_WIN, wins[k.ch - '1'].id);
        break;
    case 'm': act(A_MUTE, 0); break;
    case '-': act(A_VOL_DOWN, 0); break;
    case '=': case '+': act(A_VOL_UP, 0); break;
    case '[': act(A_LIGHT_DOWN, 0); break;
    case ']': act(A_LIGHT_UP, 0); break;
    case 'w': act(A_WRITABLE, 0); break;
    case 'u': act(A_UNMOUNT, 0); break;
    case 'x': act(A_FORGET, 0); break;
    case 'r': act(A_SCAN, 0); break;
    case 'l': col = (col + 1) % 3; break;
    case 'h': col = (col + 2) % 3; break;
    case 'j': if (n) sel[col] = sel[col] + 1 < n ? sel[col] + 1 : n - 1; break;
    case 'k': if (sel[col] > 0) sel[col]--; break;
    }
}

/* --- state -------------------------------------------------------------- */

static int mem_pct(void)
{
    return S.mem_total ? 100 - S.mem_avail * 100 / S.mem_total : 0;
}

static void on_tick(double dt)
{
    char msg[160];
    int r, moving;
    (void)dt;

    probe_get(&S);

    nshown = 0;
    for (int i = 0; i < S.napps; i++)
        if (!query.text[0] || strcasestr(S.apps[i].name, query.text))
            shown[nshown++] = i;
    count[APPS] = nshown;
    count[WIFI] = S.nnets;
    count[DISKS] = S.ndisks;
    for (int i = 0; i < 3; i++)
        if (sel[i] >= count[i])
            sel[i] = count[i] ? count[i] - 1 : 0;

    if (page == 1 || pkgs_busy())
        pkgs_tick();
    nwins = wins_get(wins, 9);
    if (tools_open && (col != DISKS || !count[DISKS])) {
        tools_open = 0;
        tools_t = ui_t();
    }
    if (sheet && sheet_row >= list_rows() && list_rows())
        sheet_row = list_rows() - 1;

    r = probe_wifi_result(msg, sizeof msg);
    if (r) {
        toast(r > 0 ? GREEN : RED, "%s", msg);
        if (r > 0 && !strcmp(msg, "connected"))
            burst(GREEN, 16, 1.5);
        if (r < 0 && key_tried && !sheet) {
            /* a password that was just typed and didn't work: ask again */
            open_sheet(S_KEY);
            key_failed = 1;
        }
        key_tried = 0;
    }
    if (S.st.error_at != seen_error) {
        if (seen_error || time(NULL) - S.st.error_at < 20)
            toast(RED, "%s", clip(S.st.error, 76));
        seen_error = S.st.error_at;
        leaving = 0;
    }
    if (S.st.note_at != seen_note) {
        if (time(NULL) - S.st.note_at < 60)
            toast(GREEN, "%s", clip(S.st.note, 76));
        seen_note = S.st.note_at;
    }
    moving = S.st.moving[0] != 0;
    if (was_moving && !moving && !S.st.error[0])
        burst(GREEN, 16, 1.5);
    was_moving = moving;
}

static void status(const char **w, rgb *color, char *hint, size_t n)
{
    int in_ram = strcmp(S.st.where, "stick") != 0;
    struct conf c;

    load_conf(&c);
    if (S.st.leaving[0]) {
        *w = "BYE"; *color = MUTED;
        snprintf(hint, n, "%s", !strcmp(S.st.leaving, "reboot") ? "restarting" : "powering off");
    } else if (S.st.lost) {
        *w = "LOST"; *color = RED;
        snprintf(hint, n, "the stick's gone and this session lives on it. put it back in");
    } else if (!strcmp(S.st.moving, "back")) {
        *w = "BACK"; *color = AMBER;
        snprintf(hint, n, "cancelled, putting everything back where it was. leave the stick in");
    } else if (S.st.moving[0]) {
        *w = "MOVE"; *color = AMBER;
        snprintf(hint, n, "%s", !strcmp(S.st.moving, "stick")
                 ? "moving the changes onto the stick. leave it in. c cancels"
                 : "pulling the changes back into ram. leave the stick in. c cancels");
    } else if (!in_ram) {
        *w = "STICK"; *color = SIGNAL;
        snprintf(hint, n, "the session's on the stick. keep it in");
    } else if (!strcmp(S.st.sys, "stick")) {
        *w = "STICK"; *color = SIGNAL;
        snprintf(hint, n, "ram's short, so the system is read off the stick. keep it in");
    } else if (!strcmp(S.st.sys, "moving")) {
        *w = "MOVE"; *color = AMBER;
        snprintf(hint, n, "pulling the system back into ram. leave the stick in. c cancels");
    } else if (mem_pct() >= c.warn && !S.stick) {
        *w = "FULL"; *color = RED;
        snprintf(hint, n, "ram's filling up. plug the stick in");
    } else {
        *w = "RAM"; *color = GREEN;
        if (!strcmp(S.st.hw, "stick"))
            snprintf(hint, n, "copying this machine's drivers into ram. keep the stick in a minute");
        else
            snprintf(hint, n, "%s", S.stick ? "stick's in. s moves the changes onto it"
                                        : "nowhere to save. d on a stick, then 9, makes it one");
    }
}

/* --- drawing ------------------------------------------------------------ */

static const char *bars(int dbm)
{
    return dbm > -55 ? "▂▄▆█" : dbm > -67 ? "▂▄▆ " : dbm > -78 ? "▂▄  " : "▂   ";
}

static void board(canvas *c, int top, int height)
{
    int colw = (c->w - 2) / 3, rows = height - 2;

    for (int i = 0; i < 3; i++) {
        int x = 1 + i * colw, w = colw - 2, focused = i == col, n = count[i], e;
        char num[16];

        e = put(c, x + 1, top, TITLES[i], focused ? TEXT : MUTED, BG, 1);
        snprintf(num, sizeof num, "%d", n);
        e = put(c, e + 1, top, num, n ? SIGNAL : DIM, BG, 1);
        if (i == APPS && (finding || query.text[0])) {
            e = put(c, e + 2, top, "/", SIGNAL, BG, 1);
            if (finding)
                field_draw(&query, c, e, top, w - (e - x) - 1, BG);
            else
                put(c, e + 1, top, clip(query.text, w - (e - x) - 2), TEXT, BG, 0);
        } else if (i == WIFI && S.scanning) {
            e = put(c, e + 2, top, spinner(), AMBER, BG, 0);
            put(c, e + 1, top, "looking", DIM, BG, 0);
        }
        for (int j = 0; j < w; j++)
            put(c, x + j, top + 1, "▔", focused ? SIGNAL : LINE, BG, 0);
        add_hit(x, top, w, A_COL, i);
        if (rows < 1)
            continue;

        if (focused) {
            if (sel[i] < scroll[i]) scroll[i] = sel[i];
            if (sel[i] >= scroll[i] + rows) scroll[i] = sel[i] - rows + 1;
        }
        if (scroll[i] > n - rows) scroll[i] = n > rows ? n - rows : 0;

        if (!n) {
            const char *empty = i == APPS ? "nothing by that name" :
                                i == WIFI ? (S.wifi ? "no networks yet. r looks again" : "no wifi card found") :
                                "no disks";
            put(c, x + 1, top + 3, clip(empty, w - 2), DIM, BG, 0);
            continue;
        }
        for (int r = 0; r < rows && scroll[i] + r < n; r++) {
            int idx = scroll[i] + r, y = top + 2 + r, picked = idx == sel[i];
            rgb bg = picked && focused ? PANEL2 : picked ? PANEL : BG;
            rgb name_c = picked ? TEXT : MUTED, side_c = picked ? MUTED : DIM;
            char side[64] = "";
            const char *name = "";

            fill(c, x, y, w, 1, bg);
            put(c, x, y, "┃", picked && focused ? SIGNAL : picked ? LINE : BG, bg, 0);
            if (i == APPS) {
                struct appent *a = &S.apps[shown[idx]];
                name = a->name;
                snprintf(side, sizeof side, "%s", a->src);
                if (!strcmp(a->src, "lifeboat"))
                    side_c = mix(bg, SIGNAL, picked ? 0.8 : 0.5);
            } else if (i == WIFI) {
                struct net *nt = &S.nets[idx];
                name = nt->name;
                snprintf(side, sizeof side, "%s %s", nt->connected ? "on" :
                         !strcmp(nt->type, "open") ? "open" : nt->known[0] ? "saved" : "", bars(nt->signal));
                if (nt->connected)
                    side_c = GREEN;
            } else {
                struct disk *d = &S.disks[idx];
                name = d->label[0] ? d->label : d->dev + 5;
                snprintf(side, sizeof side, "%s%s %s", d->mount[0] ? "mounted " : "", d->fs, human(d->size));
                if (d->stick)
                    name_c = picked ? SIGNAL : mix(bg, SIGNAL, 0.7);
                if (d->mount[0])
                    side_c = GREEN;
            }
            int sw = ulen(side);
            if (sw > w - 10) sw = 0;
            put(c, x + 2, y, clip(name, w - sw - 4), name_c, bg, picked);
            if (sw)
                put(c, x + w - sw - 1, y, side, side_c, bg, 0);
            add_hit(x, y, w, A_ITEM, i * 1000 + idx);
        }
        if (n - scroll[i] - rows > 0) {
            snprintf(num, sizeof num, "+%d", n - scroll[i] - rows);
            put(c, x + w - strlen(num) - 1, top, num, DIM, BG, 0);
        }
    }
}

static void drawer(canvas *c, int top, int height)
{
    int moving = S.st.moving[0] != 0, y = top + 1, x;
    void *p = picked();

    fill(c, 0, top, c->w, height, PANEL);
    for (int i = 0; i < c->w; i++)
        put(c, i, top, "▀", mix(BG, moving ? AMBER : LINE, 0.8), BG, 0);

    if (!p) {
        put(c, 2, y, "nothing picked", MUTED, PANEL, 0);
    } else if (col == APPS) {
        struct appent *a = p;
        x = put(c, 2, y, clip(a->name, 40), TEXT, PANEL, 1);
        put(c, x + 2, y, clip(!strcmp(a->exec, "@pkgs") ? "pacman and the aur, built in" :
                              !strcmp(a->exec, "@extras") ? "the apps the image leaves out, ticked and installed" :
                              !strcmp(a->exec, "@settings") ? "screen, scale, keyboard, touchpad, time" :
                              !strcmp(a->exec, "@bt") ? "pair and connect things" :
                              !strcmp(a->exec, "@audio") ? "pick where the sound goes" :
                              a->exec[0] == '@' ? "from flathub" : a->exec, c->w - x - 6), MUTED, PANEL, 0);
        x = ui_link(c, 1, y + 1, "enter", !strcmp(a->exec, "@install") ? "pick one" : "open",
                    A_GO, SIGNAL, PANEL);
        ui_link(c, x, y + 1, "/", "find", A_FIND, MUTED, PANEL);
    } else if (col == WIFI) {
        struct net *n = p;
        x = put(c, 2, y, clip(n->name, 40), TEXT, PANEL, 1);
        putf(c, x + 2, y, MUTED, PANEL, 0, "%s, %d dBm%s", !strcmp(n->type, "psk") ? "password" :
             !strcmp(n->type, "open") ? "open" : n->type, n->signal, n->connected ? ", connected" : "");
        x = 1;
        if (!n->connected)
            x = ui_link(c, x, y + 1, "enter", "connect", A_GO, SIGNAL, PANEL);
        if (n->connected || n->known[0])
            x = ui_link(c, x, y + 1, "x", n->known[0] ? "forget" : "disconnect", A_FORGET, MUTED, PANEL);
        ui_link(c, x, y + 1, "r", "look again", A_SCAN, MUTED, PANEL);
    } else {
        struct disk *d = p;
        x = put(c, 2, y, d->dev, TEXT, PANEL, 1);
        putf(c, x + 2, y, MUTED, PANEL, 0, "%s", clip(d->model[0] ? d->model : d->tran, 30));
        if (d->mount[0])
            putf(c, x + 4 + ulen(clip(d->model[0] ? d->model : d->tran, 30)), y, GREEN, PANEL, 0,
                 "at %s", clip(d->mount, 40));
        x = 1;
        if (d->stick) {
            x = put(c, 2, y + 1, "this is the stick. changes get saved here", DIM, PANEL, 0) + 1;
        } else if (!strcmp(d->label, "lifeboat") && !S.stick) {
            x = put(c, 2, y + 1, "lifeboat started from this. d then 9 lets it keep sessions too", DIM, PANEL, 0) + 1;
        } else if (!strcmp(d->fs, "crypto_LUKS")) {
            x = ui_link(c, x, y + 1, "enter", "unlock", A_GO, SIGNAL, PANEL);
        } else if (d->mount[0]) {
            x = ui_link(c, x, y + 1, "enter", "browse", A_GO, SIGNAL, PANEL);
            x = ui_link(c, x, y + 1, "u", "unmount", A_UNMOUNT, MUTED, PANEL);
        } else {
            x = ui_link(c, x, y + 1, "enter", "mount read only", A_GO, SIGNAL, PANEL);
            x = ui_link(c, x, y + 1, "w", "mount writable", A_WRITABLE, AMBER, PANEL);
        }
        ui_link(c, x, y + 1, "d", tools_open ? "close the tools" : "rescue tools", A_TOOLS, MUTED, PANEL);
        if (d->health[0]) {
            rgb hc = d->hlevel == 3 ? RED : d->hlevel == 2 ? AMBER : d->hlevel == 1 ? GREEN : DIM;
            put(c, 2, y + 2, "health", hc, PANEL, 1);
            put(c, 9, y + 2, clip(d->health, c->w - 12), d->hlevel > 1 ? hc : MUTED, PANEL, 0);
        }
    }

    put(c, 2, y + 3, "boat", SIGNAL, PANEL, 1);
    if (moving) {
        long left = S.st.total - S.st.base, did = S.st.done - S.st.base;
        double frac = left > 0 ? (double)did / left : -1;
        long secs = time(NULL) - S.st.since;
        x = put(c, 7, y + 3, spinner(), AMBER, PANEL, 0);
        if (frac >= 0)
            putf(c, x + 1, y + 3, TEXT, PANEL, 0, "%d%%, %s of %s", (int)(frac * 100),
                 human(did * 65536LL), human(left * 65536LL));
        else
            put(c, x + 1, y + 3, "getting ready", TEXT, PANEL, 0);
        putf(c, c->w - 8, y + 3, MUTED, PANEL, 0, "%ld:%02ld", secs / 60, secs % 60);
        if (strcmp(S.st.moving, "back"))
            ui_link(c, c->w - 22, y + 3, "c", "cancel", A_CANCEL, AMBER, PANEL);
        else
            put(c, c->w - 22, y + 3, "going back", AMBER, PANEL, 0);
        progress(c, 2, y + 4, c->w - 4, frac, PANEL);
    } else {
        int in_ram = strcmp(S.st.where, "stick") != 0;
        putf(c, 7, y + 3, MUTED, PANEL, 0, "%s of changes %s", human(S.changes),
             in_ram ? "in ram" : "on the stick");
        if (S.st.stick_size > 0 && c->w > 90) {
            /* how full the stick is */
            double full = 1.0 - (double)S.st.stick_free / S.st.stick_size;
            char room[48];
            snprintf(room, sizeof room, "%s free", human(S.st.stick_free));
            x = c->w - 2 - ulen(room);
            put(c, x, y + 3, room, MUTED, PANEL, 0);
            meter(c, x - 15, y + 3, 14, full, full > 0.9 ? RED : SIGNAL, PANEL);
            put(c, x - 21, y + 3, "stick", DIM, PANEL, 0);
        }
    }

    /* the tools grid, in the part of the drawer that slides up */
    if (height > 9 && col == DISKS && p) {
        struct disk *d = p;
        int half = (c->w - 4) / 2, ty = y + 6;

        for (int i = 0; i < c->w; i++)
            put(c, i, ty - 1, "▔", LINE, PANEL, 0);
        for (int i = 0; i < 9 && ty + i % 5 < top + height; i++) {
            int tx = 2 + (i / 5) * half, row = ty + i % 5, e;
            int off = (i == 7) != !strcmp(d->fs, "crypto_LUKS") && (i == 7 || i < 3);
            char num[2] = {'1' + i, 0};
            int hot = ui_hover == A_TOOL && ui_hover_arg == i;

            put(c, tx, row, num, off ? DIM : SIGNAL, PANEL, 1);
            e = put(c, tx + 2, row, TOOLS[i][0], off ? DIM : hot ? SIGNAL : TEXT, PANEL, hot);
            put(c, e + 2, row, clip(TOOLS[i][1], half - (e - tx) - 4), DIM, PANEL, 0);
            add_hit(tx, row, half - 2, A_TOOL, i);
        }
    }
}

/* one row of a list sheet */
static void list_row(canvas *c, int x, int y, int w, int i, const char *left, const char *right,
                     rgb right_c, int ok)
{
    int on = i == sheet_row;
    rgb bg = on ? PANEL2 : PANEL;
    int rw = ulen(right);

    fill(c, x + 2, y, w - 4, 1, bg);
    put(c, x + 2, y, "┃", on ? SIGNAL : LINE, bg, 0);
    if (i < 9)
        putf(c, x + 4, y, on ? SIGNAL : DIM, bg, 1, "%d", i + 1);
    put(c, x + 6, y, clip(left, w - 10 - rw), !ok ? DIM : on ? TEXT : MUTED, bg, on);
    put(c, x + w - 3 - rw, y, right, right_c, bg, 0);
    add_hit(x + 2, y, w - 4, A_ROW, i);
}

static void bt_sheet(canvas *c, int x, int w)
{
    int y = 6, lx;

    pixel_word(c, x + 3, 1, "BLUE", S.bt && S.bt_on ? SIGNAL : DIM, PANEL, ui_t() - sheet_t);
    if (S.bt_scanning) {
        put(c, x + w - 12, 2, spinner(), AMBER, PANEL, 0);
        put(c, x + w - 10, 2, "looking", DIM, PANEL, 0);
    }
    if (!S.bt) {
        wrap(c, x + 3, y, w - 6, "no bluetooth adapter here, or its driver didn't load.", MUTED, PANEL);
    } else if (!S.bt_on) {
        put(c, x + 3, y, "bluetooth is off", MUTED, PANEL, 0);
        list_row(c, x, y + 2, w, 0, "turn it on", "", DIM, 1);
    } else if (!S.nbt) {
        wrap(c, x + 3, y, w - 6, "nothing yet. put the device in pairing mode",
             MUTED, PANEL);
    } else {
        for (int i = 0; i < S.nbt && y < c->h - 4; i++, y++) {
            struct bt *b = &S.bts[i];
            list_row(c, x, y, w, i, b->name, b->connected ? "on" : b->paired ? "paired" :
                     b->rssi ? bars(b->rssi) : "", b->connected ? GREEN : DIM, 1);
        }
    }
    lx = x + 2;
    if (S.bt && S.bt_on && sheet_row < S.nbt) {
        lx = ui_link(c, lx, c->h - 2, "enter", S.bts[sheet_row].connected ? "disconnect" : "connect",
                     A_ROW, SIGNAL, PANEL);
        if (S.bts[sheet_row].paired) {
            lx = put(c, lx + 1, c->h - 2, "x", SIGNAL, PANEL, 1);
            lx = put(c, lx + 1, c->h - 2, "forget", MUTED, PANEL, 0) + 2;
        }
    }
    ui_link(c, lx, c->h - 2, "esc", "close", A_CLOSE, MUTED, PANEL);
}

static void audio_sheet(canvas *c, int x, int w)
{
    int y = 6;

    pixel_word(c, x + 3, 1, "SOUND", SIGNAL, PANEL, ui_t() - sheet_t);
    if (!nsinks)
        wrap(c, x + 3, y, w - 6, "nothing to play sound through, or pipewire isn't up yet.", MUTED, PANEL);
    for (int i = 0; i < nsinks && y < c->h - 4; i++, y++)
        list_row(c, x, y, w, i, sinks[i].name, sinks[i].current ? "playing" : "", GREEN, 1);
    y = ui_link(c, x + 2, c->h - 2, "enter", "send sound there", A_ROW, SIGNAL, PANEL);
    ui_link(c, y, c->h - 2, "esc", "close", A_CLOSE, MUTED, PANEL);
}

static void copy_sheet(canvas *c, int x, int w)
{
    const char *src = copy_whole ? tool_disk.parent : tool_disk.dev;
    long long need = copy_whole ? tool_disk.psize : tool_disk.size;
    char line[200];
    int y = 6, lx;

    pixel_word(c, x + 3, 1, "COPY", SIGNAL, PANEL, ui_t() - sheet_t);
    snprintf(line, sizeof line, "copies %s (%s) into an image file with ddrescue. "
             "where to?",
             src, human(need));
    y += wrap(c, x + 3, y, w - 6, line, MUTED, PANEL) + 1;
    if (!ncand)
        y += wrap(c, x + 3, y, w - 6, "nowhere yet. mount another disk writable first (w on it).",
                  AMBER, PANEL);
    for (int i = 0; i < ncand && y < c->h - 4; i++, y++) {
        struct disk *d = &S.disks[cand[i]];
        long long room = free_bytes(d->mount);
        int ro = access(d->mount, W_OK) != 0 && !demo(), ok = !ro && (room >= need || demo());
        snprintf(line, sizeof line, "%s", ro ? "read only" : ok ? human(room) : "too small");
        list_row(c, x, y, w, i, d->label[0] ? d->label : d->mount, line, ok ? MUTED : RED, ok);
    }
    lx = ui_link(c, x + 2, c->h - 2, "tab", copy_whole ? "just the partition" : "the whole disk",
                 A_WHOLE, MUTED, PANEL);
    ui_link(c, lx, c->h - 2, "esc", "never mind", A_CLOSE, MUTED, PANEL);
}

static void off_sheet(canvas *c, int x, int w)
{
    static const char *rows[] = {"save, then power off", "save, then restart",
                                 "drop it and power off", "drop it and restart"};
    int on_stick = !strcmp(S.st.where, "stick"), can_save = on_stick || S.stick, y = 6;

    pixel_word(c, x + 3, 1, leaving ? "BYE" : "OFF", leaving ? MUTED : SIGNAL, PANEL, ui_t() - sheet_t);
    if (leaving) {
        if (S.st.moving[0]) {
            long left = S.st.total - S.st.base, did = S.st.done - S.st.base;
            put(c, x + 3, y, "saving to the stick. leave it in", TEXT, PANEL, 1);
            progress(c, x + 3, y + 2, w - 6, left > 0 ? (double)did / left : -1, PANEL);
        } else {
            put(c, x + 3, y, "going down", TEXT, PANEL, 1);
            put(c, x + 3, y + 1, spinner(), AMBER, PANEL, 0);
        }
        return;
    }
    y += wrap(c, x + 3, y, w - 6, on_stick
        ? "save keeps this session. drop goes back to the last save."
        : can_save ? "save copies this session to the stick. drop forgets everything since boot."
        : "no stick in, so nothing can be saved.",
        MUTED, PANEL) + 1;
    for (int i = 0; i < 4; i++, y += 2) {
        int picked = i == sheet_row, ok = i >= 2 || can_save;
        rgb bg = picked ? PANEL2 : PANEL;
        fill(c, x + 2, y, w - 4, 1, bg);
        put(c, x + 2, y, "┃", picked ? SIGNAL : LINE, bg, 0);
        putf(c, x + 4, y, picked ? SIGNAL : DIM, bg, 1, "%d", i + 1);
        put(c, x + 6, y, rows[i], !ok ? DIM : picked ? TEXT : MUTED, bg, picked);
        add_hit(x + 2, y, w - 4, A_ROW, i);
    }
    ui_link(c, x + 2, c->h - 2, "esc", "never mind", A_CLOSE, MUTED, PANEL);
}

static void text_sheet(canvas *c, int x, int w)
{
    int y = 6, lx;

    if (sheet == S_KEY) {
        pixel_word(c, x + 3, 1, "KEY", key_failed ? RED : SIGNAL, PANEL, ui_t() - sheet_t);
        putf(c, x + 3, y, TEXT, PANEL, 1, "%s", clip(key_for.name, w - 6));
        put(c, x + 3, y + 1, key_failed ? "that one didn't work. try it again" : "what's the password?",
            key_failed ? RED : MUTED, PANEL, 0);
    } else if (sheet == S_LUKS) {
        pixel_word(c, x + 3, 1, "LOCK", key_failed ? RED : SIGNAL, PANEL, ui_t() - sheet_t);
        putf(c, x + 3, y, TEXT, PANEL, 1, "%s, %s", tool_disk.dev, human(tool_disk.size));
        put(c, x + 3, y + 1, key_failed ? "that's not it. try again" : "it's encrypted. what's its password?",
            key_failed ? RED : MUTED, PANEL, 0);
    } else {
        pixel_word(c, x + 3, 1, "GET", SIGNAL, PANEL, ui_t() - sheet_t);
        y += wrap(c, x + 3, y, w - 6, "type a flatpak's name, like firefox or vlc. "
                  "it comes from flathub and lands in this session's changes.", MUTED, PANEL) - 2;
    }
    field_draw(&input, c, x + 3, y + 3, w - 6, PANEL2);
    lx = ui_link(c, x + 2, c->h - 2, "enter", sheet == S_KEY ? "connect" : sheet == S_LUKS ? "unlock" :
                 "get it", A_ROW, SIGNAL, PANEL);
    if (sheet != S_INSTALL)
        lx = ui_link(c, lx, c->h - 2, "tab", input.show ? "hide" : "show", A_SHOW, MUTED, PANEL);
    ui_link(c, lx, c->h - 2, "esc", "never mind", A_CLOSE, MUTED, PANEL);
}

/* sheets are full height and slide in from the right, over a dimmed board */
static void draw_sheet(canvas *c)
{
    int w = c->w < 54 ? c->w : 50;
    double in = (ui_t() - sheet_t) / 0.18;
    int x;

    if (in > 1) in = 1;
    in = 1 - (1 - in) * (1 - in);
    x = c->w - (int)(w * in);
    if (sheet_only) {
        x = 0;
        w = c->w;
    } else {
        dim(c, 0, 0, x, c->h, 0.55 * in);
    }
    fill(c, x, 0, w, c->h, PANEL);
    for (int y = 0; y < c->h && !sheet_only; y++)
        put(c, x, y, "▌", mix(BG, SIGNAL, 0.6), PANEL, 0);
    switch (sheet) {
    case S_OFF: off_sheet(c, x, w); break;
    case S_BT: bt_sheet(c, x, w); break;
    case S_AUDIO: audio_sheet(c, x, w); break;
    case S_COPY: copy_sheet(c, x, w); break;
    default: text_sheet(c, x, w);
    }
}

/* a volume or brightness dial, right-aligned at rx: - label ▀▀▀▀▀▀ 45 =
   the bar takes clicks, and its tip glows for a moment after a change */
static int dial(canvas *c, int rx, int y, const char *label, int value, int muted, int mw, int which)
{
    int down = which ? A_LIGHT_DOWN : A_VOL_DOWN, up = which ? A_LIGHT_UP : A_VOL_UP;
    int set = which ? A_LIGHT_SET : A_VOL_SET;
    int width = 3 + ulen(label) + 1 + (mw ? mw + 1 : 0) + 3 + 3, x = rx - width, e;
    int filled = (value * mw + 50) / 100;
    double glow = 1 - (ui_t() - knob_t[which]) / 0.5;
    rgb on = muted ? DIM : SIGNAL;

    if (filled > mw)
        filled = mw;
    e = put(c, x, y, which ? " [ " : " - ", ui_hover == down ? TEXT : DIM, BG, 1);
    add_hit(x, y, 3, down, 0);
    e = put(c, e, y, label, MUTED, BG, 0) + 1;
    for (int i = 0; i < mw; i++) {
        rgb cell = i < filled ? on : PANEL2;
        if (i == filled - 1 && glow > 0)
            cell = mix(on, HOT, glow);
        put(c, e + i, y, "▀", cell, BG, 0);
        add_hit(e + i, y, 1, set, (i + 1) * 100 / mw);
    }
    if (mw)
        e += mw + 1;
    if (muted)
        e = put(c, e, y, "off", DIM, BG, 0);
    else
        e = putf(c, e, y, TEXT, BG, 1, "%3d", value);
    if (!which)
        add_hit(e - 3, y, 3, A_MUTE, 0);
    put(c, e, y, which ? " ] " : " = ", ui_hover == up ? TEXT : DIM, BG, 1);
    add_hit(e, y, 3, up, 0);
    return x;
}

static void on_draw(canvas *c)
{
    const char *w;
    rgb color;
    char hint[160], clock[16], line[96];
    time_t now = time(NULL);
    int end, tx, room, x, rx, mw, drawer_h = 7;
    double ease;
    long used = S.mem_total - S.mem_avail;

    if (sheet_only) {
        if (!sheet)
            ui_quit = 1;
        else
            draw_sheet(c);
        return;
    }
    if (page) {
        if (page == 1)
            pkgs_draw(c);
        else if (page == 2)
            extras_draw(c, S.ssid[0] || S.route[0]);
        else if (page == 4)
            fix_draw(c);
        else
            set_draw(c);
        if (sheet)
            draw_sheet(c);
        return;
    }

    status(&w, &color, hint, sizeof hint);
    if (strcmp(w, word)) {
        if (word[0] && !strcmp(w, "RAM"))
            burst(GREEN, 16, 1.5);
        word = w;
        ui_word_t = ui_t();
    }
    end = pixel_word(c, 2, 0, w, color, BG, ui_t() - ui_word_t);

    tx = end + 4;
    room = c->w - tx - 50;
    snprintf(line, sizeof line, "ram %d%% used, %s of %s", mem_pct(), human(used * 1024LL),
             human(S.mem_total * 1024LL));
    put(c, tx, 0, clip(line, room), TEXT, BG, 1);
    /* what's taking the ram: changes, the system image, the drivers */
    if (S.st.sys[0] && strcmp(S.st.sys, "none")) {
        char drv[32];
        snprintf(drv, sizeof drv, "%s", !strcmp(S.st.hw, "this") ? human(S.st.hw_kept) :
                 !strcmp(S.st.hw, "all") ? "all" : "on stick");
        snprintf(line, sizeof line, "changes %s %s, system %s, drivers %s", human(S.changes),
                 strcmp(S.st.where, "stick") ? "in ram" : "on stick",
                 !strcmp(S.st.sys, "ram") ? "in ram" : !strcmp(S.st.sys, "moving") ? "moving" : "on stick", drv);
    } else
    snprintf(line, sizeof line, "%s of changes %s", human(S.changes),
             strcmp(S.st.where, "stick") ? "in ram" : "on the stick");
    put(c, tx, 1, clip(line, room), MUTED, BG, 0);
    put(c, tx, 2, clip(hint, room), DIM, BG, 0);

    /* top right: a pixel clock, and the small readings stacked left of it */
    strftime(clock, sizeof clock, set_clock12() ? "%I:%M" : "%H:%M", localtime(&now));
    if (clock[0] == '0' && set_clock12())
        memmove(clock, clock + 1, strlen(clock));
    pixel_word(c, c->w - 19, 0, clock, TEXT, BG, 10);
    rx = c->w - 22;
    strftime(line, sizeof line, "%a %d %b", localtime(&now));
    for (char *p = line; *p; p++)
        *p = tolower((unsigned char)*p);
    put(c, rx - ulen(line), 0, line, MUTED, BG, 0);
    if (S.bat >= 0 && S.bat_mins > 0 && !S.charging)
        snprintf(line, sizeof line, "battery %d%%, %d:%02d left", S.bat, S.bat_mins / 60, S.bat_mins % 60);
    else if (S.bat >= 0)
        snprintf(line, sizeof line, "%s %d%%", S.charging ? "charging" : "battery", S.bat);
    else
        snprintf(line, sizeof line, "plugged in");
    put(c, rx - ulen(line), 1, line, S.bat >= 0 && S.bat < 15 && !S.charging ? RED : MUTED, BG, 0);
    snprintf(line, sizeof line, "%s", S.ssid[0] ? clip(S.ssid, 16) : S.route[0] ? "wired" : "offline");
    put(c, rx - ulen(line), 2, line, S.ssid[0] || S.route[0] ? MUTED : DIM, BG, 0);

    x = 1;
    x = ui_link(c, x, 4, "s", strcmp(S.st.where, "stick") ? "to stick" : "to ram", A_MOVE, SIGNAL, BG);
    x = ui_link(c, x, 4, "/", "find", A_FIND, SIGNAL, BG);
    x = ui_link(c, x, 4, "t", "term", A_TERM, SIGNAL, BG);
    x = ui_link(c, x, 4, "p", "pkgs", A_PKGS, SIGNAL, BG);
    x = ui_link(c, x, 4, "b", "bt", A_BT, SIGNAL, BG);
    x = ui_link(c, x, 4, "e", "extras", A_EXTRAS, SIGNAL, BG);
    x = ui_link(c, x, 4, "f", "fix", A_FIX, SIGNAL, BG);
    x = ui_link(c, x, 4, ",", "settings", A_SETTINGS, SIGNAL, BG);
    x = ui_link(c, x, 4, "o", "off", A_OFF, SIGNAL, BG);

    mw = c->w - x >= 66 ? 12 : c->w - x >= 52 ? 6 : 0;
    rx = c->w - 2;
    if (S.light >= 0 && c->w - x > 16)
        rx = dial(c, rx, 4, "light", S.light, 0, mw, 1) - 2;
    if (S.vol >= 0 && rx - x > 16)
        dial(c, rx, 4, "vol", S.vol, S.muted, mw, 0);

    /* what's open: a number jumps to it */
    if (nwins) {
        x = put(c, 2, 5, "open", DIM, BG, 0) + 2;
        for (int i = 0; i < nwins && x < c->w - 12; i++) {
            const char *name = clip(wins[i].title[0] ? wins[i].title : wins[i].app, 22);
            int hot = ui_hover == A_WIN && ui_hover_arg == wins[i].id, start = x;
            x = putf(c, x, 5, hot || wins[i].active ? SIGNAL : MUTED, BG, 1, "%d", i + 1);
            x = put(c, x + 1, 5, name, hot ? TEXT : MUTED, BG, 0) + 3;
            add_hit(start, 5, x - start - 2, A_WIN, wins[i].id);
        }
    }

    /* the drawer grows to make room for the tools */
    ease = (ui_t() - tools_t) / 0.16;
    if (ease > 1) ease = 1;
    ease = 1 - (1 - ease) * (1 - ease);
    drawer_h += (int)(7 * (tools_open ? ease : 1 - ease) + 0.5);
    if (drawer_h > c->h - 10)
        drawer_h = c->h - 10 > 7 ? c->h - 10 : 7;

    board(c, 6, c->h - drawer_h - 6);
    drawer(c, c->h - drawer_h, drawer_h);
    if (sheet)
        draw_sheet(c);
}

static int on_fps(void)
{
    if (sheet && (ui_t() - sheet_t < 0.4 || sheet == S_KEY || sheet == S_LUKS || sheet == S_INSTALL ||
                  leaving))
        return 30;
    if (page == 1)
        return pkgs_fps();
    if (page == 3)
        return set_fps();
    if (page == 2)
        return 4;
    if (page == 4)
        return fix_fps();
    if (ui_t() - knob_t[0] < 0.6 || ui_t() - knob_t[1] < 0.6 || ui_t() - tools_t < 0.4)
        return 30;
    if (sheet == S_BT)
        return 15;
    if (S.st.moving[0] || S.scanning || finding)
        return 15;
    if (strcmp(word, "RAM") && strcmp(word, "BYE"))
        return 8;       /* hazard stripes crawl */
    return 2;
}

int tui_home(const char *only)
{
    struct app app = {on_draw, on_key, on_tick, on_fps, 60, 18};
    const char *dump = getenv("LIFEBOAT_DUMP");

    probe_start();
    wins_start();
    tools_t = -5;
    probe_get(&S);
    if (!demo())
        usleep(150000);
    if (only && !strcmp(only, "off")) {
        sheet_only = 1;
        app.minw = 40;
        app.minh = 16;
        open_sheet(S_OFF);
    }
    if (dump) {
        int w = 110, h = 30;
        const char *sh = getenv("LIFEBOAT_SHEET");
        sscanf(dump, "%dx%d", &w, &h);
        on_tick(0);
        if (getenv("LIFEBOAT_TOOLS")) {
            col = DISKS;
            sel[DISKS] = atoi(getenv("LIFEBOAT_TOOLS"));
            tools_open = 1;
        }
        if (sh && atoi(sh) == S_COPY) {
            col = DISKS;
            tool(3);
        } else if (sh)
            open_sheet(atoi(sh));
        if (getenv("LIFEBOAT_PAGE")) {
            page = atoi(getenv("LIFEBOAT_PAGE"));
            if (page == 2) extras_open();
            else if (page == 4) fix_open();
            else if (page == 3) set_open();
            else { page = 1; pkgs_open(); }
            on_tick(0);
        }
        sheet_t = -5;
        ui_dump(&app, w, h);
        return 0;
    }
    /* a fresh session starts on the extras checklist */
    if (!only && extras_wanted()) {
        extras_open();
        page = 2;
    }
    ui_run(&app);
    return 0;
}

/* --- alerts: one small card, the shotwheel kind ------------------------- */

static const char *alert_word, *alert_text;
static rgb alert_color;
static char alert_buf[240];

static void alert_draw(canvas *c)
{
    fill(c, 0, 0, c->w, c->h, PANEL);
    for (int y = 1; y < c->h - 1; y++)
        put(c, 1, y, "┃", alert_color, PANEL, 0);
    pixel_word(c, 4, 1, alert_word, alert_color, PANEL, ui_t());
    wrap(c, 4, 5, c->w - 8, alert_text, TEXT, PANEL);
    put(c, c->w - 14, c->h - 1, "any key, ok", DIM, PANEL, 0);
}

static void alert_key(key k)
{
    if (k.kind != K_MOVE)
        ui_quit = 1;
}

static int alert_fps(void)
{
    return 10;
}

int tui_alert(const char *kind, const char *text)
{
    struct app app = {alert_draw, alert_key, NULL, alert_fps, 30, 8};
    struct state st;

    alert_color = RED;
    if (!strcmp(kind, "full")) {
        alert_word = "FULL";
        alert_text = "ram's nearly full and there's no stick in. plug it in and the "
                     "changes move onto it. if you don't, things start dying.";
    } else if (!strcmp(kind, "moving")) {
        alert_word = "MOVE";
        alert_color = AMBER;
        alert_text = "ram got tight, so the changes are moving onto the stick. "
                     "leave it plugged in from now on. c on the home screen cancels.";
    } else if (!strcmp(kind, "lost")) {
        alert_word = "LOST";
        alert_text = "the stick's gone and this session was living on it. "
                     "put it back in right now.";
    } else if (!strcmp(kind, "low")) {
        alert_word = "LOW";
        alert_text = "the battery's nearly flat. plug in, or press o on the home screen "
                     "and save, because an unsaved session dies with it.";
    } else if (!strcmp(kind, "stickfull")) {
        alert_word = "FULL";
        alert_text = "the stick's nearly out of room. delete something or the session breaks.";
    } else {
        read_state(&st);
        alert_word = "STUCK";
        snprintf(alert_buf, sizeof alert_buf, "%s", text && text[0] ? text :
                 st.error[0] ? st.error : "something went wrong");
        alert_text = alert_buf;
    }
    if (getenv("LIFEBOAT_DUMP")) {
        ui_dump(&app, 62, 9);
        return 0;
    }
    ui_run(&app);
    return 0;
}
