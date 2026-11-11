#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#include "probe.h"

#define IWD "net.connman.iwd"

static struct snap shared, work;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static int poked;

static int bt_scan_wanted;
static char wifi_msg[160];
static int wifi_done;       /* 1 worked, -1 didn't */

static void copy(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src);
}

/* --- apps -------------------------------------------------------------- */

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const struct appent *)a)->name, ((const struct appent *)b)->name);
}

static void add_app(struct snap *s, const char *name, const char *exec, const char *src, int term)
{
    struct appent *a;
    if (s->napps >= 300)
        return;
    for (int i = 0; i < s->napps; i++)
        if (!strcmp(s->apps[i].name, name))
            return;
    a = &s->apps[s->napps++];
    copy(a->name, sizeof a->name, name);
    copy(a->exec, sizeof a->exec, exec);
    copy(a->src, sizeof a->src, src);
    a->term = term;
}

/* Exec= minus the %f style codes and flatpak's @@ markers */
static void clean_exec(char *exec)
{
    char out[300] = "";
    size_t o = 0;
    for (char *tok = strtok(exec, " "); tok; tok = strtok(NULL, " ")) {
        if ((tok[0] == '%' && tok[1] && !tok[2]) || !strncmp(tok, "@@", 2))
            continue;
        o += snprintf(out + o, sizeof out - o, "%s%s", o ? " " : "", tok);
        if (o >= sizeof out)
            break;
    }
    strcpy(exec, out);
}

static void desktop_dir(struct snap *s, const char *dir, const char *src)
{
    struct dirent *e;
    DIR *d = opendir(dir);

    while (d && (e = readdir(d))) {
        char path[512], line[512], name[64] = "", exec[300] = "";
        int term = 0, hide = 0, app = 0, main = 0;
        size_t len = strlen(e->d_name);
        FILE *f;

        if (len < 9 || strcmp(e->d_name + len - 8, ".desktop"))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        f = fopen(path, "r");
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '[') {
                main = !strcmp(line, "[Desktop Entry]");
                continue;
            }
            if (!main)
                continue;
            if (!strncmp(line, "Name=", 5) && !name[0]) copy(name, sizeof name, line + 5);
            else if (!strncmp(line, "Exec=", 5)) copy(exec, sizeof exec, line + 5);
            else if (!strcmp(line, "Terminal=true")) term = 1;
            else if (!strcmp(line, "NoDisplay=true") || !strcmp(line, "Hidden=true")) hide = 1;
            else if (!strcmp(line, "Type=Application")) app = 1;
        }
        if (f)
            fclose(f);
        if (!app || hide || !name[0] || !exec[0])
            continue;
        clean_exec(exec);
        if (strstr(exec, "gparted"))
            continue;       /* its launcher wants pkexec. "partitions" above runs it through doas */
        /* already covered by the built in entries, or of no use from a launcher */
        static const char *skip[] = {"foot", "footclient", "htop", "nnn", "lstopo", "qv4l2", "qvidcap",
                                     "bssh", "bvnc", "avahi-discover", "cmake-gui", "electron", NULL};
        int dull = 0;
        for (const char **k = skip; *k; k++) {
            size_t kl = strlen(*k);
            if (!strncmp(exec, *k, kl) && (exec[kl] == 0 || exec[kl] == ' '))
                dull = 1;
        }
        if (dull)
            continue;
        add_app(s, name, exec, src, term);
    }
    if (d)
        closedir(d);
}

static void appimage_dir(struct snap *s, const char *dir)
{
    struct dirent *e;
    DIR *d = opendir(dir);

    while (d && (e = readdir(d))) {
        char exec[300], name[64];
        size_t len = strlen(e->d_name);
        if (len < 10 || strcasecmp(e->d_name + len - 9, ".appimage"))
            continue;
        snprintf(exec, sizeof exec, "chmod +x '%s/%s' && exec '%s/%s'", dir, e->d_name, dir, e->d_name);
        snprintf(name, sizeof name, "%.*s", (int)(len - 9 < 63 ? len - 9 : 63), e->d_name);
        add_app(s, name, exec, "appimage", 0);
    }
    if (d)
        closedir(d);
}

