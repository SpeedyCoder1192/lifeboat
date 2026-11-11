/* Packages. Everything in the repos and everything installed, read straight
 * from pacman's databases through libalpm, so browsing 15000 packages costs
 * nothing. The aur is searched through yay. Installing and removing run
 * pacman or yay underneath with their output in the drawer.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <alpm.h>

#include "boat.h"
#include "pkgs.h"

enum {
    P_FIND = PKG_ACTIONS, P_REFRESH, P_UPDATE, P_BACK, P_LOG, P_COL, P_ITEM, P_GO, P_REMOVE, P_CANCEL
};
enum { FOUND, MINE, NEW };

#define YAY "yay --noconfirm --sudo doas --answerclean None --answerdiff None --answeredit None"

struct pkg {
    const char *name, *ver, *desc, *repo, *newver;
    long long size;
    int installed, aur;
};

static alpm_handle_t *handle;
static struct pkg *all;
static int nall, nsync;
static int *view[3], count[3], col, sel[3], scroll[3];
static time_t lists_at;

static struct field query;
static int finding;
static char searched[128];

/* aur hits come from a thread, since yay takes a second or two */
static struct { char name[80], ver[40], desc[200]; } aur_hit[60], aur_new[60];
static struct pkg aur_pkg[60];
static int naur, naur_new, aur_ready, aur_busy;
static char aur_query[128];
static pthread_mutex_t aur_lock = PTHREAD_MUTEX_INITIALIZER;

static pid_t job;
static int job_fd = -1, job_failed, show_log;
static char job_title[96], lines[200][200], partial[200];
static int nlines;
static double job_t;

static int armed;           /* 0, P_GO or P_REMOVE: waiting for a second press */
static struct pkg *armed_for;
static const char *word = "";

/* --- the databases ------------------------------------------------------ */

static int by_name(const void *a, const void *b)
{
    return strcmp(((const struct pkg *)a)->name, ((const struct pkg *)b)->name);
}

static void load(void)
{
    char line[256];
    alpm_errno_t err;
    alpm_db_t *local;
    alpm_list_t *i, *dbs;
    struct stat sb;
    FILE *f;
    int cap = 0;

    if (handle)
        alpm_release(handle);
    free(all);
    all = NULL;
    nall = nsync = 0;
    handle = alpm_initialize("/", "/var/lib/pacman/", &err);
    if (!handle)
        return;

    f = fopen("/etc/pacman.conf", "r");
    while (f && fgets(line, sizeof line, f)) {
        char *end = strchr(line, ']');
        if (line[0] == '[' && end && strncmp(line, "[options]", 9)) {
            *end = 0;
            alpm_register_syncdb(handle, line + 1, 0);
        }
    }
    if (f)
        fclose(f);
    lists_at = stat("/var/lib/pacman/sync/core.db", &sb) ? 0 : sb.st_mtime;

    local = alpm_get_localdb(handle);
    dbs = alpm_get_syncdbs(handle);
    for (alpm_list_t *d = dbs; d; d = d->next)
        for (i = alpm_db_get_pkgcache(d->data); i; i = i->next) {
            alpm_pkg_t *p = i->data, *have = alpm_db_get_pkg(local, alpm_pkg_get_name(p));
            if (nall == cap)
                all = realloc(all, sizeof *all * (cap = cap ? cap * 2 : 4096));
            all[nall] = (struct pkg){alpm_pkg_get_name(p), alpm_pkg_get_version(p),
                                     alpm_pkg_get_desc(p), alpm_db_get_name(d->data), NULL,
                                     alpm_pkg_get_isize(p), have != NULL, 0};
            if (have && alpm_pkg_vercmp(alpm_pkg_get_version(p), alpm_pkg_get_version(have)) > 0) {
                all[nall].newver = all[nall].ver;
                all[nall].ver = alpm_pkg_get_version(have);
            }
            nall++;
        }
    qsort(all, nall, sizeof *all, by_name);
    nsync = nall;

    /* installed but in no repo: it came from the aur or was built by hand */
    for (i = alpm_db_get_pkgcache(local); i; i = i->next) {
        alpm_pkg_t *p = i->data;
        struct pkg want = {.name = alpm_pkg_get_name(p)};
        if (bsearch(&want, all, nsync, sizeof *all, by_name))
            continue;
        if (nall == cap)
            all = realloc(all, sizeof *all * (cap = cap ? cap * 2 : 4096));
        all[nall++] = (struct pkg){want.name, alpm_pkg_get_version(p), alpm_pkg_get_desc(p),
                                   "aur", NULL, alpm_pkg_get_isize(p), 1, 1};
    }
    for (int v = 0; v < 3; v++)
        view[v] = realloc(view[v], sizeof(int) * (nall + 64));
    searched[0] = 1;        /* forces the views to be rebuilt */
}

