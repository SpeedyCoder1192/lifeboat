/* Deciding what lives in ram.
 *
 * Three things compete for it:
 *   the system image   system.sfs, ~550M. On the dm device lifeboat-system,
 *                      which maps either a ram copy or the file on the stick,
 *                      and can be switched either way while mounted.
 *   the drivers        hw.sfs, firmware and kernel modules for every machine,
 *                      ~650M. Each machine uses a few dozen megabytes of it.
 *   the changes        the session itself, on lifeboat-changes (store.c).
 *
 * At boot init puts in ram what fits. After that the watcher keeps:
 *   - a copy of just this machine's drivers in ram, so the stick can go;
 *   - when ram gets tight, frees it in the cheapest order: drivers this
 *     machine doesn't use, then the system image (back onto the stick,
 *     instantly, nothing written), and only then the changes (store.c);
 *   - when ram is plentiful again, pulls the system image back in.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "boat.h"

#define SYSDM    "lifeboat-system"
#define IMAGE    RUN_DIR "/image"
#define BOOT     RUN_DIR "/boot"
#define HW       RUN_DIR "/hw"
#define HW2      RUN_DIR "/hw-stick"
#define HWRAM    RUN_DIR "/hwram"
#define SYSMETA  RUN_DIR "/sysmeta.img"

/* modules worth having for things that get plugged in later */
static const char *KIT[] = {"usb-storage", "uas", "usbhid", "hid-generic", "hid-multitouch", "hid-apple",
    "hid-logitech-dj", "snd-usb-audio", "uvcvideo", "btusb", "cdc_ether", "cdc_ncm", "rndis_host",
    "r8152", "ax88179_178a", "asix", "ipheth", "cdc_acm", "ftdi_sio", "pl2303", "ch341", "mmc_block",
    "sdhci-pci", "rtsx_pci_sdmmc", "exfat", "ntfs3", "vfat", "isofs", "udf", "fuse", "dm-crypt", "xts",
    "joydev", "xpad", "nls_utf8", "nls_cp437", "nls_iso8859-1", NULL};

static int is_mounted(const char *path)
{
    struct stat a, b;
    char parent[256];
    snprintf(parent, sizeof parent, "%s/..", path);
    return !stat(path, &a) && !stat(parent, &b) && a.st_dev != b.st_dev;
}

static long long file_size(const char *path)
{
    struct stat sb;
    return stat(path, &sb) ? -1 : sb.st_size;
}

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

/* the file behind a loop device, given "7:3" or "/dev/loop3" */
static void loop_file(const char *dev, char *out, size_t n)
{
    char path[96];
    int num = -1;
    if (!strncmp(dev, "/dev/loop", 9))
        num = atoi(dev + 9);
    else if (!strncmp(dev, "7:", 2))
        num = atoi(dev + 2);
    out[0] = 0;
    if (num < 0)
        return;
    snprintf(path, sizeof path, "/sys/block/loop%d/loop/backing_file", num);
    read_line(path, out, n);
}

/* where the system image is read from: ram, stick, moving or none */
static const char *sys_where(char *loop, size_t n, long long *len)
{
    char out[256], type[16] = "", dev[32] = "", file[256];
    *len = 0;
    if (loop)
        loop[0] = 0;
    if (run(out, sizeof out, "dmsetup", "table", SYSDM) ||
        sscanf(out, "%*d %lld %15s %31s", len, type, dev) < 2)
        return "none";
    if (!strcmp(type, "clone"))
        return "moving";
    loop_file(dev, file, sizeof file);
    if (loop && !strncmp(dev, "7:", 2))
        snprintf(loop, n, "/dev/loop%d", atoi(dev + 2));
    return !strncmp(file, IMAGE, strlen(IMAGE)) || !strncmp(file, "/system.sfs", 11) ? "ram" : "stick";
}

static int boot_dev(char *dev, size_t n)
{
    struct stat sb;
    return !read_line(RUN_DIR "/bootdev", dev, n) && dev[0] == '/' && !stat(dev, &sb) && S_ISBLK(sb.st_mode);
}