static void probe_apps(struct snap *s)
{
    const char *home = getenv("HOME") ? getenv("HOME") : "/home/" USER;
    char path[300];
    int fixed;

    s->napps = 0;
    add_app(s, "terminal", "foot", "lifeboat", 0);
    add_app(s, "files", "nnn -de", "lifeboat", 1);
    add_app(s, "fix", "@fix", "lifeboat", 0);
    add_app(s, "settings", "@settings", "lifeboat", 0);
    add_app(s, "extras", "@extras", "lifeboat", 0);
    add_app(s, "packages", "@pkgs", "lifeboat", 0);
    add_app(s, "get a flatpak", "@install", "lifeboat", 0);
    add_app(s, "bluetooth", "@bt", "lifeboat", 0);
    add_app(s, "sound output", "@audio", "lifeboat", 0);
    add_app(s, "partitions", "doas gparted", "lifeboat", 0);
    add_app(s, "processes", "htop", "lifeboat", 1);
    fixed = s->napps;

    snprintf(path, sizeof path, "%s/.local/share/flatpak/exports/share/applications", home);
    desktop_dir(s, path, "flatpak");
    desktop_dir(s, "/var/lib/flatpak/exports/share/applications", "flatpak");
    snprintf(path, sizeof path, "%s/.local/share/applications", home);
    desktop_dir(s, path, "yours");
    desktop_dir(s, "/usr/share/applications", "system");
    for (const char **d = (const char *[]){"Applications", "Downloads", "Desktop", NULL}; *d; d++) {
        snprintf(path, sizeof path, "%s/%s", home, *d);
        appimage_dir(s, path);
    }
    qsort(s->apps + fixed, s->napps - fixed, sizeof *s->apps, by_name);
}

/* --- disks ------------------------------------------------------------- */

static void pair(const char *line, const char *key, char *out, size_t n)
{
    char want[32];
    const char *p;

    out[0] = 0;
    snprintf(want, sizeof want, " %s=\"", key);
    p = strstr(line, want);
    if (!p)
        return;
    p += strlen(want);
    for (size_t i = 0; *p && *p != '"' && i + 1 < n; i++) {
        out[i] = *p++;
        out[i + 1] = 0;
    }
}

/* smart verdicts, asked for once per disk and kept for a few minutes */
static struct { char disk[40], text[120]; int level; time_t at; } healths[16];

static void health_of(struct disk *d)
{
    char out[200] = "";
    int slot = -1;

    for (int i = 0; i < 16; i++) {
        if (!strcmp(healths[i].disk, d->parent)) { slot = i; break; }
        if (slot < 0 && !healths[i].disk[0]) slot = i;
    }
    if (slot < 0)
        slot = 0;
    if (strcmp(healths[slot].disk, d->parent) || time(NULL) - healths[slot].at > 300) {
        copy(healths[slot].disk, sizeof healths[slot].disk, d->parent);
        healths[slot].at = time(NULL);
        healths[slot].level = 0;
        healths[slot].text[0] = 0;
        if (!run(out, sizeof out, "doas", "-n", SELF, "health", d->parent)) {
            char *words = strchr(out, ' ');
            healths[slot].level = !strncmp(out, "ok", 2) ? 1 : !strncmp(out, "warn", 4) ? 2 :
                                  !strncmp(out, "bad", 3) ? 3 : 0;
            copy(healths[slot].text, sizeof healths[slot].text, words ? words + 1 : "");
        }
    }
    d->hlevel = healths[slot].level;
    copy(d->health, sizeof d->health, healths[slot].text);
}