static struct pkg *at(int v, int idx)
{
    int id = view[v][idx];
    return id < 0 ? &aur_pkg[-id - 1] : &all[id];
}

static void rebuild(void)
{
    const char *q = query.text;

    count[FOUND] = count[MINE] = count[NEW] = 0;
    /* names that match come first, then the ones that only mention it */
    for (int pass = 0; pass < (q[0] ? 2 : 1); pass++)
        for (int i = 0; i < nsync; i++) {
            int in_name = !q[0] || strcasestr(all[i].name, q) != NULL;
            if (pass == 0 ? in_name : (!in_name && all[i].desc && strcasestr(all[i].desc, q)))
                view[FOUND][count[FOUND]++] = i;
        }
    for (int i = 0; i < naur; i++)
        view[FOUND][count[FOUND]++] = -i - 1;
    for (int i = 0; i < nall; i++) {
        if (all[i].installed && (!q[0] || strcasestr(all[i].name, q)))
            view[MINE][count[MINE]++] = i;
        if (all[i].newver)
            view[NEW][count[NEW]++] = i;
    }
    for (int v = 0; v < 3; v++) {
        if (sel[v] >= count[v])
            sel[v] = count[v] ? count[v] - 1 : 0;
    }
    snprintf(searched, sizeof searched, "%s", q);
}

/* --- the aur, through yay ----------------------------------------------- */

static void *aur_search(void *arg)
{
    static char out[65536];
    char q[128];
    int n = 0;

    snprintf(q, sizeof q, "%s", (char *)arg);
    free(arg);
    if (run(out, sizeof out, "yay", "-Ssa", "--topdown", q) == 0)
        for (char *p = out; *p && n < 60;) {
            size_t len = strcspn(p, "\n");
            char *next = p + len + (p[len] == '\n');
            p[len] = 0;
            if (!strncmp(p, "aur/", 4) &&
                sscanf(p + 4, "%79s %39s", aur_new[n].name, aur_new[n].ver) == 2) {
                size_t dlen = strcspn(next, "\n");
                const char *d = next;
                while (*d == ' ')
                    d++;
                snprintf(aur_new[n].desc, sizeof aur_new[n].desc, "%.*s",
                         (int)(dlen - (d - next)), d);
                n++;
            }
            p = next;
        }
    pthread_mutex_lock(&aur_lock);
    naur_new = n;
    snprintf(aur_query, sizeof aur_query, "%s", q);
    aur_ready = 1;
    aur_busy = 0;
    pthread_mutex_unlock(&aur_lock);
    return NULL;
}

static void aur_start(void)
{
    pthread_t th;

    naur = 0;
    searched[0] = 1;
    if (strlen(query.text) < 2 || aur_busy)
        return;
    aur_busy = 1;
    if (pthread_create(&th, NULL, aur_search, strdup(query.text)))
        aur_busy = 0;
    else
        pthread_detach(th);
}

static void aur_collect(void)
{
    alpm_db_t *local = handle ? alpm_get_localdb(handle) : NULL;

    pthread_mutex_lock(&aur_lock);
    if (aur_ready && !strcmp(aur_query, query.text)) {
        naur = naur_new;
        memcpy(aur_hit, aur_new, sizeof aur_hit);
        for (int i = 0; i < naur; i++)
            aur_pkg[i] = (struct pkg){aur_hit[i].name, aur_hit[i].ver, aur_hit[i].desc, "aur", NULL, 0,
                                      local && alpm_db_get_pkg(local, aur_hit[i].name) != NULL, 1};
        searched[0] = 1;
    }
    aur_ready = 0;
    pthread_mutex_unlock(&aur_lock);
}

/* --- jobs: pacman or yay with their output in the drawer ---------------- */

static void add_line(const char *text)
{
    if (!text[0])
        return;
    if (nlines == 200) {
        memmove(lines, lines + 1, sizeof *lines * 199);
        nlines--;
    }
    snprintf(lines[nlines++], sizeof *lines, "%s", text);
}

