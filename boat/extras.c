/* Extras: the things the image leaves out to stay small. This comes up once
 * on a fresh boot as a checklist, and "extras" in APPS brings it back.
 * Whatever is ticked gets installed with pacman into the session.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "boat.h"
#include "extras.h"
#include "pkgs.h"

enum { E_ITEM = EXTRA_ACTIONS, E_GO, E_REC, E_NONE, E_SKIP };

struct extra {
    int col;
    const char *group, *name, *pkgs, *note;
    int mb, rec;
};

static const struct extra X[] = {
    {0, "WEB", "firefox", "firefox", "the browser most things expect", 307, 1},
    {0, "WEB", "chromium", "chromium", "the other one, heavier", 434, 0},
    {0, "WEB", "qutebrowser", "qutebrowser", "keyboard driven, pulls in qt's web engine", 220, 0},
    {0, "WEB", "w3m", "w3m", "text only, for when nothing else starts", 2, 0},
    {0, "FILES", "yazi", "yazi", "terminal file manager with previews", 34, 1},
    {0, "FILES", "pcmanfm", "pcmanfm", "a plain window to drag files around in", 5, 0},
    {0, "FILES", "ncdu", "ncdu", "shows what is eating a disk", 1, 1},
    {0, "FILES", "rclone", "rclone", "google drive, onedrive and friends", 112, 0},
    {0, "MEDIA", "imv", "imv", "image viewer", 1, 1},
    {0, "MEDIA", "yt-dlp", "yt-dlp", "downloads videos, mpv plays them", 32, 1},
    {0, "MEDIA", "cmus", "cmus", "music player in a terminal", 1, 0},

    {1, "WRITING", "libreoffice", "libreoffice-fresh", "documents, sheets, slides. the heavy one", 423, 0},
    {1, "WRITING", "micro", "micro", "an editor where ctrl+s and ctrl+c just work", 13, 1},
    {1, "WRITING", "helix", "helix", "modal editor with everything built in", 207, 0},
    {1, "WRITING", "glow", "glow", "reads markdown in the terminal", 18, 0},
    {1, "WRITING", "keepassxc", "keepassxc", "password manager", 33, 0},
    {1, "NETWORK", "tailscale", "tailscale", "your tailnet. start it with doas tailscaled", 52, 1},
    {1, "NETWORK", "vpn tools", "wireguard-tools openvpn", "wireguard and openvpn", 2, 0},
    {1, "NETWORK", "net debugging", "nmap traceroute bind", "nmap, traceroute, dig", 34, 0},
    {1, "NETWORK", "wayvnc", "wayvnc", "share this screen", 1, 0},
    {1, "NETWORK", "croc", "croc", "send files to another machine", 23, 0},
    {1, "NETWORK", "aria2", "aria2", "fast downloads and torrents", 5, 0},
    {1, "NETWORK", "aerc", "aerc", "mail in a terminal", 18, 0},
    {1, "NETWORK", "discord", "discord", "chat", 300, 0},

    {2, "RESCUE", "boot repair", "efibootmgr grub os-prober", "fix boot entries and reinstall grub", 43, 1},
    {2, "RESCUE", "arch installer", "arch-install-scripts", "pacstrap, genfstab, arch-chroot", 1, 1},
    {2, "RESCUE", "windows passwords", "chntpw", "reset a windows login", 1, 1},
    {2, "RESCUE", "clonezilla", "clonezilla", "clone whole disks", 4, 0},
    {2, "RESCUE", "memtester", "memtester", "test the ram", 1, 0},
    {2, "RESCUE", "hardware info", "lshw inxi dmidecode", "what exactly is in this machine", 10, 1},
    {2, "RESCUE", "hdparm", "hdparm", "disk settings and secure erase", 1, 0},
    {2, "EXTRA", "btop", "btop", "a nicer process viewer", 2, 1},
    {2, "EXTRA", "fastfetch", "fastfetch", "system summary", 2, 0},
    {2, "EXTRA", "build tools", "base-devel", "lets yay compile things from the aur", 250, 0},
};
#define N ((int)(sizeof X / sizeof *X))

static int on[N], have[N], sel, scroll[3];
static const char *word = "";

static void seen_path(char *path, size_t n)
{
    snprintf(path, n, "%s/.config/lifeboat", getenv("HOME") ? getenv("HOME") : "/home/" USER);
}

int extras_wanted(void)
{
    char dir[300], path[320];
    seen_path(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/extras-seen", dir);
    return access(path, F_OK) != 0;
}

static void mark_seen(void)
{
    char dir[300], path[320];
    seen_path(dir, sizeof dir);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/extras-seen", dir);
    close(creat(path, 0644));
}

void extras_open(void)
{
    for (int i = 0; i < N; i++) {
        char first[64];
        sscanf(X[i].pkgs, "%63s", first);
        have[i] = pkgs_has(first);
        if (have[i])
            on[i] = 0;
    }
    if (have[sel])
        for (int i = 0; i < N; i++)
            if (!have[i]) { sel = i; break; }
}

static int install(void)
{
    char cmd[1600] = "doas pacman -Sy --noconfirm --needed", title[64];
    int n = 0;

    for (int i = 0; i < N; i++)
        if (on[i] && !have[i]) {
            strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1);
            strncat(cmd, X[i].pkgs, sizeof cmd - strlen(cmd) - 1);
            n++;
        }
    if (!n) {
        toast(MUTED, "nothing ticked. space ticks one, a ticks the usual set");
        return 1;
    }
    snprintf(title, sizeof title, "getting %d extra%s", n, n == 1 ? "" : "s");
    mark_seen();
    pkgs_run(title, cmd);
    memset(on, 0, sizeof on);
    return 2;
}

static void act(int action, int arg)
{
    flash(action);
    switch (action) {
    case E_ITEM:
        if (sel == arg && !have[arg])
            on[arg] = !on[arg];
        sel = arg;
        break;
    case E_REC:
        for (int i = 0; i < N; i++)
            on[i] = X[i].rec && !have[i];
        break;
    case E_NONE: memset(on, 0, sizeof on); break;
    }
}

/* the nearest item in a neighbouring column, by position in the column */
static void sideways(int dir)
{
    int col = X[sel].col + dir, pos = 0, best = -1, seen = 0;

    if (col < 0 || col > 2)
        return;
    for (int i = 0; i < sel; i++)
        pos += X[i].col == X[sel].col;
    for (int i = 0; i < N; i++)
        if (X[i].col == col) {
            best = i;
            if (seen++ >= pos)
                break;
        }
    if (best >= 0)
        sel = best;
}