static void probe_disks(struct snap *s)
{
    static char out[16384];
    char bootdev[64] = "";
    FILE *bf = fopen(RUN_DIR "/bootdev", "r");
    if (bf) {
        if (fscanf(bf, "%63s", bootdev) != 1)
            bootdev[0] = 0;
        fclose(bf);
    }
    char line[1024], num[32], type[16], model[40] = "", tran[8] = "", parent[40] = "";
    struct conf c;
    long long psize = 0;
    int rm = 0;

    load_conf(&c);
    s->ndisks = 0;
    s->stick = 0;
    if (run(out, sizeof out, "lsblk", "-P", "-b", "-o",
            "TYPE,PATH,SIZE,FSTYPE,LABEL,MOUNTPOINT,RM,TRAN,MODEL"))
        return;
    for (char *p = out; *p && s->ndisks < 48;) {
        size_t len = strcspn(p, "\n");
        struct disk *d = &s->disks[s->ndisks];

        snprintf(line, sizeof line, " %.*s", (int)len, p);
        p += len + (p[len] == '\n');
        pair(line, "TYPE", type, sizeof type);
        if (!strcmp(type, "disk")) {
            /* partitions don't repeat these, so remember the parent's */
            pair(line, "MODEL", model, sizeof model);
            pair(line, "TRAN", tran, sizeof tran);
            pair(line, "RM", num, sizeof num);
            rm = atoi(num);
            pair(line, "PATH", parent, sizeof parent);
            pair(line, "SIZE", num, sizeof num);
            psize = atoll(num);
        }
        pair(line, "FSTYPE", d->fs, sizeof d->fs);
        pair(line, "PATH", d->dev, sizeof d->dev);
        if (!d->fs[0] || strstr(d->dev, "zram") || strstr(d->dev, "/loop") ||
            strstr(d->dev, DM) || !strcmp(d->fs, "squashfs"))
            continue;
        pair(line, "LABEL", d->label, sizeof d->label);
        /* the boot partition of a flashed image, and the whole-disk view of it */
        if (!strcmp(d->label, "LBEFI") || (!strcmp(type, "disk") && !strcmp(d->fs, "iso9660")))
            continue;
        if (!strcmp(d->label, "LBBOOT") || !strcmp(d->dev, bootdev))
            snprintf(d->label, sizeof d->label, "lifeboat");
        pair(line, "MOUNTPOINT", d->mount, sizeof d->mount);
        pair(line, "SIZE", num, sizeof num);
        d->size = atoll(num);
        copy(d->model, sizeof d->model, model);
        copy(d->tran, sizeof d->tran, tran);
        d->rm = rm;
        copy(d->parent, sizeof d->parent, parent[0] ? parent : d->dev);
        d->psize = psize ? psize : d->size;
        health_of(d);
        d->stick = !strcmp(d->label, c.label);
        if (d->stick)
            s->stick = 1;
        s->ndisks++;
    }
    if (!s->stick) {
        /* a save file on the boot stick doesn't show up as a partition */
        char dev[128];
        s->stick = stick_dev(dev, sizeof dev);
    }
}

/* --- small readings ---------------------------------------------------- */

static int read_line(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (!f)
        return -1;
    if (!fgets(out, n, f))
        out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return 0;
}

static void probe_battery(struct snap *s)
{
    struct dirent *e;
    DIR *d = opendir("/sys/class/power_supply");

    s->bat = s->bat_mins = -1;
    s->charging = 0;
    while (d && (e = readdir(d))) {
        char path[300], val[32];
        if (strncmp(e->d_name, "BAT", 3))
            continue;
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", e->d_name);
        if (!read_line(path, val, sizeof val))
            s->bat = atoi(val);
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/status", e->d_name);
        read_line(path, val, sizeof val);
        s->charging = !strcmp(val, "Charging") || !strcmp(val, "Full");
        if (!strcmp(val, "Discharging")) {
            /* some batteries count energy, some count charge */
            static const char *pairs[][2] = {{"energy_now", "power_now"}, {"charge_now", "current_now"}};
            for (int i = 0; i < 2 && s->bat_mins < 0; i++) {
                long long have = 0, rate = 0;
                snprintf(path, sizeof path, "/sys/class/power_supply/%s/%s", e->d_name, pairs[i][0]);
                if (!read_line(path, val, sizeof val))
                    have = atoll(val);
                snprintf(path, sizeof path, "/sys/class/power_supply/%s/%s", e->d_name, pairs[i][1]);
                if (!read_line(path, val, sizeof val))
                    rate = atoll(val);
                if (have > 0 && rate > 0)
                    s->bat_mins = have * 60 / rate;
            }
        }
        break;
    }
    if (d)
        closedir(d);
}

static void probe_route(struct snap *s)
{
    char line[256], ifname[16], dest[16];
    FILE *f = fopen("/proc/net/route", "r");

    s->route[0] = 0;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "%15s %15s", ifname, dest) == 2 && !strcmp(dest, "00000000")) {
            copy(s->route, sizeof s->route, ifname);
            break;
        }
    if (f)
        fclose(f);
}

static void probe_knobs(struct snap *s)
{
    char out[256], *p;

    s->vol = s->light = -1;
    s->muted = 0;
    if (!run(out, sizeof out, "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@") &&
        (p = strstr(out, "Volume: "))) {
        s->vol = atof(p + 8) * 100 + 0.5;
        s->muted = strstr(out, "MUTED") != NULL;
    }
    /* device,class,current,percent%,max */
    if (!run(out, sizeof out, "brightnessctl", "-m", "-c", "backlight") && (p = strchr(out, '%'))) {
        while (p > out && p[-1] != ',')
            p--;
        s->light = atoi(p);
    }
}