static void start(const char *title, const char *cmd)
{
    int p[2];

    if (job) {
        toast(MUTED, "wait for this one to finish");
        return;
    }
    if (demo())
        cmd = "for n in glibc mesa firefox; do echo \"installing $n...\"; sleep 0.7; done";
    if (pipe(p))
        return;
    job = fork();
    if (job == 0) {
        int null = open("/dev/null", O_RDONLY);
        dup2(null, 0);
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }
    close(p[1]);
    job_fd = p[0];
    fcntl(job_fd, F_SETFL, O_NONBLOCK);
    snprintf(job_title, sizeof job_title, "%s", title);
    job_t = ui_t();
    job_failed = 0;
    partial[0] = 0;
    add_line("");
    snprintf(lines[nlines < 200 ? nlines++ : 199], sizeof *lines, "$ %s", cmd);
    armed = 0;
}

static void pump(void)
{
    char buf[1024];
    ssize_t r;
    int status;

    if (!job)
        return;
    while ((r = read(job_fd, buf, sizeof buf)) > 0)
        for (ssize_t i = 0; i < r; i++) {
            size_t n = strlen(partial);
            if (buf[i] == '\n' || buf[i] == '\r') {
                add_line(partial);
                partial[0] = 0;
            } else if (buf[i] == 27) {
                /* colour codes: skip to the letter that ends them */
                while (i + 1 < r && !isalpha((unsigned char)buf[i + 1]))
                    i++;
                i++;
            } else if (n < sizeof partial - 1 && (unsigned char)buf[i] >= 32) {
                partial[n] = buf[i];
                partial[n + 1] = 0;
            }
        }
    if (r == 0 || (r < 0 && errno != EAGAIN)) {
        add_line(partial);
        close(job_fd);
        job_fd = -1;
        waitpid(job, &status, 0);
        job = 0;
        job_failed = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
        if (job_failed) {
            toast(RED, "that didn't work. l shows what it said");
        } else {
            toast(GREEN, "done");
            burst(GREEN, 16, 1.5);
        }
        load();
    }
}

static int safe(const char *name)
{
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("@._+-", *p))
            return 0;
    return name[0] != 0 && name[0] != '-';
}

static void install(struct pkg *p)
{
    char cmd[512], title[96];

    if (!safe(p->name))
        return;
    snprintf(title, sizeof title, "getting %s", p->name);
    if (p->aur)
        snprintf(cmd, sizeof cmd, YAY " -S %s", p->name);
    else
        /* lists older than a day go stale against the mirrors, so fetch first */
        snprintf(cmd, sizeof cmd, "doas pacman -S%s --noconfirm --needed %s",
                 time(NULL) - lists_at > 86400 ? "y" : "", p->name);
    start(title, cmd);
}

static void removal(struct pkg *p)
{
    char cmd[256], title[96];

    if (!safe(p->name))
        return;
    snprintf(title, sizeof title, "removing %s", p->name);
    snprintf(cmd, sizeof cmd, "doas pacman -Rns --noconfirm %s", p->name);
    start(title, cmd);
}

static struct pkg *picked(void)
{
    return count[col] ? at(col, sel[col]) : NULL;
}

void pkgs_act(int action, int arg)
{
    struct pkg *p = picked();

    flash(action);
    if (action != P_GO && action != P_REMOVE)
        armed = 0;
    switch (action) {
    case P_FIND: finding = 1; col = FOUND; break;
    case P_REFRESH: start("fetching the package lists", "doas pacman -Sy"); break;
    case P_UPDATE:
        start("updating everything", "doas pacman -Syu --noconfirm && " YAY " -Sua");
        break;
    case P_LOG: show_log = !show_log; break;
    case P_COL: col = arg; break;
    case P_ITEM:
        col = arg / 100000;
        sel[col] = arg % 100000;
        break;
    case P_CANCEL: break;
    case P_GO:
    case P_REMOVE:
        if (!p)
            break;
        if (action == P_REMOVE && !p->installed) {
            toast(MUTED, "it isn't installed");
            break;
        }
        if (action == P_GO && p->installed && !p->newver)
            action = P_REMOVE;
        /* the first press asks, the second one does it */
        if (armed != action || armed_for != p) {
            armed = action;
            armed_for = p;
            break;
        }
        armed = 0;
        if (action == P_GO)
            install(p);
        else
            removal(p);
        break;
    }
}