int extras_key(key k)
{
    if (k.kind == K_CLICK) {
        hit *h = find_hit(k.x, k.y);
        if (h && h->action == E_GO)
            return install();
        if (h && h->action == E_SKIP) {
            mark_seen();
            return 0;
        }
        if (h && h->action >= EXTRA_ACTIONS)
            act(h->action, h->arg);
        return 1;
    }
    switch (k.kind) {
    case K_CTRLC: ui_quit = 1; return 1;
    case K_ESC: mark_seen(); return 0;
    case K_ENTER: return install();
    case K_RIGHT: case K_TAB: sideways(1); return 1;
    case K_LEFT: case K_STAB: sideways(-1); return 1;
    case K_DOWN:
        for (int i = sel + 1; i < N; i++)
            if (X[i].col == X[sel].col) { sel = i; break; }
        return 1;
    case K_UP:
        for (int i = sel - 1; i >= 0; i--)
            if (X[i].col == X[sel].col) { sel = i; break; }
        return 1;
    case K_WHEEL: return extras_key((key){k.d > 0 ? K_DOWN : K_UP, 0, 0, 0, 0});
    }
    if (k.kind != K_CHAR)
        return 1;
    switch (k.ch) {
    case ' ': case 'x': if (!have[sel]) on[sel] = !on[sel]; break;
    case 'a': act(E_REC, 0); break;
    case 'n': act(E_NONE, 0); break;
    case 'q': mark_seen(); return 0;
    case 'j': return extras_key((key){K_DOWN, 0, 0, 0, 0});
    case 'k': return extras_key((key){K_UP, 0, 0, 0, 0});
    case 'h': sideways(-1); break;
    case 'l': sideways(1); break;
    }
    return 1;
}