/* --- wifi, straight off iwd's bus api ---------------------------------- */

static sd_bus *bus;

static void drop_bus(void)
{
    if (bus)
        sd_bus_flush_close_unref(bus);
    bus = NULL;
}

static void probe_wifi_list(struct snap *s)
{
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    struct net found[48];
    int nfound = 0;
    const char *path, *iface, *prop, *contents, *str;
    char type;

    s->wifi = s->scanning = s->nnets = 0;
    s->station[0] = s->ssid[0] = 0;
    if (!bus && sd_bus_open_system(&bus) < 0) {
        bus = NULL;
        return;
    }
    if (sd_bus_call_method(bus, IWD, "/", "org.freedesktop.DBus.ObjectManager",
                           "GetManagedObjects", &err, &reply, "") < 0) {
        sd_bus_error_free(&err);
        drop_bus();
        return;
    }

    sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    while (sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
        struct net n = {0};
        int is_net = 0;

        sd_bus_message_read(reply, "o", &path);
        copy(n.path, sizeof n.path, path);
        sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
        while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
            int station, network;
            sd_bus_message_read(reply, "s", &iface);
            station = !strcmp(iface, IWD ".Station");
            network = !strcmp(iface, IWD ".Network");
            if (station)
                copy(s->station, sizeof s->station, path);
            if (network)
                is_net = 1;
            sd_bus_message_enter_container(reply, 'a', "{sv}");
            while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                sd_bus_message_read(reply, "s", &prop);
                sd_bus_message_peek_type(reply, &type, &contents);
                if ((station || network) && contents && strchr("sob", contents[0]) && !contents[1]) {
                    int flag = 0;
                    str = "";
                    sd_bus_message_enter_container(reply, 'v', contents);
                    if (contents[0] == 'b')
                        sd_bus_message_read_basic(reply, 'b', &flag);
                    else
                        sd_bus_message_read_basic(reply, contents[0], &str);
                    sd_bus_message_exit_container(reply);
                    if (station && !strcmp(prop, "Scanning")) s->scanning = flag;
                    else if (network && !strcmp(prop, "Name")) copy(n.name, sizeof n.name, str);
                    else if (network && !strcmp(prop, "Type")) copy(n.type, sizeof n.type, str);
                    else if (network && !strcmp(prop, "Connected")) n.connected = flag;
                    else if (network && !strcmp(prop, "KnownNetwork")) copy(n.known, sizeof n.known, str);
                } else {
                    sd_bus_message_skip(reply, "v");
                }
                sd_bus_message_exit_container(reply);
            }
            sd_bus_message_exit_container(reply);
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);
        sd_bus_message_exit_container(reply);
        if (is_net && nfound < 48)
            found[nfound++] = n;
    }
    sd_bus_message_unref(reply);
    reply = NULL;
    if (!s->station[0])
        return;
    s->wifi = 1;

    /* strongest first, which is the order iwd hands them out in */
    if (sd_bus_call_method(bus, IWD, s->station, IWD ".Station", "GetOrderedNetworks",
                           &err, &reply, "") < 0) {
        sd_bus_error_free(&err);
        return;
    }
    sd_bus_message_enter_container(reply, 'a', "(on)");
    for (;;) {
        int16_t signal;
        if (sd_bus_message_read(reply, "(on)", &path, &signal) <= 0)
            break;
        for (int i = 0; i < nfound && s->nnets < 48; i++)
            if (!strcmp(found[i].path, path)) {
                found[i].signal = signal / 100;
                if (found[i].connected)
                    copy(s->ssid, sizeof s->ssid, found[i].name);
                s->nets[s->nnets++] = found[i];
            }
    }
    sd_bus_message_unref(reply);
}

struct wifi_job { char verb[16], path[200]; };