int pkgs_key(key k)
{
    int n = count[col];

    if (k.kind == K_CLICK) {
        hit *h = find_hit(k.x, k.y);
        if (h && h->action == P_BACK)
            return 0;
        if (h && h->action >= PKG_ACTIONS)
            pkgs_act(h->action, h->arg);
        return 1;
    }
    if (k.kind == K_WHEEL) {
        sel[col] += k.d * 3;
        if (sel[col] >= n) sel[col] = n - 1;
        if (sel[col] < 0) sel[col] = 0;
        return 1;
    }
    switch (k.kind) {
    case K_MOVE: return 1;
    case K_CTRLC: ui_quit = 1; return 1;
    case K_ESC:
        if (finding) finding = 0;
        else if (armed) armed = 0;
        else if (query.text[0]) { query.text[0] = 0; naur = 0; }
        else return 0;
        return 1;
    case K_ENTER:
        if (finding) {
            finding = 0;
            aur_start();
        } else {
            pkgs_act(P_GO, 0);
        }
        return 1;
    case K_TAB: case K_RIGHT: col = (col + 1) % 3; finding = armed = 0; return 1;
    case K_STAB: case K_LEFT: col = (col + 2) % 3; finding = armed = 0; return 1;
    case K_DOWN: if (n) sel[col] = sel[col] + 1 < n ? sel[col] + 1 : n - 1; armed = 0; return 1;
    case K_UP: if (sel[col] > 0) sel[col]--; armed = 0; return 1;
    }
    if (finding) {
        if (field_key(&query, k)) {
            sel[FOUND] = scroll[FOUND] = sel[MINE] = scroll[MINE] = 0;
            naur = 0;
        }
        return 1;
    }
    if (k.kind != K_CHAR)
        return 1;
    switch (k.ch) {
    case 'q': case 'p': return 0;
    case '/': pkgs_act(P_FIND, 0); break;
    case 'r': pkgs_act(P_REFRESH, 0); break;
    case 'u': pkgs_act(P_UPDATE, 0); break;
    case 'l': pkgs_act(P_LOG, 0); break;
    case 'x': pkgs_act(P_REMOVE, 0); break;
    case 'j': if (n) sel[col] = sel[col] + 1 < n ? sel[col] + 1 : n - 1; armed = 0; break;
    case 'k': if (sel[col] > 0) sel[col]--; armed = 0; break;
    case 'g': sel[col] = 0; break;
    case 'G': sel[col] = n ? n - 1 : 0; break;
    case ' ': sel[col] = sel[col] + 15 < n ? sel[col] + 15 : (n ? n - 1 : 0); break;
    }
    return 1;
}

void pkgs_run(const char *title, const char *cmd)
{
    if (!handle)
        load();
    start(title, cmd);
    show_log = 1;
}

int pkgs_busy(void)
{
    return job != 0;
}

int pkgs_has(const char *name)
{
    if (!handle)
        load();
    return handle && alpm_db_get_pkg(alpm_get_localdb(handle), name) != NULL;
}

void pkgs_open(void)
{
    if (!handle)
        load();
}

void pkgs_tick(void)
{
    pump();
    aur_collect();
    if (strcmp(searched, query.text))
        rebuild();
}

int pkgs_fps(void)
{
    return job || aur_busy || finding ? 15 : 2;
}

/* --- drawing ------------------------------------------------------------ */

static const char *ago(time_t then)
{
    static char buf[32];
    long s = time(NULL) - then;
    if (s < 3600) snprintf(buf, sizeof buf, "%ldm ago", s / 60);
    else if (s < 86400) snprintf(buf, sizeof buf, "%ldh ago", s / 3600);
    else snprintf(buf, sizeof buf, "%ldd ago", s / 86400);
    return buf;
}