void extras_draw(canvas *c, int online)
{
    int picked = 0, mb = 0, end, tx, x, colw = (c->w - 2) / 3, top = 6, drawer_h = 7;
    int rows = c->h - drawer_h - top - 2;
    const char *w = "EXTRAS";
    char line[200];

    for (int i = 0; i < N; i++)
        if (on[i] && !have[i]) {
            picked++;
            mb += X[i].mb;
        }
    if (strcmp(w, word)) {
        word = w;
        ui_word_t = ui_t();
    }
    end = pixel_word(c, 2, 0, w, picked ? SIGNAL : GREEN, BG, ui_t() - ui_word_t);
    tx = end + 4;
    snprintf(line, sizeof line, "%d picked, about %dM to install", picked, mb);
    put(c, tx, 0, clip(line, c->w - tx - 2), TEXT, BG, 1);
    put(c, tx, 1, clip(online ? "kept if you save at power off" : "no network yet. connect in WIFI first",
                       c->w - tx - 2), online ? MUTED : AMBER, BG, 0);

    x = 1;
    x = ui_link(c, x, 4, "space", "tick", E_ITEM + 1000, SIGNAL, BG);
    x = ui_link(c, x, 4, "a", "the usual set", E_REC, SIGNAL, BG);
    x = ui_link(c, x, 4, "n", "none", E_NONE, MUTED, BG);
    x = ui_link(c, x, 4, "enter", "install them", E_GO, SIGNAL, BG);
    ui_link(c, x, 4, "esc", "not now", E_SKIP, MUTED, BG);

    for (int col = 0; col < 3; col++) {
        int cx = 1 + col * colw, cw = colw - 2, line_no = 0, sel_line = -1;
        const char *group = "";

        /* where the picked row sits, to keep it on screen */
        for (int i = 0; i < N; i++) {
            if (X[i].col != col)
                continue;
            if (strcmp(group, X[i].group)) {
                line_no += line_no ? 2 : 1;
                group = X[i].group;
            }
            if (i == sel)
                sel_line = line_no;
            line_no++;
        }
        if (sel_line >= 0) {
            if (sel_line < scroll[col] + 1) scroll[col] = sel_line > 1 ? sel_line - 1 : 0;
            if (sel_line >= scroll[col] + rows) scroll[col] = sel_line - rows + 1;
        }

        group = "";
        line_no = 0;
        for (int i = 0; i < N; i++) {
            int y, focused = i == sel;
            rgb bg = focused ? PANEL2 : BG;
            char size[16];

            if (X[i].col != col)
                continue;
            if (strcmp(group, X[i].group)) {
                if (line_no)
                    line_no++;
                group = X[i].group;
                y = top + line_no - scroll[col];
                if (y >= top && y < top + rows) {
                    put(c, cx + 1, y, group, MUTED, BG, 1);
                    for (int j = ulen(group) + 2; j < cw; j++)
                        put(c, cx + j, y, "▔", LINE, BG, 0);
                }
                line_no++;
            }
            y = top + line_no - scroll[col];
            line_no++;
            if (y < top || y >= top + rows)
                continue;
            fill(c, cx, y, cw, 1, bg);
            put(c, cx, y, "┃", focused ? SIGNAL : BG, bg, 0);
            put(c, cx + 2, y, have[i] ? "●" : on[i] ? "■" : "□", have[i] ? GREEN : on[i] ? SIGNAL : DIM, bg, 0);
            put(c, cx + 4, y, clip(X[i].name, cw - 14), have[i] ? DIM : on[i] || focused ? TEXT : MUTED,
                bg, focused);
            if (have[i])
                snprintf(size, sizeof size, "here");
            else
                snprintf(size, sizeof size, "%dM", X[i].mb);
            put(c, cx + cw - ulen(size) - 1, y, size, on[i] ? MUTED : DIM, bg, 0);
            add_hit(cx, y, cw, E_ITEM, i);
        }
    }

    /* drawer */
    {
        int dtop = c->h - drawer_h, y = dtop + 1;
        fill(c, 0, dtop, c->w, drawer_h, PANEL);
        for (int i = 0; i < c->w; i++)
            put(c, i, dtop, "▀", mix(BG, LINE, 0.8), BG, 0);
        x = put(c, 2, y, X[sel].name, TEXT, PANEL, 1);
        putf(c, x + 2, y, MUTED, PANEL, 0, "%s", clip(X[sel].pkgs, c->w - x - 20));
        putf(c, c->w - 12, y, DIM, PANEL, 0, "about %dM", X[sel].mb);
        put(c, 2, y + 1, clip(X[sel].note, c->w - 4), MUTED, PANEL, 0);
        put(c, 2, y + 3, clip(have[sel] ? "already installed" :
                              "everything else: p",
                              c->w - 4), DIM, PANEL, 0);
    }
}