static void *wifi_worker(void *arg)
{
    struct wifi_job *job = arg;
    sd_bus *b = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    const char *iface = IWD ".Network", *method = "Connect";
    char msg[160] = "";
    int r;

    if (!strcmp(job->verb, "forget")) { iface = IWD ".KnownNetwork"; method = "Forget"; }
    else if (!strcmp(job->verb, "disconnect")) { iface = IWD ".Station"; method = "Disconnect"; }
    else if (!strcmp(job->verb, "scan")) { iface = IWD ".Station"; method = "Scan"; }

    r = sd_bus_open_system(&b);
    if (r >= 0)
        r = sd_bus_call_method(b, IWD, job->path, iface, method, &err, NULL, "");
    if (strcmp(job->verb, "scan")) {
        if (r >= 0)
            snprintf(msg, sizeof msg, "%s", !strcmp(job->verb, "connect") ? "connected" :
                     !strcmp(job->verb, "forget") ? "forgotten" : "disconnected");
        else if (!strcmp(job->verb, "connect"))
            snprintf(msg, sizeof msg, "didn't connect. wrong password, or it's out of reach");
        else
            snprintf(msg, sizeof msg, "that didn't work");
        pthread_mutex_lock(&lock);
        copy(wifi_msg, sizeof wifi_msg, msg);
        wifi_done = r >= 0 ? 1 : -1;
        pthread_mutex_unlock(&lock);
    }
    sd_bus_error_free(&err);
    if (b)
        sd_bus_flush_close_unref(b);
    free(job);
    probe_poke();
    return NULL;
}

/* connect, forget, disconnect or scan. the answer shows up in probe_wifi_result */
void probe_wifi(const char *verb, const char *path)
{
    struct wifi_job *job = calloc(1, sizeof *job);
    pthread_t th;

    if (demo() || !path[0]) {
        free(job);
        return;
    }
    copy(job->verb, sizeof job->verb, verb);
    copy(job->path, sizeof job->path, path);
    if (pthread_create(&th, NULL, wifi_worker, job))
        free(job);
    else
        pthread_detach(th);
}

int probe_wifi_result(char *msg, size_t n)
{
    int r;
    pthread_mutex_lock(&lock);
    r = wifi_done;
    wifi_done = 0;
    copy(msg, n, wifi_msg);
    pthread_mutex_unlock(&lock);
    return r;
}

/* --- bluetooth, straight off bluez ---------------------------------------- */

#define BLUEZ "org.bluez"

static void probe_bt_list(struct snap *s)
{
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    const char *path, *iface, *prop, *contents, *str;
    char type;

    s->bt = s->bt_on = s->bt_scanning = s->nbt = 0;
    s->adapter[0] = 0;
    if (!bus && sd_bus_open_system(&bus) < 0) {
        bus = NULL;
        return;
    }
    if (sd_bus_call_method(bus, BLUEZ, "/", "org.freedesktop.DBus.ObjectManager",
                           "GetManagedObjects", &err, &reply, "") < 0) {
        sd_bus_error_free(&err);
        return;
    }
    sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    while (sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
        struct bt b = {0};
        int is_dev = 0;

        sd_bus_message_read(reply, "o", &path);
        copy(b.path, sizeof b.path, path);
        sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
        while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
            int adapter, device;
            sd_bus_message_read(reply, "s", &iface);
            adapter = !strcmp(iface, BLUEZ ".Adapter1");
            device = !strcmp(iface, BLUEZ ".Device1");
            if (adapter && !s->adapter[0]) {
                copy(s->adapter, sizeof s->adapter, path);
                s->bt = 1;
            }
            if (device)
                is_dev = 1;
            sd_bus_message_enter_container(reply, 'a', "{sv}");
            while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                sd_bus_message_read(reply, "s", &prop);
                sd_bus_message_peek_type(reply, &type, &contents);
                if ((adapter || device) && contents && strchr("sbn", contents[0]) && !contents[1]) {
                    int flag = 0;
                    int16_t num = 0;
                    str = "";
                    sd_bus_message_enter_container(reply, 'v', contents);
                    if (contents[0] == 'b') sd_bus_message_read_basic(reply, 'b', &flag);
                    else if (contents[0] == 'n') sd_bus_message_read_basic(reply, 'n', &num);
                    else sd_bus_message_read_basic(reply, 's', &str);
                    sd_bus_message_exit_container(reply);
                    if (adapter && !strcmp(prop, "Powered")) s->bt_on = flag;
                    else if (adapter && !strcmp(prop, "Discovering")) s->bt_scanning = flag;
                    else if (device && !strcmp(prop, "Alias")) copy(b.name, sizeof b.name, str);
                    else if (device && !strcmp(prop, "Paired")) b.paired = flag;
                    else if (device && !strcmp(prop, "Connected")) b.connected = flag;
                    else if (device && !strcmp(prop, "RSSI")) b.rssi = num;
                } else {
                    sd_bus_message_skip(reply, "v");
                }
                sd_bus_message_exit_container(reply);
            }
            sd_bus_message_exit_container(reply);
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);
        sd_bus_message_exit_container(reply);
        /* things that only shout an address are noise */
        if (is_dev && s->nbt < 32 && (b.paired || (b.name[0] && b.name[2] != '-' && b.name[2] != ':')))
            s->bts[s->nbt++] = b;
    }
    sd_bus_message_unref(reply);

    /* connected first, then paired, then by signal */
    for (int i = 1; i < s->nbt; i++)
        for (int j = i; j > 0; j--) {
            struct bt *a = &s->bts[j - 1], *b = &s->bts[j];
            int ka = a->connected * 4 + a->paired * 2, kb = b->connected * 4 + b->paired * 2;
            if (ka > kb || (ka == kb && (a->rssi ? a->rssi : -200) >= (b->rssi ? b->rssi : -200)))
                break;
            struct bt t = *a; *a = *b; *b = t;
        }

    /* discovery belongs to this connection, so it has to be started from here */
    if (s->bt && s->bt_on && bt_scan_wanted != s->bt_scanning) {
        sd_bus_call_method(bus, BLUEZ, s->adapter, BLUEZ ".Adapter1",
                           bt_scan_wanted ? "StartDiscovery" : "StopDiscovery", &err, NULL, "");
        sd_bus_error_free(&err);
    }
}