static void board(canvas *c, int top, int height)
{
    static const char *titles[] = {"FOUND", "INSTALLED", "UPDATES"};
    int colw = (c->w - 2) / 3, rows = height - 2;

    for (int i = 0; i < 3; i++) {
        int x = 1 + i * colw, w = colw - 2, focused = i == col, n = count[i], e;
        char num[16];

        e = put(c, x + 1, top, i == FOUND && !query.text[0] ? "EVERYTHING" : titles[i],
                focused ? TEXT : MUTED, BG, 1);
        snprintf(num, sizeof num, "%d", n);
        e = put(c, e + 1, top, num, n ? SIGNAL : DIM, BG, 1);
        if (i == FOUND && (finding || query.text[0])) {
            e = put(c, e + 2, top, "/", SIGNAL, BG, 1);
            if (finding)
                field_draw(&query, c, e, top, w - (e - x) - 1, BG);
            else
                e = put(c, e + 1, top, clip(query.text, w - (e - x) - 12), TEXT, BG, 0);
            if (aur_busy && !finding) {
                e = put(c, e + 1, top, spinner(), AMBER, BG, 0);
                put(c, e + 1, top, "aur", DIM, BG, 0);
            }
        }
        for (int j = 0; j < w; j++)
            put(c, x + j, top + 1, "▔", focused ? SIGNAL : LINE, BG, 0);
        add_hit(x, top, w, P_COL, i);
        if (rows < 1)
            continue;

        if (focused) {
            if (sel[i] < scroll[i]) scroll[i] = sel[i];
            if (sel[i] >= scroll[i] + rows) scroll[i] = sel[i] - rows + 1;
        }
        if (scroll[i] > n - rows) scroll[i] = n > rows ? n - rows : 0;

        if (!n) {
            const char *empty = i == FOUND ? (nsync ? "nothing by that name" : "no package lists yet. r fetches them") :
                                i == MINE ? "nothing" : (nsync ? "all up to date" : "-");
            put(c, x + 1, top + 3, clip(empty, w - 2), DIM, BG, 0);
            continue;
        }
        for (int r = 0; r < rows && scroll[i] + r < n; r++) {
            int idx = scroll[i] + r, y = top + 2 + r, on = idx == sel[i];
            struct pkg *p = at(i, idx);
            rgb bg = on && focused ? PANEL2 : on ? PANEL : BG;
            rgb side_c = on ? MUTED : DIM;
            const char *side = i == NEW ? p->newver : p->ver;
            int sw, tag = 0;

            fill(c, x, y, w, 1, bg);
            put(c, x, y, "┃", on && focused ? SIGNAL : on ? LINE : BG, bg, 0);
            if (i == FOUND && p->aur) {
                side = "aur";
                side_c = mix(bg, SIGNAL, on ? 0.9 : 0.6);
            }
            if (i == FOUND && p->installed) {
                put(c, x + 2, y, "●", p->newver ? AMBER : GREEN, bg, 0);
                tag = 2;
            }
            sw = ulen(side);
            if (sw > w / 2)
                sw = 0;
            put(c, x + 2 + tag, y, clip(p->name, w - sw - 4 - tag), on ? TEXT : MUTED, bg, on);
            if (sw)
                put(c, x + w - sw - 1, y, side, side_c, bg, 0);
            add_hit(x, y, w, P_ITEM, i * 100000 + idx);
        }
        if (n - scroll[i] - rows > 0) {
            snprintf(num, sizeof num, "+%d", n - scroll[i] - rows);
            put(c, x + w - strlen(num) - 1, top, num, DIM, BG, 0);
        }
    }
}

static void show_lines(canvas *c, int top, int rows)
{
    int first = nlines > rows ? nlines - rows : 0;

    if (!nlines)
        put(c, 2, top, "nothing has run yet", DIM, PANEL, 0);
    for (int i = first; i < nlines; i++) {
        const char *l = lines[i];
        rgb color = MUTED;
        if (l[0] == '$') color = SIGNAL;
        else if (strcasestr(l, "error") || strcasestr(l, "failed") || strcasestr(l, "conflict")) color = RED;
        else if (!strncmp(l, "warning", 7)) color = AMBER;
        else if (!strncmp(l, "::", 2) || !strncmp(l, "==>", 3)) color = TEXT;
        put(c, 2, top + i - first, clip(l, c->w - 4), color, PANEL, 0);
    }
}