static int mount_boot(void)
{
    char dev[128];
    if (is_mounted(BOOT))
        return 0;
    if (!boot_dev(dev, sizeof dev))
        return -1;
    mkdir(BOOT, 0755);
    return run(NULL, 0, "mount", "-o", "ro", dev, BOOT) ? -1 : 0;
}

/* --- the system image ---------------------------------------------------- */

int sys_to_stick(struct state *st)
{
    char ramloop[64], sloop[64], out[256], tbl[256], file[256];
    long long len;

    if (strcmp(sys_where(ramloop, sizeof ramloop, &len), "ram") || !ramloop[0])
        return -1;
    if (mount_boot())
        return -1;
    if (run(sloop, sizeof sloop, "losetup", "-r", "--find", "--show", BOOT "/lifeboat/system.sfs"))
        return -1;
    if (file_size(BOOT "/lifeboat/system.sfs") != len * 512) {
        run(NULL, 0, "losetup", "-d", sloop);
        return -1;      /* a different lifeboat on that stick */
    }
    snprintf(tbl, sizeof tbl, "0 %lld linear %s 0", len, sloop);
    if (run(out, sizeof out, "dmsetup", "load", SYSDM, "--table", tbl) ||
        run(out, sizeof out, "dmsetup", "resume", SYSDM)) {
        run(NULL, 0, "dmsetup", "clear", SYSDM);
        run(NULL, 0, "losetup", "-d", sloop);
        return -1;
    }
    loop_file(ramloop, file, sizeof file);
    run(NULL, 0, "losetup", "-d", ramloop);
    if (!strncmp(file, IMAGE, strlen(IMAGE)))
        unlink(file);
    snprintf(st->sys, sizeof st->sys, "stick");
    return 0;
}

static int clone_line(long *done, long *total, long *busy)
{
    char out[512], *tok[32];
    int n = 0;
    if (run(out, sizeof out, "dmsetup", "status", SYSDM))
        return -1;
    for (char *p = strtok(out, " "); p && n < 32; p = strtok(NULL, " "))
        tok[n++] = p;
    if (n < 9 || strcmp(tok[2], "clone") || sscanf(tok[6], "%ld/%ld", done, total) != 2)
        return -1;
    *busy = atol(tok[7]);
    return strstr(tok[n - 1], "ail") ? -2 : 0;
}

/* copy the image off the stick into ram, under the mounted system, then map
   the ram copy. Reads keep working the whole time. */