struct bt_job { char verb[16], path[160], adapter[160]; };

static void *bt_worker(void *arg)
{
    struct bt_job *job = arg;
    sd_bus *b = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char msg[160] = "";
    int r = sd_bus_open_system(&b);

    if (r < 0) {
        /* nothing to do */
    } else if (!strcmp(job->verb, "connect")) {
        /* pairing something that is already paired fails, which is fine */
        sd_bus_call_method(b, BLUEZ, job->path, BLUEZ ".Device1", "Pair", &err, NULL, "");
        sd_bus_error_free(&err);
        sd_bus_call_method(b, BLUEZ, job->path, "org.freedesktop.DBus.Properties", "Set", &err, NULL,
                           "ssv", BLUEZ ".Device1", "Trusted", "b", 1);
        sd_bus_error_free(&err);
        r = sd_bus_call_method(b, BLUEZ, job->path, BLUEZ ".Device1", "Connect", &err, NULL, "");
        snprintf(msg, sizeof msg, "%s", r >= 0 ? "connected" :
                 "didn't connect. is it in pairing mode? keyboards may need bluetoothctl");
    } else if (!strcmp(job->verb, "disconnect")) {
        r = sd_bus_call_method(b, BLUEZ, job->path, BLUEZ ".Device1", "Disconnect", &err, NULL, "");
        snprintf(msg, sizeof msg, "%s", r >= 0 ? "disconnected" : "that didn't work");
    } else if (!strcmp(job->verb, "forget")) {
        r = sd_bus_call_method(b, BLUEZ, job->adapter, BLUEZ ".Adapter1", "RemoveDevice", &err, NULL,
                               "o", job->path);
        snprintf(msg, sizeof msg, "%s", r >= 0 ? "forgotten" : "that didn't work");
    } else if (!strcmp(job->verb, "on") || !strcmp(job->verb, "off")) {
        run(NULL, 0, "rfkill", "unblock", "bluetooth");
        r = sd_bus_call_method(b, BLUEZ, job->adapter, "org.freedesktop.DBus.Properties", "Set", &err,
                               NULL, "ssv", BLUEZ ".Adapter1", "Powered", "b", !strcmp(job->verb, "on"));
        snprintf(msg, sizeof msg, "%s", r >= 0 ? (!strcmp(job->verb, "on") ? "bluetooth's on" : "bluetooth's off")
                                               : "couldn't switch it");
    }
    pthread_mutex_lock(&lock);
    copy(wifi_msg, sizeof wifi_msg, msg);
    wifi_done = r >= 0 ? 1 : -1;
    pthread_mutex_unlock(&lock);
    sd_bus_error_free(&err);
    if (b)
        sd_bus_flush_close_unref(b);
    free(job);
    probe_poke();
    return NULL;
}