static void drawer(canvas *c, int top, int height)
{
    struct pkg *p = picked();
    int y = top + 1, x;

    fill(c, 0, top, c->w, height, PANEL);
    for (int i = 0; i < c->w; i++)
        put(c, i, top, "▀", mix(BG, job ? AMBER : show_log ? SIGNAL : LINE, 0.8), BG, 0);

    if (job) {
        long secs = ui_t() - job_t;
        put(c, 2, y, spinner(), AMBER, PANEL, 0);
        put(c, 4, y, clip(job_title, c->w - 16), TEXT, PANEL, 1);
        putf(c, c->w - 8, y, MUTED, PANEL, 0, "%ld:%02ld", secs / 60, secs % 60);
        progress(c, 2, y + 1, c->w - 4, -1, PANEL);
        show_lines(c, top + 3, height - 4);
        return;
    }
    if (show_log) {
        put(c, 2, y, "what the last one said", TEXT, PANEL, 1);
        show_lines(c, top + 2, height - 3);
        return;
    }
    if (!p) {
        put(c, 2, y, "nothing picked", MUTED, PANEL, 0);
        return;
    }

    x = put(c, 2, y, clip(p->name, 44), TEXT, PANEL, 1);
    x = put(c, x + 2, y, p->ver, MUTED, PANEL, 0);
    if (p->newver) {
        x = put(c, x + 1, y, "→", DIM, PANEL, 0);
        x = put(c, x + 1, y, p->newver, AMBER, PANEL, 0);
    }
    x = put(c, x + 2, y, p->repo, p->aur ? SIGNAL : DIM, PANEL, 0);
    if (p->size)
        put(c, x + 2, y, human(p->size), DIM, PANEL, 0);
    put(c, 2, y + 1, clip(p->desc ? p->desc : "", c->w - 4), MUTED, PANEL, 0);

    x = 1;
    if (armed && armed_for == p) {
        const char *what = armed == P_REMOVE ? "really remove it" :
                           p->aur ? "build and install it" : p->newver ? "update it" : "install it";
        x = ui_link(c, x, y + 3, "enter", what, armed, armed == P_REMOVE ? RED : SIGNAL, PANEL);
        ui_link(c, x, y + 3, "esc", "never mind", P_CANCEL, MUTED, PANEL);
        if (p->aur && armed == P_GO)
            put(c, 2, y + 4, clip("from the aur, so it's built here. bigger ones want base-devel first",
                                  c->w - 4), DIM, PANEL, 0);
    } else {
        if (!p->installed || p->newver)
            x = ui_link(c, x, y + 3, "enter", p->newver ? "update" : p->aur ? "build" : "install",
                        P_GO, SIGNAL, PANEL);
        if (p->installed)
            x = ui_link(c, x, y + 3, "x", "remove", P_REMOVE, MUTED, PANEL);
        put(c, 2, y + 4, clip("it lands in this session's changes, so it lasts as long as they do",
                              c->w - 4), DIM, PANEL, 0);
    }
}

void pkgs_draw(canvas *c)
{
    const char *w;
    rgb color;
    char line[160];
    int end, tx, x, installed = 0, drawer_h;

    for (int i = 0; i < nall; i++)
        installed += all[i].installed;

    if (job) { w = "BUSY"; color = AMBER; }
    else if (job_failed) { w = "STUCK"; color = RED; }
    else if (count[NEW]) { w = "NEW"; color = SIGNAL; }
    else { w = "PKGS"; color = nsync ? GREEN : DIM; }
    if (strcmp(w, word)) {
        word = w;
        ui_word_t = ui_t();
    }
    end = pixel_word(c, 2, 0, w, color, BG, ui_t() - ui_word_t);

    tx = end + 4;
    snprintf(line, sizeof line, "%d installed, %d in the repos, %d to update", installed, nsync, count[NEW]);
    put(c, tx, 0, clip(line, c->w - tx - 2), TEXT, BG, 1);
    if (lists_at)
        snprintf(line, sizeof line, "lists fetched %s", ago(lists_at));
    else
        snprintf(line, sizeof line, "no package lists yet");
    put(c, tx, 1, clip(line, c->w - tx - 2), MUTED, BG, 0);
    put(c, tx, 2, clip(job ? job_title : job_failed ? "the last one failed. l shows what it said" :
                       !nsync ? "r fetches the lists, then everything in the repos shows up here" :
                       "/ searches the repos and the aur", c->w - tx - 2), DIM, BG, 0);

    x = 1;
    x = ui_link(c, x, 4, "/", "search", P_FIND, SIGNAL, BG);
    x = ui_link(c, x, 4, "r", "fetch lists", P_REFRESH, SIGNAL, BG);
    x = ui_link(c, x, 4, "u", "update all", P_UPDATE, SIGNAL, BG);
    x = ui_link(c, x, 4, "l", show_log ? "hide log" : "log", P_LOG, MUTED, BG);
    ui_link(c, x, 4, "esc", "back", P_BACK, MUTED, BG);

    drawer_h = job || show_log ? (c->h - 6) / 2 : 7;
    if (drawer_h < 7)
        drawer_h = 7;
    board(c, 6, c->h - drawer_h - 6);
    drawer(c, c->h - drawer_h, drawer_h);
}