int sys_to_ram(struct state *st)
{
    char sloop[64], rloop[64], meta[64], out[256], tbl[320], size[32];
    long long len, room;
    long done, total, busy;
    int fd;

    if (strcmp(sys_where(sloop, sizeof sloop, &len), "stick") || !sloop[0])
        return -1;
    mkdir(IMAGE, 0755);
    room = len * 512 + (16LL << 20);
    if (is_mounted(IMAGE)) {
        snprintf(size, sizeof size, "remount,size=%lldk", (room + file_size(IMAGE "/hw.sfs") + 4095) / 1024);
        if (file_size(IMAGE "/hw.sfs") < 0)
            snprintf(size, sizeof size, "remount,size=%lldk", room / 1024);
        run(NULL, 0, "mount", "-o", size, IMAGE);
    } else {
        snprintf(size, sizeof size, "size=%lldk,mode=0755", room / 1024);
        if (run(out, sizeof out, "mount", "-t", "tmpfs", "-o", size, "image", IMAGE))
            return -1;
    }
    fd = open(IMAGE "/system.sfs", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0 || ftruncate(fd, len * 512)) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    close(fd);
    fd = open(SYSMETA, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0 || ftruncate(fd, 4 << 20)) {
        unlink(IMAGE "/system.sfs");
        return -1;
    }
    close(fd);
    if (run(rloop, sizeof rloop, "losetup", "--find", "--show", IMAGE "/system.sfs"))
        goto undo_file;
    if (run(meta, sizeof meta, "losetup", "--find", "--show", SYSMETA))
        goto undo_rloop;

    /* dm-clone wants the length to be whole regions: the biggest power of two
       up to 64K that divides it (the build pads the image to 64K anyway) */
    int region = 128;
    while (region > 8 && len % region)
        region /= 2;
    snprintf(tbl, sizeof tbl, "0 %lld clone %s %s %s %d 1 no_hydration", len, meta, rloop, sloop, region);
    if (run(out, sizeof out, "dmsetup", "load", SYSDM, "--table", tbl) ||
        run(out, sizeof out, "dmsetup", "resume", SYSDM)) {
        fprintf(stderr, "lifeboat: system back into ram failed: %s\n", out);
        run(NULL, 0, "dmsetup", "clear", SYSDM);
        goto undo_meta;
    }
    snprintf(st->sys, sizeof st->sys, "moving");
    write_state(st);
    run(NULL, 0, "dmsetup", "message", SYSDM, "0", "enable_hydration");
    for (;;) {
        int r = clone_line(&done, &total, &busy);
        if (r == -2 || r == -1) {
            fprintf(stderr, "lifeboat: copying the system into ram broke, staying on the stick\n");
            /* back to the stick, which still has every byte */
            snprintf(tbl, sizeof tbl, "0 %lld linear %s 0", len, sloop);
            run(NULL, 0, "dmsetup", "load", SYSDM, "--table", tbl);
            run(NULL, 0, "dmsetup", "resume", SYSDM);
            snprintf(st->sys, sizeof st->sys, "stick");
            goto undo_meta;
        }
        if (done >= total && !busy)
            break;
        if (!access(RUN_DIR "/cancel", F_OK)) {
            snprintf(tbl, sizeof tbl, "0 %lld linear %s 0", len, sloop);
            run(NULL, 0, "dmsetup", "load", SYSDM, "--table", tbl);
            run(NULL, 0, "dmsetup", "resume", SYSDM);
            unlink(RUN_DIR "/cancel");
            snprintf(st->sys, sizeof st->sys, "stick");
            /* and don't try again for a while */
            close(creat(RUN_DIR "/sys-on-stick-by-choice", 0644));
            note_text(st, "cancelled. the system stays on the stick");
            goto undo_meta;
        }
        usleep(300000);
    }
    snprintf(tbl, sizeof tbl, "0 %lld linear %s 0", len, rloop);
    if (run(out, sizeof out, "dmsetup", "load", SYSDM, "--table", tbl) ||
        run(out, sizeof out, "dmsetup", "resume", SYSDM))
        return -1;
    run(NULL, 0, "losetup", "-d", meta);
    unlink(SYSMETA);
    run(NULL, 0, "losetup", "-d", sloop);
    snprintf(st->sys, sizeof st->sys, "ram");
    return 0;

undo_meta:
    run(NULL, 0, "losetup", "-d", meta);
undo_rloop:
    run(NULL, 0, "losetup", "-d", rloop);
undo_file:
    unlink(SYSMETA);
    unlink(IMAGE "/system.sfs");
    return -1;
}

/* --- the drivers ----------------------------------------------------------- */

/* where the drivers come from right now: "all" (hw.sfs in ram), "stick" (hw.sfs
   on the stick), "this" (only this machine's, in ram) */
static void hw_where(char *out, size_t n)
{
    char src[256] = "", file[256];
    FILE *f = fopen("/proc/self/mountinfo", "r");
    char line[1024];

    snprintf(out, n, "none");
    /* the mount at /usr/lib/firmware tells */
    while (f && fgets(line, sizeof line, f)) {
        char mp[256], fs[64], dev[128];
        char *dash = strstr(line, " - ");
        if (!dash || sscanf(line, "%*d %*d %*s %*s %255s", mp) != 1 || strcmp(mp, "/usr/lib/firmware"))
            continue;
        if (sscanf(dash + 3, "%63s %127s", fs, dev) != 2)
            continue;
        if (!strcmp(fs, "overlay") || !strcmp(fs, "tmpfs"))
            snprintf(src, sizeof src, "this");
        else
            snprintf(src, sizeof src, "%s", dev);
    }
    if (f)
        fclose(f);
    if (!strcmp(src, "this")) {
        snprintf(out, n, "this");
        return;
    }
    if (!strncmp(src, "/dev/loop", 9)) {
        loop_file(src, file, sizeof file);
        snprintf(out, n, "%s", !strncmp(file, BOOT, strlen(BOOT)) ? "stick" : "all");
    }
}