/* connect (pairs first), disconnect, forget, on, off. answers come back
   through probe_wifi_result like the wifi ones */
void probe_bt(const char *verb, const char *path)
{
    struct bt_job *job = calloc(1, sizeof *job);
    pthread_t th;

    if (demo()) {
        free(job);
        return;
    }
    copy(job->verb, sizeof job->verb, verb);
    copy(job->path, sizeof job->path, path);
    pthread_mutex_lock(&lock);
    copy(job->adapter, sizeof job->adapter, shared.adapter);
    pthread_mutex_unlock(&lock);
    if (pthread_create(&th, NULL, bt_worker, job))
        free(job);
    else
        pthread_detach(th);
}

void probe_bt_scan(int on)
{
    pthread_mutex_lock(&lock);
    bt_scan_wanted = on;
    pthread_mutex_unlock(&lock);
    probe_poke();
}

/* --- sound outputs -------------------------------------------------------- */

/* the Sinks block of `wpctl status`:  " │  *   62. Speaker [vol: 0.45]" */
int probe_sinks(struct sink *out, int max)
{
    static char text[16384];
    char *p;
    int n = 0;

    if (demo()) {
        static const struct sink fake[] = {
            {"Built-in Audio Speaker", 62, 1}, {"HDMI / DisplayPort 1 Output", 61, 0},
            {"WH-1000XM4", 88, 0},
        };
        memcpy(out, fake, sizeof fake);
        return 3;
    }
    if (run(text, sizeof text, "wpctl", "status") || !(p = strstr(text, "Sinks:")))
        return 0;
    p += strcspn(p, "\n");
    while (*p == '\n' && n < max) {
        char *line = ++p, *dot, *num, *vol;
        size_t len = strcspn(p, "\n");
        p += len;
        dot = memchr(line, '.', len);
        if (!dot || dot == line || !isdigit((unsigned char)dot[-1]))
            break;
        num = dot;
        while (num > line && isdigit((unsigned char)num[-1]))
            num--;
        out[n].id = atoi(num);
        out[n].current = memchr(line, '*', num - line) != NULL;
        vol = strstr(dot, " [vol");
        snprintf(out[n].name, sizeof out[n].name, "%.*s",
                 (int)((vol && vol < line + len ? vol : line + len) - dot - 2), dot + 2);
        n++;
    }
    return n;
}

/* --- pretend data, for looking at the screens on another machine -------- */