static long long du(const char *path)
{
    char out[64];
    if (run(out, sizeof out, "du", "-sb", path))
        return 0;
    return atoll(out);
}

/* firmware this machine has asked for. init turns on the firmware loader's
   debug lines, which name every file it loads */
static int loaded_firmware(char names[][128], int max)
{
    static char log[1 << 20];
    int n = 0;
    if (run(log, sizeof log, "dmesg"))
        return 0;
    for (char *p = log; (p = strstr(p, "direct-loading ")) && n < max; ) {
        p += 15;
        size_t len = strcspn(p, " \n");
        if (len && len < 127) {
            int dup = 0;
            for (int i = 0; i < n; i++)
                dup |= !strncmp(names[i], p, len) && !names[i][len];
            if (!dup) {
                memcpy(names[n], p, len);
                names[n++][len] = 0;
            }
        }
        p += len;
    }
    return n;
}

/* copy one file (resolved through links) from src/rel into dst/rel */
static void take(const char *src, const char *dst, const char *rel)
{
    char from[512], to[512], dir[512];
    snprintf(from, sizeof from, "%s/%s", src, rel);
    snprintf(to, sizeof to, "%s/%s", dst, rel);
    snprintf(dir, sizeof dir, "%s", to);
    *strrchr(dir, '/') = 0;
    run(NULL, 0, "mkdir", "-p", dir);
    run(NULL, 0, "cp", "-L", from, to);
}

/* put a fresh mount at /usr/lib/<what> without a moment where it's missing
   for more than a blink: mount next to it, drop the old one, move it in */
static int swap_in(const char *what, const char *lower1, const char *lower2)
{
    char tmp[128], target[128], opts[640];
    snprintf(tmp, sizeof tmp, RUN_DIR "/swap-%s", what);
    snprintf(target, sizeof target, "/usr/lib/%s", what);
    mkdir(tmp, 0755);
    if (lower2) {
        snprintf(opts, sizeof opts, "lowerdir=%s:%s", lower1, lower2);
        if (mount("overlay", tmp, "overlay", MS_RDONLY, opts))
            return -1;
    } else if (mount(lower1, tmp, NULL, MS_BIND, NULL)) {
        return -1;
    }
    umount2(target, MNT_DETACH);
    if (mount(tmp, target, NULL, MS_MOVE, NULL)) {
        /* put something back rather than nothing */
        mount(lower1, target, NULL, MS_BIND, NULL);
        umount2(tmp, MNT_DETACH);
        return -1;
    }
    return 0;
}

/* the stick's hw.sfs at HW2 */
static int mount_stick_hw(void)
{
    char loop[64];
    if (is_mounted(HW2))
        return 0;
    if (mount_boot() || run(loop, sizeof loop, "losetup", "-r", "--find", "--show", BOOT "/lifeboat/hw.sfs"))
        return -1;
    mkdir(HW2, 0755);
    if (run(NULL, 0, "mount", "-t", "squashfs", "-o", "ro", loop, HW2)) {
        run(NULL, 0, "losetup", "-d", loop);
        return -1;
    }
    return 0;
}

/* every driver there is back under /usr/lib, after a trim */
int hw_full(struct state *st)
{
    char where[16];
    const char *full;

    hw_where(where, sizeof where);
    if (strcmp(where, "this"))
        return 0;
    if (is_mounted(HW))
        full = HW;
    else if (!mount_stick_hw())
        full = HW2;
    else
        return -1;
    for (int k = 0; k < 2; k++) {
        const char *what = k ? "modules" : "firmware";
        char dir[128];
        snprintf(dir, sizeof dir, "%s/%s", full, what);
        if (swap_in(what, dir, NULL))
            return -1;
    }
    hw_where(st->hw, sizeof st->hw);
    return 0;
}

/* Keep only this machine's drivers in ram: the modules it has loaded plus a
   kit for things that get plugged in, and the firmware it has loaded. While
   the stick is there, anything else still falls through to its copy. */
int hw_trim(struct state *st)
{
    static char names[400][128];
    char where[16], kver[64], from[256], src_root[64], line[512], dst[64];
    const char *fallthrough = NULL;
    struct utsname u;
    int n, gen;
    FILE *f;
    DIR *d;
    struct dirent *e;

    hw_where(where, sizeof where);
    if (!strcmp(where, "none"))
        return -1;
    uname(&u);
    snprintf(kver, sizeof kver, "%s", u.release);

    /* the full set to copy from: whichever hw.sfs is mounted */
    if (is_mounted(HW))
        snprintf(src_root, sizeof src_root, HW);
    else if (is_mounted(HW2))
        snprintf(src_root, sizeof src_root, HW2);
    else
        return -1;

    /* a new directory each time, the old one goes after the swap */
    gen = (int)(time(NULL) % 100000);
    mkdir(HWRAM, 0755);
    if (!is_mounted(HWRAM) && run(NULL, 0, "mount", "-t", "tmpfs", "-o", "mode=0755", "hwram", HWRAM))
        return -1;
    snprintf(dst, sizeof dst, HWRAM "/%d", gen);
    mkdir(dst, 0755);

    /* modules: every index file, everything loaded, the kit */
    snprintf(from, sizeof from, "%s/modules/%s", src_root, kver);
    if ((d = opendir(from))) {
        while ((e = readdir(d)))
            if (!strncmp(e->d_name, "modules.", 8)) {
                snprintf(line, sizeof line, "modules/%s/%s", kver, e->d_name);
                take(src_root, dst, line);
            }
        closedir(d);
    }
    f = fopen("/proc/modules", "r");
    n = 0;
    while (f && fgets(line, sizeof line, f) && n < 400)
        if (sscanf(line, "%127s", names[n]) == 1)
            n++;
    if (f)
        fclose(f);
    for (int i = 0; KIT[i] && n < 400; i++)
        snprintf(names[n++], sizeof names[0], "%s", KIT[i]);
    for (int i = 0; i < n; i++) {
        char deps[4096];
        if (run(deps, sizeof deps, "modprobe", "--show-depends", names[i]))
            continue;
        for (char *p = strstr(deps, "insmod "); p; p = strstr(p, "insmod ")) {
            char path[512], *mod;
            p += 7;
            sscanf(p, "%511s", path);
            mod = strstr(path, "/modules/");
            if (mod)
                take(src_root, dst, mod + 1);
        }
    }

    /* firmware: what was loaded, and the wifi regulatory database */
    n = loaded_firmware(names, 400);
    snprintf(names[n++], sizeof names[0], "regulatory.db");
    snprintf(names[n++], sizeof names[0], "regulatory.db.p7s");
    for (int i = 0; i < n; i++) {
        static const char *ext[] = {"", ".zst", ".xz", NULL};
        for (int j = 0; ext[j]; j++) {
            char rel[200];
            snprintf(rel, sizeof rel, "firmware/%s%s", names[i], ext[j]);
            snprintf(from, sizeof from, "%s/%s", src_root, rel);
            if (!access(from, F_OK))
                take(src_root, dst, rel);
        }
    }
    snprintf(line, sizeof line, "%s/firmware", dst);
    mkdir(line, 0755);

    /* while the stick is in, everything else still comes off it */
    if (!strcmp(where, "stick") || !strcmp(where, "this")) {
        if (is_mounted(HW) && strcmp(where, "all"))
            fallthrough = HW;
        else if (is_mounted(HW2))
            fallthrough = HW2;
    }
    /* the ram copy is about to go: fall through to the stick's, if it has one */
    if (!fallthrough && !strcmp(where, "all") && !mount_stick_hw())
        fallthrough = HW2;

    for (int k = 0; k < 2; k++) {
        const char *what = k ? "modules" : "firmware";
        char l1[128], l2[128];
        snprintf(l1, sizeof l1, "%s/%s", dst, what);
        snprintf(l2, sizeof l2, "%s/%s", fallthrough ? fallthrough : "", what);
        if (swap_in(what, l1, fallthrough ? l2 : NULL))
            return -1;
    }

    /* old copies, and the full set in ram if that's where it was */
    if ((d = opendir(HWRAM))) {
        while ((e = readdir(d)))
            if (e->d_name[0] != '.' && atoi(e->d_name) != gen) {
                snprintf(line, sizeof line, HWRAM "/%s", e->d_name);
                run(NULL, 0, "rm", "-rf", line);
            }
        closedir(d);
    }
    if (!strcmp(where, "all") && is_mounted(HW)) {
        char dev[256] = "", file[256] = "";
        FILE *mi = fopen("/proc/self/mounts", "r");
        while (mi && fgets(line, sizeof line, mi)) {
            char a[256], b[256];
            if (sscanf(line, "%255s %255s", a, b) == 2 && !strcmp(b, HW))
                snprintf(dev, sizeof dev, "%s", a);
        }
        if (mi)
            fclose(mi);
        umount2(HW, MNT_DETACH);
        loop_file(dev, file, sizeof file);
        if (dev[0])
            run(NULL, 0, "losetup", "-d", dev);
        if (!strncmp(file, IMAGE, strlen(IMAGE)))
            unlink(file);
    }
    st->hw_kept = du(dst);
    snprintf(st->hw, sizeof st->hw, "this");
    return 0;
}