static void fake(struct snap *s)
{
    const char *mode = getenv("LIFEBOAT_DEMO");
    static const struct net nets[] = {
        {"harbour", "/n/1", "/k/1", "psk", -48, 1}, {"MSCS-Staff", "/n/2", "", "8021x", -61, 0},
        {"cafe upstairs", "/n/3", "", "open", -70, 0}, {"DIRECT-printer", "/n/4", "", "psk", -83, 0},
    };
#define NVME "/dev/nvme0n1", "on its way out, 212 bad sectors moved, 96% worn, 47°C", 512110190592LL, 2
    static const struct disk disks[] = {
        {"/dev/nvme0n1p1", "vfat", "", "", "SAMSUNG MZVL2512", "nvme", 536870912LL, 0, 0, NVME},
        {"/dev/nvme0n1p5", "ext4", "arch", "", "SAMSUNG MZVL2512", "nvme", 141733920768LL, 0, 0, NVME},
        {"/dev/nvme0n1p6", "ntfs", "Windows", "", "SAMSUNG MZVL2512", "nvme", 214748364800LL, 0, 0, NVME},
        {"/dev/nvme0n1p7", "crypto_LUKS", "", "", "SAMSUNG MZVL2512", "nvme", 107374182400LL, 0, 0, NVME},
        {"/dev/sdb1", "exfat", "backup", "/mnt/backup", "WD Elements", "usb", 2000398934016LL, 1, 0,
         "/dev/sdb", "healthy, 31°C", 2000398934016LL, 1},
        {"/dev/sda2", "ext4", "LIFEBOAT", "", "SanDisk Ultra", "usb", 30765219840LL, 1, 1,
         "/dev/sda", "this one doesn't report its health", 30765219840LL, 0},
    };
    static const struct bt bts[] = {
        {"MX Master 3", "/b/1", 1, 1, -52}, {"WH-1000XM4", "/b/2", 1, 0, 0},
        {"Keychron K2", "/b/3", 0, 0, -61}, {"JBL Flip 5", "/b/4", 0, 0, -77},
    };

    memset(&s->st, 0, sizeof s->st);
    s->napps = 0;
    add_app(s, "terminal", "foot", "lifeboat", 0);
    add_app(s, "files", "nnn -de", "lifeboat", 1);
    add_app(s, "fix", "@fix", "lifeboat", 0);
    add_app(s, "settings", "@settings", "lifeboat", 0);
    add_app(s, "extras", "@extras", "lifeboat", 0);
    add_app(s, "packages", "@pkgs", "lifeboat", 0);
    add_app(s, "get a flatpak", "@install", "lifeboat", 0);
    add_app(s, "bluetooth", "@bt", "lifeboat", 0);
    add_app(s, "sound output", "@audio", "lifeboat", 0);
    add_app(s, "partitions", "doas gparted", "lifeboat", 0);
    add_app(s, "processes", "htop", "lifeboat", 1);
    add_app(s, "Firefox", "flatpak run org.mozilla.firefox", "flatpak", 0);
    add_app(s, "mpv Media Player", "mpv --player-operation-mode=pseudo-gui", "system", 0);
    add_app(s, "Obsidian", "/home/lifeboat/Applications/Obsidian.AppImage", "appimage", 0);
    add_app(s, "Zathura", "zathura", "system", 0);
    memcpy(s->nets, nets, sizeof nets);
    s->nnets = 4;
    memcpy(s->disks, disks, sizeof disks);
    s->ndisks = 6;
    memcpy(s->bts, bts, sizeof bts);
    s->nbt = 4;
    s->bt = s->bt_on = 1;
    s->bat_mins = 208;
    s->st.stick_size = 30765219840LL;
    s->st.stick_free = 21474836480LL;
    s->wifi = 1;
    copy(s->ssid, sizeof s->ssid, "harbour");
    copy(s->route, sizeof s->route, "wlan0");
    s->mem_total = 30L << 20;
    s->mem_avail = 19L << 20;
    s->bat = 71;
    s->vol = 45;
    s->light = 80;
    s->stick = 1;
    s->changes = 1288490188LL;
    strcpy(s->st.where, "ram");
    strcpy(s->st.sys, "ram");
    strcpy(s->st.hw, "this");
    s->st.hw_kept = 48LL << 20;
    if (!strcmp(mode, "stick")) {
        strcpy(s->st.where, "stick");
    } else if (!strcmp(mode, "move")) {
        strcpy(s->st.moving, "stick");
        s->st.base = 120000;
        s->st.total = 131072;
        s->st.done = 120000 + (time(NULL) % 20) * 550;
        s->st.since = time(NULL) - 14;
    } else if (!strcmp(mode, "full")) {
        s->mem_avail = 5L << 20;
        s->stick = 0;
        s->ndisks = 5;
        s->st.stick_size = 0;
    }
}

/* --- the thread -------------------------------------------------------- */

static void *loop(void *arg)
{
    int n = 0;
    (void)arg;

    for (;;) {
        struct timespec until;
        int slow = n % 5 == 0;

        if (demo()) {
            fake(&work);
        } else {
            meminfo(&work.mem_total, &work.mem_avail);
            read_state(&work.st);
            work.changes = used_bytes(CHANGES);
            probe_battery(&work);
            probe_route(&work);
            pthread_mutex_lock(&lock);
            slow |= poked;
            poked = 0;
            pthread_mutex_unlock(&lock);
            pthread_mutex_lock(&lock);
            slow |= bt_scan_wanted;
            pthread_mutex_unlock(&lock);
            if (slow) {
                probe_disks(&work);
                probe_wifi_list(&work);
                probe_bt_list(&work);
                probe_knobs(&work);
            }
            if (n % 10 == 0)
                probe_apps(&work);
        }
        n++;

        pthread_mutex_lock(&lock);
        shared = work;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 1;
        if (!poked)
            pthread_cond_timedwait(&wake, &lock, &until);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

void probe_start(void)
{
    pthread_t th;
    if (demo())
        fake(&shared);
    if (!pthread_create(&th, NULL, loop, NULL))
        pthread_detach(th);
}

void probe_get(struct snap *out)
{
    pthread_mutex_lock(&lock);
    *out = shared;
    pthread_mutex_unlock(&lock);
}

/* something just changed, look again now instead of in a few seconds */
void probe_poke(void)
{
    pthread_mutex_lock(&lock);
    poked = 1;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}