/* --- the plan ---------------------------------------------------------------- */

static int recent(const char *path, long secs)
{
    struct stat sb;
    return !stat(path, &sb) && time(NULL) - sb.st_mtime < secs;
}

void ram_refresh(struct state *st)
{
    long long len;
    char loop[64];
    snprintf(st->sys, sizeof st->sys, "%s", sys_where(loop, sizeof loop, &len));
    st->sys_size = len * 512;
    hw_where(st->hw, sizeof st->hw);
}

/* one step of keeping ram in order, called every few seconds by the watcher.
   Returns 1 when it did something, so the caller looks again before spilling */
int ram_step(struct state *st, int pct, int stick_in, long avail_kb, long booted)
{
    static time_t trimmed, sys_moved;
    struct conf c;
    time_t now = time(NULL);

    load_conf(&c);
    ram_refresh(st);

    /* this machine's drivers into ram soon after boot, and once more later
       for whatever wifi or bluetooth loaded late */
    if (!strcmp(st->hw, "stick") && !trimmed && now - booted > 45) {
        trimmed = now;
        if (!hw_trim(st))
            return 1;
    }
    if (trimmed && trimmed < booted + 200 && now - booted > 240 && !strcmp(st->hw, "this")) {
        trimmed = now;
        hw_trim(st);
        return 1;
    }

    if (pct >= c.spill) {
        /* cheapest first: drivers nobody uses, then the image (the stick has it) */
        if (!strcmp(st->hw, "all")) {
            trimmed = now;
            if (!hw_trim(st)) {
                note_text(st, "ram got tight, so only this machine's drivers stay in it");
                return 1;
            }
        }
        if (!strcmp(st->sys, "ram") && stick_in && now - sys_moved > 60) {
            sys_moved = now;
            if (!sys_to_stick(st)) {
                note_text(st, "ram got tight, so the system runs off the stick for now. keep it in");
                return 1;
            }
        }
        return 0;
    }

    /* plenty again: the image back in, so the stick can come out */
    if (!strcmp(st->sys, "stick") && stick_in && pct < 50 && strcmp(c.sys_ram, "never") &&
        !recent(RUN_DIR "/sys-on-stick-by-choice", 1800) &&
        avail_kb * 1024LL > 2 * st->sys_size && now - sys_moved > 120 && now - booted > 60) {
        sys_moved = now;
        if (!sys_to_ram(st)) {
            note_text(st, "the system is back in ram");
            return 1;
        }
    }
    return 0;
}
