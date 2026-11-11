/* Where the changes live and how they move.
 *
 * The overlay's upper is an xfs on /dev/mapper/lifeboat-changes. That device
 * is a linear map onto zram (changes in ram) or onto a loop over changes.img
 * on the stick. Moving between the two reloads the same device as a dm-clone,
 * so nothing gets unmounted and nothing notices.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "boat.h"

#define REGION 128      /* sectors, so 64K */
#define MOVER  "lifeboat-move"          /* sits under lifeboat-changes while a move runs */
#define SAVEDM "lifeboat-save"          /* changes.img opened, when saves are encrypted */
#define KEY    RUN_DIR "/key"
#define CANCEL RUN_DIR "/cancel"        /* "lifeboat cancel" leaves this for the mover */
#define META2  RUN_DIR "/meta2.img"

static struct state st;

/* --- shared helpers ---------------------------------------------------- */

int demo(void)
{
    return getenv("LIFEBOAT_DEMO") != NULL;
}

void load_conf(struct conf *c)
{
    char line[256], size[32] = "16G";
    long total, avail;
    FILE *f = fopen("/etc/lifeboat.conf", "r");

    *c = (struct conf){.comp = "lz4", .label = "LIFEBOAT", .lid = "suspend", .sys_ram = "auto",
                       .spill = 80, .warn = 75, .load = 40};
    while (f && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *v;
        if (line[0] == '#' || !eq)
            continue;
        *eq = 0;
        v = eq + 1;
        v[strcspn(v, " \t\r\n#")] = 0;
        if (!strcmp(line, "RAM_SIZE")) snprintf(size, sizeof size, "%s", v);
        else if (!strcmp(line, "ZRAM_COMP")) snprintf(c->comp, sizeof c->comp, "%s", v);
        else if (!strcmp(line, "USB_LABEL")) snprintf(c->label, sizeof c->label, "%s", v);
        else if (!strcmp(line, "LID")) snprintf(c->lid, sizeof c->lid, "%s", v);
        else if (!strcmp(line, "SYSTEM_IN_RAM")) snprintf(c->sys_ram, sizeof c->sys_ram, "%s", v);
        else if (!strcmp(line, "SPILL_PERCENT")) c->spill = atoi(v);
        else if (!strcmp(line, "WARN_PERCENT")) c->warn = atoi(v);
        else if (!strcmp(line, "LOAD_PERCENT")) c->load = atoi(v);
    }
    if (f)
        fclose(f);

    char *end;
    double n = strtod(size, &end);
    meminfo(&total, &avail);
    switch (toupper(*end)) {
    case '%': n = total * 1024.0 * n / 100; break;
    case 'T': n *= 1024; /* fall through */
    case 'G': n *= 1024; /* fall through */
    case 'M': n *= 1024 * 1024; break;
    }
    c->ram_size = (long long)n / 4096 * 4096;
}

void read_state(struct state *s)
{
    char line[512];
    FILE *f = fopen(STATE, "r");

    memset(s, 0, sizeof *s);
    strcpy(s->where, "ram");
    while (f && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *v;
        if (!eq)
            continue;
        *eq = 0;
        v = eq + 1;
        v[strcspn(v, "\n")] = 0;
        if (!strcmp(line, "where")) snprintf(s->where, sizeof s->where, "%s", v);
        else if (!strcmp(line, "moving")) snprintf(s->moving, sizeof s->moving, "%s", v);
        else if (!strcmp(line, "leaving")) snprintf(s->leaving, sizeof s->leaving, "%s", v);
        else if (!strcmp(line, "error")) snprintf(s->error, sizeof s->error, "%s", v);
        else if (!strcmp(line, "note")) snprintf(s->note, sizeof s->note, "%s", v);
        else if (!strcmp(line, "done")) s->done = atol(v);
        else if (!strcmp(line, "total")) s->total = atol(v);
        else if (!strcmp(line, "base")) s->base = atol(v);
        else if (!strcmp(line, "since")) s->since = atol(v);
        else if (!strcmp(line, "error_at")) s->error_at = atol(v);
        else if (!strcmp(line, "note_at")) s->note_at = atol(v);
        else if (!strcmp(line, "lost")) s->lost = atoi(v);
        else if (!strcmp(line, "encrypt")) s->encrypt = atoi(v);
        else if (!strcmp(line, "stick_free")) s->stick_free = atoll(v);
        else if (!strcmp(line, "stick_size")) s->stick_size = atoll(v);
        else if (!strcmp(line, "sys")) snprintf(s->sys, sizeof s->sys, "%s", v);
        else if (!strcmp(line, "hw")) snprintf(s->hw, sizeof s->hw, "%s", v);
        else if (!strcmp(line, "sys_size")) s->sys_size = atoll(v);
        else if (!strcmp(line, "hw_kept")) s->hw_kept = atoll(v);
    }
    if (f)
        fclose(f);
}

void write_state(const struct state *s)
{
    FILE *f = fopen(STATE ".new", "w");
    if (!f)
        return;
    fprintf(f, "where=%s\nmoving=%s\nleaving=%s\nerror=%s\nnote=%s\n"
               "done=%ld\ntotal=%ld\nbase=%ld\nsince=%ld\nerror_at=%ld\nnote_at=%ld\nlost=%d\n"
               "stick_free=%lld\nstick_size=%lld\nsys=%s\nhw=%s\nsys_size=%lld\nhw_kept=%lld\nencrypt=%d\n",
            s->where, s->moving, s->leaving, s->error, s->note,
            s->done, s->total, s->base, s->since, s->error_at, s->note_at, s->lost,
            s->stick_free, s->stick_size, s->sys, s->hw, s->sys_size, s->hw_kept, s->encrypt);
    fchmod(fileno(f), 0644);
    fclose(f);
    rename(STATE ".new", STATE);
}

/* run something, collect what it prints (both streams), return its exit code */
int runv(char *out, size_t n, const char *input, const char *const argv[])
{
    int po[2], pi[2], status = -1;
    size_t got = 0;
    pid_t pid;

    if (pipe(po) || pipe(pi))
        return -1;
    pid = fork();
    if (pid == 0) {
        dup2(pi[0], 0);
        dup2(po[1], 1);
        dup2(po[1], 2);
        close(po[0]); close(po[1]); close(pi[0]); close(pi[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(po[1]);
    close(pi[0]);
    if (input && write(pi[1], input, strlen(input)) < 0)
        status = -1;
    close(pi[1]);
    for (;;) {
        char buf[512];
        ssize_t r = read(po[0], buf, sizeof buf);
        if (r <= 0)
            break;
        if (out && got + 1 < n) {
            size_t take = (size_t)r < n - 1 - got ? (size_t)r : n - 1 - got;
            memcpy(out + got, buf, take);
            got += take;
        }
    }
    close(po[0]);
    if (out && n) {
        while (got && isspace((unsigned char)out[got - 1]))
            got--;
        out[got] = 0;
    }
    if (pid < 0 || waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* start something and forget about it */
void spawn(char *const argv[])
{
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        setsid();
        dup2(null, 0); dup2(null, 1); dup2(null, 2);
        if (fork() == 0) {
            execvp(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

long long used_bytes(const char *path)
{
    struct statvfs v;
    if (statvfs(path, &v))
        return 0;
    return (long long)(v.f_blocks - v.f_bfree) * v.f_frsize;
}

long long free_bytes(const char *path)
{
    struct statvfs v;
    if (statvfs(path, &v))
        return 0;
    return (long long)v.f_bavail * v.f_frsize;
}

void meminfo(long *total_kb, long *avail_kb)
{
    char line[128];
    FILE *f = fopen("/proc/meminfo", "r");
    *total_kb = *avail_kb = 0;
    while (f && fgets(line, sizeof line, f)) {
        sscanf(line, "MemTotal: %ld", total_kb);
        sscanf(line, "MemAvailable: %ld", avail_kb);
    }
    if (f)
        fclose(f);
}

#define BOOT RUN_DIR "/boot"
#define SAVEFILE BOOT "/lifeboat/save.img"
#define SAVEMARK RUN_DIR "/savefile"

static int read_first(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (!f)
        return 0;
    if (!fgets(out, n, f))
        out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return out[0] == '/';
}

static int dev_present(const char *dev)
{
    struct stat sb;
    return !stat(dev, &sb) && S_ISBLK(sb.st_mode);
}

/* Where saves go. A LIFEBOAT partition, or, on a fat stick, the stick's own
 * partition when it holds lifeboat/save.img (the loop over that file is an
 * implementation detail the callers never see). */
int stick_dev(char *dev, size_t n)
{
    struct conf c;
    char mark[128];

    load_conf(&c);
    if (run(dev, n, "blkid", "-L", c.label) == 0 && !strncmp(dev, "/dev/", 5) &&
        strncmp(dev, "/dev/loop", 9))
        return 1;
    if (read_first(SAVEMARK, mark, sizeof mark) && dev_present(mark)) {
        snprintf(dev, n, "%s", mark);
        return 1;
    }
    if (n)
        dev[0] = 0;
    return 0;
}

static int is_save_file(const char *dev)
{
    char mark[128];
    return read_first(SAVEMARK, mark, sizeof mark) && !strcmp(mark, dev);
}

static int mounted_at(const char *path)
{
    struct stat a, b;
    return !stat(path, &a) && !stat(RUN_DIR, &b) && a.st_dev != b.st_dev;
}

/* mount whatever holds the saves on USB */
static int open_stick(const char *dev, int ro, char *err, size_t n)
{
    const char *opts = ro ? "ro,noload" : "noatime";
    char loop[128];

    mkdir(USB, 0755);
    if (!is_save_file(dev))
        return run(err, n, "mount", "-t", "ext4", "-o", opts, dev, USB) ? -1 : 0;
    mkdir(BOOT, 0755);
    if (!mounted_at(BOOT) && run(err, n, "mount", "-t", "vfat", "-o", "rw,noatime", dev, BOOT))
        return -1;
    if (run(loop, sizeof loop, "losetup", "-j", SAVEFILE) == 0 && loop[0] == '/')
        loop[strcspn(loop, ":")] = 0;
    else if (run(loop, sizeof loop, "losetup", "--find", "--show", SAVEFILE)) {
        snprintf(err, n, "%s", loop);
        return -1;
    }
    return run(err, n, "mount", "-t", "ext4", "-o", opts, loop, USB) ? -1 : 0;
}

static void close_stick(void)
{
    char loop[128];

    run(NULL, 0, "umount", USB);
    if (!access(SAVEMARK, F_OK)) {
        while (run(loop, sizeof loop, "losetup", "-j", SAVEFILE) == 0 && loop[0] == '/') {
            loop[strcspn(loop, ":")] = 0;
            if (run(NULL, 0, "losetup", "-d", loop))
                break;
        }
        /* busy when the system itself runs off the stick, which is fine */
        run(NULL, 0, "umount", BOOT);
    }
}

/* --- storage ----------------------------------------------------------- */

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st.error, sizeof st.error, fmt, ap);
    va_end(ap);
    for (char *p = st.error; *p; p++)
        if (*p == '\n')
            *p = ' ';
    st.error_at = time(NULL);
    st.moving[0] = 0;
    write_state(&st);
    fprintf(stderr, "lifeboat: %s\n", st.error);
    return -1;
}

void note_text(struct state *s, const char *text)
{
    snprintf(s->note, sizeof s->note, "%s", text);
    s->note_at = time(NULL);
    write_state(s);
}

static void note(const char *text)
{
    snprintf(st.note, sizeof st.note, "%s", text);
    st.note_at = time(NULL);
    write_state(&st);
}

static void splash_say(const char *text)
{
    FILE *f = fopen(RUN_DIR "/splash.new", "w");
    if (!f)
        return;
    fprintf(f, "text=%s\npct=-1\n", text);
    fclose(f);
    rename(RUN_DIR "/splash.new", RUN_DIR "/splash");
}

static int need_root(void)
{
    if (geteuid() == 0)
        return 0;
    fprintf(stderr, "lifeboat: that needs root (doas lifeboat ...)\n");
    return -1;
}

/* One thing at a time touches the stick: moves, saving, measuring it. */
static int lock_fd = -1;

static int lock_try(void)
{
    if (lock_fd >= 0)
        return 0;
    lock_fd = open(RUN_DIR "/lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0)
        return -1;
    if (flock(lock_fd, LOCK_EX | LOCK_NB)) {
        close(lock_fd);
        lock_fd = -1;
        return -1;
    }
    return 0;
}

static void lock_drop(void)
{
    if (lock_fd >= 0)
        close(lock_fd);
    lock_fd = -1;
}

static int take_lock(void)
{
    return lock_try() ? fail("busy moving things already") : 0;
}

/* spill or unspill under the lock, which is let go again after */
static int locked(int (*fn)(void))
{
    int had = lock_fd >= 0, r = fn();
    if (!had)
        lock_drop();
    return r;
}

/* the live table: length in sectors, and for a linear one the device under it */
static int table(long long *len, char *type, char *dev)
{
    char out[512];
    if (run(out, sizeof out, "dmsetup", "table", DM))
        return -1;
    dev[0] = 0;
    if (sscanf(out, "%*d %lld %15s %31s", len, type, dev) < 2)
        return -1;
    return 0;
}

static void where_now(char *where, char *loop)
{
    char type[16], dev[32], zram[32] = "";
    long long len;
    FILE *f = fopen("/sys/block/zram0/dev", "r");

    if (f) {
        if (fscanf(f, "%31s", zram) != 1)
            zram[0] = 0;
        fclose(f);
    }
    loop[0] = 0;
    strcpy(where, "ram");
    if (table(&len, type, dev))
        return;
    if (!strcmp(type, "clone") || !run(NULL, 0, "dmsetup", "info", MOVER))
        strcpy(where, "moving");
    else if (!strncmp(dev, "7:", 2)) {
        strcpy(where, "stick");
        sprintf(loop, "/dev/loop%d", atoi(dev + 2));
    } else if (strcmp(dev, zram)) {
        char path[96], name[64] = "";
        FILE *nf;
        strcpy(where, "stick");
        snprintf(path, sizeof path, "/sys/dev/block/%s/dm/name", dev);
        if ((nf = fopen(path, "r"))) {
            if (fscanf(nf, "%63s", name) != 1)
                name[0] = 0;
            fclose(nf);
        }
        if (!strcmp(name, SAVEDM))
            strcpy(loop, "/dev/mapper/" SAVEDM);
    }
}

static int put_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY), ok;
    if (fd < 0)
        return -1;
    ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    close(fd);
    return ok ? 0 : -1;
}

static int zram_reset(void)
{
    for (int i = 0; i < 20; i++) {
        if (!put_file("/sys/block/zram0/reset", "1"))
            return 0;
        usleep(100000);
    }
    return -1;
}

static int zram_setup(long long bytes)
{
    struct conf c;
    char size[32];
    load_conf(&c);
    if (zram_reset())
        return -1;
    put_file("/sys/block/zram0/comp_algorithm", c.comp);
    snprintf(size, sizeof size, "%lld", bytes);
    return put_file("/sys/block/zram0/disksize", size);
}

static int mount_stick(void)
{
    char dev[128], out[256];

    if (mounted_at(USB))
        return run(out, sizeof out, "mount", "-o", "remount,rw", USB) ? fail("the stick won't mount writable: %s", out) : 0;
    if (!stick_dev(dev, sizeof dev))
        return fail("no stick plugged in");
    if (open_stick(dev, 0, out, sizeof out))
        return fail("couldn't mount the stick: %s", out);
    return 0;
}

static int clone_status_of(const char *name, long *done, long *total, long *busy, char *mode)
{
    char out[512], *tok[32];
    int n = 0;

    if (run(out, sizeof out, "dmsetup", "status", name))
        return -1;
    for (char *p = strtok(out, " "); p && n < 32; p = strtok(NULL, " "))
        tok[n++] = p;
    /* 0 <len> clone <meta bs> <used>/<total> <region> <hydrated>/<total> <hydrating> ... <mode> */
    if (n < 9 || strcmp(tok[2], "clone") || sscanf(tok[6], "%ld/%ld", done, total) != 2)
        return -1;
    *busy = atol(tok[7]);
    snprintf(mode, 8, "%s", tok[n - 1]);
    return 0;
}

static int make_meta(const char *path, char *loop, size_t n)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0 || ftruncate(fd, 16 << 20)) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    close(fd);
    return run(loop, n, "losetup", "--find", "--show", path) ? -1 : 0;
}

static int reload(const char *name, const char *tbl, char *out, size_t n)
{
    if (run(out, n, "dmsetup", "load", name, "--table", tbl) || run(out, n, "dmsetup", "resume", name)) {
        run(NULL, 0, "dmsetup", "clear", name);
        return -1;
    }
    return 0;
}

/* wait for a clone on <name> to finish. 1 when a cancel was asked for (and
   allowed), 0 when done, -1 when it broke */
static int hydrate(const char *name, int can_cancel)
{
    char out[256], mode[8];
    long done, total, busy;

    run(NULL, 0, "fstrim", CHANGES);
    if (!clone_status_of(name, &done, &total, &busy, mode))
        st.base = done;
    if (run(out, sizeof out, "dmsetup", "message", name, "0", "enable_hydration"))
        return -1;
    for (;;) {
        if (clone_status_of(name, &done, &total, &busy, mode))
            return -1;
        st.done = done;
        st.total = total;
        write_state(&st);
        if (!strcmp(mode, "Fail") || !strcmp(mode, "fail"))
            return -1;
        if (done >= total && !busy)
            return 0;
        if (can_cancel && !access(CANCEL, F_OK))
            return 1;
        usleep(300000);
    }
}

/* Move the changes from src to dest under the mounted filesystem.
 *
 * lifeboat-changes is pointed at a second device, lifeboat-move, which is a
 * dm-clone of src onto dest. Free space is trimmed first, so regions nothing
 * uses count as copied. When it's done lifeboat-changes points straight at
 * dest and lifeboat-move goes away.
 *
 * Cancelling can't just stop: anything written during the move landed on
 * dest. So lifeboat-changes becomes a clone of lifeboat-move back onto src,
 * which copies every used region (from dest where the move got to, from src
 * where it didn't) back where it came from, still under the mounted
 * filesystem, and then points straight at src again.
 *
 * 0 moved, 1 cancelled and back on src, -1 broke. */
static int clone_to(const char *dest, const char *src, const char *heading)
{
    char meta[64], meta2[64] = "", tbl[512], out[256], type[16], dev[32];
    long long len;
    int r;

    if (table(&len, type, dev))
        return fail("no %s device", DM);
    unlink(CANCEL);
    if (make_meta(META, meta, sizeof meta))
        return fail("couldn't make room for the copy map");

    snprintf(tbl, sizeof tbl,
             "0 %lld clone %s %s %s %d 1 no_hydration 4 hydration_threshold 256 hydration_batch_size 64",
             len, meta, dest, src, REGION);
    if (run(out, sizeof out, "dmsetup", "create", MOVER, "--table", tbl)) {
        run(NULL, 0, "losetup", "-d", meta);
        unlink(META);
        return fail("couldn't start the move: %s", out);
    }
    snprintf(tbl, sizeof tbl, "0 %lld linear /dev/mapper/" MOVER " 0", len);
    if (reload(DM, tbl, out, sizeof out)) {
        run(NULL, 0, "dmsetup", "remove", MOVER);
        run(NULL, 0, "losetup", "-d", meta);
        unlink(META);
        return fail("couldn't start the move: %s", out);
    }

    /* from here on new writes land on dest */
    snprintf(st.moving, sizeof st.moving, "%s", heading);
    st.since = time(NULL);
    st.done = st.total = st.base = 0;
    write_state(&st);

    r = hydrate(MOVER, 1);
    if (r < 0)
        return fail("the copy broke. is the stick still in?");

    if (r == 0) {
        snprintf(tbl, sizeof tbl, "0 %lld linear %s 0", len, dest);
        if (reload(DM, tbl, out, sizeof out))
            return fail("couldn't finish the move: %s", out);
    } else {
        /* going back: copy lifeboat-move onto src */
        snprintf(st.moving, sizeof st.moving, "back");
        st.since = time(NULL);
        st.done = st.total = st.base = 0;
        write_state(&st);
        run(NULL, 0, "dmsetup", "message", MOVER, "0", "disable_hydration");
        if (make_meta(META2, meta2, sizeof meta2))
            return fail("couldn't make room to go back. the move goes on");
        snprintf(tbl, sizeof tbl,
                 "0 %lld clone %s %s /dev/mapper/" MOVER " %d 1 no_hydration 4 hydration_threshold 256 "
                 "hydration_batch_size 64", len, meta2, src, REGION);
        if (reload(DM, tbl, out, sizeof out))
            return fail("couldn't go back: %s", out);
        if (hydrate(DM, 0))
            return fail("going back broke. is the stick still in?");
        snprintf(tbl, sizeof tbl, "0 %lld linear %s 0", len, src);
        if (reload(DM, tbl, out, sizeof out))
            return fail("couldn't finish going back: %s", out);
    }
    run(NULL, 0, "dmsetup", "remove", MOVER);
    run(NULL, 0, "losetup", "-d", meta);
    unlink(META);
    if (meta2[0]) {
        run(NULL, 0, "losetup", "-d", meta2);
        unlink(META2);
    }
    unlink(CANCEL);
    return r;
}

/* let go of what changes.img is open through */
static void drop_save(const char *dev)
{
    char out[512], *p;
    if (strcmp(dev, "/dev/mapper/" SAVEDM)) {
        run(NULL, 0, "losetup", "-d", dev);
        return;
    }
    run(out, sizeof out, "cryptsetup", "status", SAVEDM);
    run(NULL, 0, "cryptsetup", "close", SAVEDM);
    if ((p = strstr(out, "device:"))) {
        char loop[64];
        if (sscanf(p + 7, "%63s", loop) == 1)
            run(NULL, 0, "losetup", "-d", loop);
    }
}

/* changes.img as a luks container, opened as SAVEDM */
static int open_encrypted(const char *loop, int format, char *dev, size_t n)
{
    char out[256];
    if (format && runv(out, sizeof out, NULL, (const char *const[]){"cryptsetup", "luksFormat", "--type", "luks2",
            "-q", "--key-file", KEY, "--pbkdf", "argon2id", "--pbkdf-memory", "262144", loop, NULL}))
        return fail("couldn't encrypt the save: %s", out);
    if (run(out, sizeof out, "cryptsetup", "open", "--key-file", KEY, loop, SAVEDM))
        return fail("couldn't open the encrypted save: %s", out);
    snprintf(dev, n, "/dev/mapper/" SAVEDM);
    return 0;
}

static int spill(void)
{
    char where[8], loop[64], type[16], dev[32];
    long long len, need, room;
    int fd;

    if (take_lock())
        return -1;
    where_now(where, loop);
    if (strcmp(where, "ram"))
        return fail("the changes aren't in ram");
    if (mount_stick())
        return -1;
    need = used_bytes(CHANGES);
    room = free_bytes(USB);
    if (room < need + (64 << 20))
        return fail("the stick's too full. this needs %lldM, it has %lldM", need >> 20, room >> 20);

    int enc = !access(USB "/encrypt", F_OK);
    if (enc && access(KEY, F_OK))
        return fail("saves on this stick are encrypted, and this session doesn't know the passphrase. "
                    "settings, encrypt saves");

    /* the last save stays around as .prev until this session is kept or dropped */
    if (!access(IMG, F_OK))
        rename(IMG, PREV);
    table(&len, type, dev);
    fd = open(IMG, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    /* luks2 keeps its header in the first 16M */
    if (fd < 0 || ftruncate(fd, len * 512 + (enc ? 16 << 20 : 0))) {
        rename(PREV, IMG);
        return fail("couldn't make changes.img on the stick");
    }
    close(fd);
    if (run(loop, sizeof loop, "losetup", "--find", "--show", "--direct-io=on", IMG) &&
        run(loop, sizeof loop, "losetup", "--find", "--show", IMG))
        return fail("losetup: %s", loop);
    if (enc) {
        char raw[64];
        snprintf(raw, sizeof raw, "%s", loop);
        if (open_encrypted(raw, 1, loop, sizeof loop)) {
            run(NULL, 0, "losetup", "-d", raw);
            unlink(IMG);
            if (!access(PREV, F_OK))
                rename(PREV, IMG);
            return -1;
        }
    }

    int r = clone_to(loop, "/dev/zram0", "stick");
    if (r < 0)
        return -1;
    if (r == 1) {
        /* cancelled: the stick goes back to how it was */
        drop_save(loop);
        unlink(IMG);
        if (!access(PREV, F_OK))
            rename(PREV, IMG);
        close_stick();
        strcpy(st.where, "ram");
        st.moving[0] = 0;
        close(creat(RUN_DIR "/spill-cancelled", 0644));
        note("cancelled. the changes stay in ram");
        return 1;
    }
    zram_reset();
    strcpy(st.where, "stick");
    st.moving[0] = 0;
    write_state(&st);
    sync();
    return 0;
}

static int unspill(void)
{
    char where[8], loop[64], type[16], dev[32];
    long long len, need;
    long total, avail;

    if (take_lock())
        return -1;
    where_now(where, loop);
    if (strcmp(where, "stick") || !loop[0])
        return fail("the changes aren't on the stick");
    need = used_bytes(CHANGES);
    meminfo(&total, &avail);
    if (need > avail * 1024LL * 6 / 10)
        return fail("that won't fit in ram. %lldM of changes, %ldM free", need >> 20, avail >> 10);
    table(&len, type, dev);
    if (zram_setup(len * 512))
        return fail("couldn't set up zram");

    int r = clone_to("/dev/zram0", loop, "ram");
    if (r < 0)
        return -1;
    if (r == 1) {
        zram_reset();
        strcpy(st.where, "stick");
        st.moving[0] = 0;
        note("cancelled. the changes stay on the stick");
        return 1;
    }
    drop_save(loop);
    close_stick();
    strcpy(st.where, "ram");
    st.moving[0] = 0;
    write_state(&st);
    return 0;
}

int cmd_spill(void)
{
    if (need_root())
        return 1;
    read_state(&st);
    return locked(spill) ? 1 : 0;
}

int cmd_unspill(void)
{
    if (need_root())
        return 1;
    read_state(&st);
    if (locked(unspill))
        return 1;
    note("changes are back in ram. the stick can come out");
    return 0;
}

/* --- leaving ----------------------------------------------------------- */

static void stop_session(void)
{
    char out[64];
    close(open(LEAVING, O_CREAT | O_WRONLY, 0644));
    run(NULL, 0, "pkill", "-TERM", "-u", USER);
    for (int i = 0; i < 30 && !run(out, sizeof out, "pgrep", "-u", USER); i++)
        usleep(100000);
    run(NULL, 0, "pkill", "-KILL", "-u", USER);
}

/* mode: save keeps this session on the stick, drop goes back to the last save */
int cmd_off(const char *mode, int reboot)
{
    char where[8], loop[64];

    if (need_root())
        return 1;
    read_state(&st);
    where_now(where, loop);

    if (!strcmp(mode, "save")) {
        if (!strcmp(where, "ram") && locked(spill))
            return 1;       /* broke or cancelled: stay up */
        unlink(PREV);
        strcpy(where, "stick");
    } else if (!strcmp(where, "stick")) {
        if (!access(PREV, F_OK))
            rename(PREV, IMG);
        else
            unlink(IMG);
        strcpy(where, "dropped");
    }

    strcpy(st.leaving, reboot ? "reboot" : "off");
    write_state(&st);
    usleep(700000);
    stop_session();

    /* the desktop is gone, so the animation gets the screen */
    splash_say(!strcmp(where, "stick") ? "writing everything to the stick" :
               !strcmp(where, "dropped") ? "forgetting this session" : "nothing to save, just going");
    run(NULL, 0, "chvt", "1");
    spawn((char *const[]){"lifeboat-splash", "down", reboot ? "reboot" : "off", NULL});
    usleep(300000);
    run(NULL, 0, "/usr/local/lib/lifeboat/release-stick");
    {
        splash_say(reboot ? "restarting" : "powering off");
        FILE *f = fopen(RUN_DIR "/splash", "a");
        if (f) {
            fputs("end=1\n", f);
            fclose(f);
        }
        for (int i = 0; i < 50 && !run(NULL, 0, "pgrep", "-x", "lifeboat-splash"); i++)
            usleep(100000);
    }
    execl("/usr/bin/busybox", "busybox", reboot ? "reboot" : "poweroff", "-f", NULL);
    return 1;
}

/* --- the watcher -------------------------------------------------------- */

/* open a small terminal window in the session, from root or from the user */
void popup(const char *app, const char *size, const char *what, const char *arg)
{
    char cmd[512];

    if (access(USER_RUN "/wayland-0", F_OK))
        return;
    snprintf(cmd, sizeof cmd,
             "XDG_RUNTIME_DIR=" USER_RUN " WAYLAND_DISPLAY=wayland-0 "
             "exec foot --app-id %s --window-size-chars %s " SELF " %s %s",
             app, size, what, arg ? arg : "");
    if (geteuid() == 0)
        spawn((char *const[]){"su", USER, "-c", cmd, NULL});
    else
        spawn((char *const[]){"sh", "-c", cmd, NULL});
}

/* how much room the stick has. it is only mounted while the session lives
   on it, so otherwise mount it for a moment to look */
static void measure_stick(const char *dev)
{
    struct statvfs v;
    struct stat a, b;
    int mine = 0;

    if (lock_try())
        return;     /* something's using the stick: measure next time */
    (void)a;
    (void)b;
    if (!mounted_at(USB)) {
        if (open_stick(dev, 1, NULL, 0)) {
            close_stick();
            return;
        }
        mine = 1;
    }
    if (!statvfs(USB, &v)) {
        st.stick_free = (long long)v.f_bavail * v.f_frsize;
        st.stick_size = (long long)v.f_blocks * v.f_frsize;
    }
    st.encrypt = !access(USB "/encrypt", F_OK);
    if (mine)
        close_stick();
    lock_drop();
}

static int battery(int *discharging)
{
    char path[300], val[32] = "";
    FILE *f;
    int pct = -1;

    *discharging = 0;
    for (int i = 0; i < 3 && pct < 0; i++) {
        snprintf(path, sizeof path, "/sys/class/power_supply/BAT%d/capacity", i);
        if ((f = fopen(path, "r"))) {
            if (fscanf(f, "%d", &pct) != 1)
                pct = -1;
            fclose(f);
            snprintf(path, sizeof path, "/sys/class/power_supply/BAT%d/status", i);
            if ((f = fopen(path, "r"))) {
                if (fgets(val, sizeof val, f))
                    *discharging = !strncmp(val, "Discharging", 11);
                fclose(f);
            }
        }
    }
    return pct;
}

int cmd_daemon(void)
{
    struct conf c;
    char where[8], loop[64], dev[128];
    time_t warned = 0, warned_full = 0, measured = 0, booted = time(NULL);
    int told_lost = 0, told_low = 0, had_stick = 0;
    char bootdev[128];

    if (need_root())
        return 1;
    signal(SIGHUP, SIG_IGN);
    load_conf(&c);
    read_state(&st);
    where_now(where, loop);
    snprintf(st.where, sizeof st.where, "%s", where);
    st.moving[0] = 0;
    write_state(&st);

    /* booted from a save: pull it into ram if it fits, so the stick can go */
    if (!strcmp(where, "stick") && c.load > 0) {
        long total, avail;
        sleep(4);
        meminfo(&total, &avail);
        if (used_bytes(CHANGES) < avail * 1024LL * c.load / 100 && !locked(unspill))
            note("your saved changes are loaded. the stick can come out");
    }

    for (;;) {
        long total, avail;
        int pct, stick;

        sleep(3);
        read_state(&st);
        if (st.leaving[0])
            continue;
        meminfo(&total, &avail);
        pct = total ? 100 - avail * 100 / total : 0;
        where_now(where, loop);
        stick = stick_dev(dev, sizeof dev);
        int boot_in = read_first(RUN_DIR "/bootdev", bootdev, sizeof bootdev) && dev_present(bootdev);

        /* what lives in ram: drivers, then the system image (ram.c) */
        if (strcmp(where, "moving") && ram_step(&st, pct, boot_in, avail, booted)) {
            write_state(&st);
            continue;
        }
        /* the system itself running off a stick that just went away */
        if (!strcmp(st.sys, "stick") && !boot_in && !st.lost) {
            st.lost = 1;
            write_state(&st);
            if (!told_lost++)
                popup("lifeboat-alert", "62x9", "alert", "lost");
        }

        /* stick room, for the meter on the home screen */
        if (stick && strcmp(where, "moving") && (!had_stick || time(NULL) - measured > 60)) {
            measure_stick(dev);
            measured = time(NULL);
            write_state(&st);
        } else if (!stick && had_stick) {
            st.stick_free = st.stick_size = 0;
            write_state(&st);
        }
        had_stick = stick;

        /* a flat battery takes an unsaved session with it */
        int discharging, bat = battery(&discharging);
        if (bat >= 0 && bat <= 10 && discharging && !told_low) {
            told_low = 1;
            popup("lifeboat-alert", "62x9", "alert", "low");
        } else if (bat > 15 || !discharging) {
            told_low = 0;
        }

        if (!strcmp(where, "ram")) {
            struct stat cs;
            int recently_cancelled = !stat(RUN_DIR "/spill-cancelled", &cs) && time(NULL) - cs.st_mtime < 600;
            if (pct >= c.spill && stick && !recently_cancelled) {
                popup("lifeboat-alert", "62x9", "alert", "moving");
                if (locked(spill))
                    popup("lifeboat-alert", "62x9", "alert", "error");
            } else if (pct >= c.warn && (!stick || recently_cancelled) && time(NULL) - warned > 300) {
                warned = time(NULL);
                popup("lifeboat-alert", "62x9", "alert", "full");
            }
        } else if (!strcmp(where, "stick")) {
            if (!stick && !st.lost) {
                st.lost = 1;
                write_state(&st);
                if (!told_lost++)
                    popup("lifeboat-alert", "62x9", "alert", "lost");
            } else if (stick && st.lost && (boot_in || strcmp(st.sys, "stick"))) {
                st.lost = 0;
                told_lost = 0;
                write_state(&st);
            }
            if (stick && free_bytes(USB) < (256 << 20) && time(NULL) - warned_full > 600) {
                warned_full = time(NULL);
                popup("lifeboat-alert", "62x9", "alert", "stickfull");
            }
        }
        if (strcmp(where, "moving") && strcmp(st.where, where)) {
            snprintf(st.where, sizeof st.where, "%s", where);
            write_state(&st);
        }
    }
}

/* --- other root chores -------------------------------------------------- */

static void field_of(const char *pairs, const char *key, char *out, size_t n)
{
    char want[32];
    const char *p;

    out[0] = 0;
    snprintf(want, sizeof want, "%s=\"", key);
    p = strstr(pairs, want);
    if (!p)
        return;
    p += strlen(want);
    for (size_t i = 0; *p && *p != '"' && i + 1 < n; i++) {
        out[i] = *p++;
        out[i + 1] = 0;
    }
}

int cmd_mount(const char *dev, int rw)
{
    char info[512], fs[32], label[64], mp[160], opts[96], out[256];
    const char *name;

    if (need_root())
        return 1;
    if (run(info, sizeof info, "lsblk", "-P", "-n", "-d", "-o", "FSTYPE,LABEL", dev)) {
        fprintf(stderr, "no such disk\n");
        return 1;
    }
    field_of(info, "FSTYPE", fs, sizeof fs);
    field_of(info, "LABEL", label, sizeof label);
    if (!fs[0]) {
        fprintf(stderr, "nothing on it that mounts\n");
        return 1;
    }
    if (!strcmp(fs, "crypto_LUKS")) {
        fprintf(stderr, "it's encrypted. cryptsetup open it first\n");
        return 1;
    }
    if (!strcmp(fs, "ntfs"))
        strcpy(fs, "ntfs3");

    name = label[0] ? label : strrchr(dev, '/') + 1;
    snprintf(mp, sizeof mp, "/mnt/%s", name);
    for (char *p = mp + 5; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("-_.", *p))
            *p = '_';
    mkdir(mp, 0755);

    int foreign = !strcmp(fs, "vfat") || !strcmp(fs, "exfat") || !strcmp(fs, "ntfs3");
    snprintf(opts, sizeof opts, "%s%s", rw ? "rw" : "ro", foreign ? ",uid=1000,gid=1000" : "");
    if (run(out, sizeof out, "mount", "-t", fs, "-o", opts, dev, mp)) {
        /* a dying disk often has a journal that can't be replayed. read around it */
        const char *skip = !strcmp(fs, "xfs") ? "ro,norecovery" :
                           !strncmp(fs, "ext", 3) ? "ro,noload" : NULL;
        if (rw || !skip || run(out, sizeof out, "mount", "-t", fs, "-o", skip, dev, mp)) {
            rmdir(mp);
            fprintf(stderr, "%s\n", out);
            return 1;
        }
    }
    puts(mp);
    return 0;
}

int cmd_umount(const char *dev)
{
    char out[256];
    if (need_root())
        return 1;
    if (run(out, sizeof out, "umount", dev)) {
        fprintf(stderr, "%s\n", out);
        return 1;
    }
    return 0;
}

/* iwd connects on its own once it finds a .psk file, no agent needed.
   the passphrase comes in on stdin so it never shows up in ps */
int cmd_wifi_key(const char *ssid)
{
    char pass[128] = "", path[400], name[300];
    int plain = 1;
    FILE *f;

    if (need_root())
        return 1;
    if (!fgets(pass, sizeof pass, stdin))
        return 1;
    pass[strcspn(pass, "\r\n")] = 0;
    for (const char *p = ssid; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("-_ ", *p))
            plain = 0;
    if (plain) {
        snprintf(name, sizeof name, "%s", ssid);
    } else {
        size_t o = 1;
        name[0] = '=';
        for (const unsigned char *p = (const unsigned char *)ssid; *p && o < sizeof name - 3; p++)
            o += sprintf(name + o, "%02x", *p);
    }
    snprintf(path, sizeof path, "/var/lib/iwd/%s.psk", name);
    mkdir("/var/lib/iwd", 0700);
    f = fopen(path, "w");
    if (!f)
        return 1;
    fchmod(fileno(f), 0600);
    fprintf(f, "[Security]\nPassphrase=%s\n", pass);
    fclose(f);
    return 0;
}

/* --- rescue ------------------------------------------------------------- */

static long number_after(const char *text, const char *key)
{
    const char *p = strstr(text, key);
    if (!p)
        return -1;
    p += strlen(key);
    while (*p && !isdigit((unsigned char)*p) && *p != '\n')
        p++;
    return isdigit((unsigned char)*p) ? atol(p) : -1;
}

/* the last column of an ata attribute row, e.g. "  5 Reallocated_Sector_Ct ... 0" */
static long attribute(const char *text, const char *name)
{
    const char *p = strstr(text, name), *end;
    if (!p)
        return -1;
    end = p + strcspn(p, "\n");
    while (end > p && end[-1] != ' ')
        end--;
    /* temperatures read "38 (Min/Max 20/45)", so take the tenth column instead */
    if (strstr(name, "Temperature")) {
        int col = 0;
        const char *q = p;
        while (*q && *q != '\n' && col < 8) {
            while (*q && *q != ' ' && *q != '\n') q++;
            while (*q == ' ') q++;
            col++;
        }
        return atol(q);
    }
    return atol(end);
}

/* one line for the disks drawer: "<ok|warn|bad|none> <words>" */
int cmd_health(const char *disk)
{
    static char out[32768];
    char words[160] = "";
    long temp, worn, bad, waiting, media;
    int code, failing;
    size_t o = 0;

    if (need_root())
        return 1;
    code = run(out, sizeof out, "smartctl", "-H", "-A", disk);
    if (code < 0 || (code & 3) || strstr(out, "Unavailable") || strstr(out, "Unknown USB bridge") ||
        (!strstr(out, "overall-health") && !strstr(out, "SMART Health Status"))) {
        puts("none this one doesn't report its health");
        return 0;
    }
    failing = (code & 8) || strstr(out, "FAILED") != NULL;
    temp = number_after(out, "Temperature:");
    if (temp < 0) temp = attribute(out, "Temperature_Celsius");
    if (temp < 0) temp = attribute(out, "Airflow_Temperature_Cel");
    worn = number_after(out, "Percentage Used:");
    media = number_after(out, "Media and Data Integrity Errors:");
    bad = attribute(out, "Reallocated_Sector_Ct");
    waiting = attribute(out, "Current_Pending_Sector");

    if (bad > 0) o += snprintf(words + o, sizeof words - o, ", %ld bad sectors moved", bad);
    if (waiting > 0) o += snprintf(words + o, sizeof words - o, ", %ld sectors unreadable", waiting);
    if (media > 0) o += snprintf(words + o, sizeof words - o, ", %ld data errors", media);
    if (worn >= 0) o += snprintf(words + o, sizeof words - o, ", %ld%% worn", worn);
    if (temp > 0) o += snprintf(words + o, sizeof words - o, ", %ld°C", temp);

    if (failing)
        printf("bad failing. copy it off now%s\n", words);
    else if (bad > 0 || waiting > 0 || media > 0 || worn >= 90)
        printf("warn on its way out%s\n", words);
    else
        printf("ok healthy%s\n", words);
    return 0;
}

/* open a luks partition. the passphrase comes in on stdin */
int cmd_unlock(const char *dev)
{
    char pass[256] = "", name[80], out[256];
    const char *base = strrchr(dev, '/');

    if (need_root())
        return 1;
    if (!fgets(pass, sizeof pass, stdin))
        return 1;
    pass[strcspn(pass, "\r\n")] = 0;
    snprintf(name, sizeof name, "open-%s", base ? base + 1 : dev);
    if (runv(out, sizeof out, pass,
             (const char *const[]){"cryptsetup", "open", "--key-file", "-", dev, name, NULL})) {
        fprintf(stderr, "%s\n", strstr(out, "No key") ? "wrong password" : out);
        return 1;
    }
    printf("/dev/mapper/%s\n", name);
    return 0;
}

static void disk_info(const char *dev, char *fs, size_t nfs, char *mp, size_t nmp)
{
    char info[512];
    fs[0] = mp[0] = 0;
    if (run(info, sizeof info, "lsblk", "-P", "-n", "-d", "-o", "FSTYPE,MOUNTPOINT", dev))
        return;
    field_of(info, "FSTYPE", fs, nfs);
    field_of(info, "MOUNTPOINT", mp, nmp);
}

/* A shell inside the system installed on dev, the arch-chroot way but in a
 * private mount namespace: the mounts only exist for this process, so when
 * the shell exits they are all gone and nothing has to be unmounted by hand. */
int cmd_chroot(const char *dev, const char *mode)
{
    const char *root = RUN_DIR "/inside";
    char out[512], fs[32], mp[128], path[256];

    if (need_root())
        return 1;
    disk_info(dev, fs, sizeof fs, mp, sizeof mp);
    if (mp[0]) {
        fprintf(stderr, "it's mounted at %s. unmount it first, this mounts it by itself\n", mp);
        return 1;
    }
    if (unshare(CLONE_NEWNS) || mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)) {
        perror("unshare");
        return 1;
    }
    mkdir(root, 0755);
    if (run(out, sizeof out, "mount", dev, root)) {
        fprintf(stderr, "couldn't mount %s: %s\n", dev, out);
        return 1;
    }
    /* btrfs installs usually keep the system in a subvolume called @ */
    snprintf(path, sizeof path, "%s/@/etc", root);
    if (!strcmp(fs, "btrfs") && !access(path, F_OK)) {
        umount(root);
        run(out, sizeof out, "mount", "-o", "subvol=@", dev, root);
    }
    snprintf(path, sizeof path, "%s/etc/passwd", root);
    if (access(path, F_OK)) {
        fprintf(stderr, "there's no installed linux system on %s\n", dev);
        return 1;
    }

    snprintf(path, sizeof path, "%s/proc", root);
    run(NULL, 0, "mount", "-t", "proc", "proc", path);
    snprintf(path, sizeof path, "%s/sys", root);
    run(NULL, 0, "mount", "--rbind", "/sys", path);
    snprintf(path, sizeof path, "%s/dev", root);
    run(NULL, 0, "mount", "--rbind", "/dev", path);
    snprintf(path, sizeof path, "%s/run", root);
    run(NULL, 0, "mount", "-t", "tmpfs", "-o", "mode=0755", "tmpfs", path);
    snprintf(path, sizeof path, "%s/tmp", root);
    run(NULL, 0, "mount", "-t", "tmpfs", "tmpfs", path);

    /* its own /boot, /efi, /home and so on, from its fstab */
    snprintf(path, sizeof path, "%s/etc/fstab", root);
    if (run(out, sizeof out, "mount", "-a", "--fstab", path, "--target-prefix", root))
        printf("not everything in its fstab mounted:\n%s\n\n", out);

    /* name lookups should work in there, when it has a plain resolv.conf */
    snprintf(path, sizeof path, "%s/etc/resolv.conf", root);
    run(NULL, 0, "mount", "--bind", "/etc/resolv.conf", path);

    if (chroot(root) || chdir("/")) {
        perror("chroot");
        return 1;
    }
    setenv("HOME", "/root", 1);
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("PS1", "(inside) \\w # ", 1);

    if (!strcmp(mode, "passwd")) {
        char line[512], names[32][64], pick[16];
        int n = 0;
        FILE *f = fopen("/etc/passwd", "r");

        while (f && fgets(line, sizeof line, f) && n < 32) {
            char name[64];
            int uid;
            if (sscanf(line, "%63[^:]:%*[^:]:%d", name, &uid) == 2 &&
                (uid == 0 || (uid >= 1000 && uid < 60000)))
                strcpy(names[n++], name);
        }
        if (f)
            fclose(f);
        printf("whose password?\n\n");
        for (int i = 0; i < n; i++)
            printf("  %d  %s\n", i + 1, names[i]);
        printf("\nnumber: ");
        fflush(stdout);
        if (!fgets(pick, sizeof pick, stdin) || atoi(pick) < 1 || atoi(pick) > n)
            return 1;
        execlp("passwd", "passwd", names[atoi(pick) - 1], NULL);
        perror("passwd");
        return 1;
    }

    if (!strcmp(mode, "fixboot")) {
        /* put the boot loader back and rebuild the initramfs, with whatever
           this system uses itself */
        static const char script[] =
            "set +e\n"
            "name=$(. /etc/os-release 2>/dev/null; echo \"${NAME:-linux}\")\n"
            "id=$(. /etc/os-release 2>/dev/null; echo \"${ID:-linux}\")\n"
            "echo \"fixing $name\"; echo\n"
            "esp=\n"
            "for d in /efi /boot/efi /boot; do\n"
            "  if [ \"$(findmnt -no FSTYPE \"$d\" 2>/dev/null)\" = vfat ]; then esp=$d; break; fi\n"
            "done\n"
            "[ -n \"$esp\" ] || { echo \"its efi partition isn't in its fstab, or didn't mount\"; exit 1; }\n"
            "echo \"efi partition at $esp\"\n"
            "if [ -d /boot/grub ] && command -v grub-install >/dev/null; then\n"
            "  echo; echo '== grub'\n"
            "  grub-install --target=x86_64-efi --efi-directory=\"$esp\" --bootloader-id=\"$id\" &&\n"
            "  grub-install --target=x86_64-efi --efi-directory=\"$esp\" --removable --no-nvram &&\n"
            "  grub-mkconfig -o /boot/grub/grub.cfg\n"
            "elif [ -d /boot/grub2 ] && command -v grub2-install >/dev/null; then\n"
            "  echo; echo '== grub2'\n"
            "  grub2-install --target=x86_64-efi --efi-directory=\"$esp\" --bootloader-id=\"$id\" &&\n"
            "  grub2-mkconfig -o /boot/grub2/grub.cfg\n"
            "elif command -v bootctl >/dev/null && [ -d \"$esp/loader\" ]; then\n"
            "  echo; echo '== systemd-boot'\n"
            "  bootctl --esp-path=\"$esp\" install || bootctl --esp-path=\"$esp\" update\n"
            "else\n"
            "  echo \"no grub or systemd-boot found in it, so the boot loader is left alone\"\n"
            "fi\n"
            "echo; echo '== initramfs'\n"
            "if command -v mkinitcpio >/dev/null; then mkinitcpio -P\n"
            "elif command -v dracut >/dev/null; then dracut --regenerate-all -f\n"
            "elif command -v update-initramfs >/dev/null; then update-initramfs -u -k all\n"
            "else echo 'no initramfs tool found, skipped'; fi\n"
            "echo; echo '== boot entries now'\n"
            "efibootmgr 2>/dev/null || echo '(efibootmgr is not in that system)'\n";
        execl("/bin/sh", "sh", "-c", script, NULL);
        perror("no shell in there");
        return 1;
    }

    printf("you're inside %s now, as root. exit comes back out,\n"
           "and everything unmounts by itself.\n\n", dev);
    fflush(stdout);
    execl("/bin/bash", "bash", NULL);
    execl("/bin/sh", "sh", NULL);
    perror("no shell in there");
    return 1;
}

/* the right checker for whatever is on dev */
int cmd_fsck(const char *dev)
{
    char fs[32], mp[128];

    if (need_root())
        return 1;
    disk_info(dev, fs, sizeof fs, mp, sizeof mp);
    if (mp[0]) {
        fprintf(stderr, "it's mounted at %s. unmount it first\n", mp);
        return 1;
    }
    printf("checking %s (%s)\n\n", dev, fs[0] ? fs : "nothing recognisable");
    fflush(stdout);
    if (!strncmp(fs, "ext", 3)) execlp("e2fsck", "e2fsck", "-f", "-p", dev, NULL);
    else if (!strcmp(fs, "xfs")) execlp("xfs_repair", "xfs_repair", dev, NULL);
    else if (!strcmp(fs, "vfat")) execlp("fsck.fat", "fsck.fat", "-a", "-w", dev, NULL);
    else if (!strcmp(fs, "exfat")) execlp("fsck.exfat", "fsck.exfat", "-y", dev, NULL);
    else if (!strcmp(fs, "ntfs")) execlp("ntfsfix", "ntfsfix", dev, NULL);
    else if (!strcmp(fs, "btrfs")) execlp("btrfs", "btrfs", "check", dev, NULL);
    else if (!strcmp(fs, "f2fs")) execlp("fsck.f2fs", "fsck.f2fs", "-a", dev, NULL);
    fprintf(stderr, "no checker here for that kind of filesystem\n");
    return 1;
}

/* On a fat stick lifeboat booted from (rufus in iso mode, or files copied on),
 * the save place is a file next to the system: lifeboat/save.img, ext4 inside,
 * as big as fat allows (4G) or the stick has room for. Returns 1 when that
 * isn't the case here, so the caller tries a partition instead; otherwise 0,
 * with *rc set to the exit code. */
static int save_file_here(const char *disk, long long *rc)
{
    char bootdev[128], out[256], parent[64];
    long long room, size;
    int made_mount = 0;

    *rc = 1;
    if (!read_first(RUN_DIR "/bootdev", bootdev, sizeof bootdev) || !dev_present(bootdev))
        return 1;
    if (run(parent, sizeof parent, "lsblk", "-ndo", "PKNAME", bootdev) || !parent[0])
        snprintf(parent, sizeof parent, "%s", bootdev + 5);
    if (strcmp(disk + 5, parent) && strcmp(disk, bootdev))
        return 1;
    run(out, sizeof out, "blkid", "-o", "value", "-s", "TYPE", bootdev);
    if (strcmp(out, "vfat"))
        return 1;

    mkdir(BOOT, 0755);
    if (!mounted_at(BOOT)) {
        if (run(out, sizeof out, "mount", "-t", "vfat", "-o", "rw,noatime", bootdev, BOOT)) {
            fprintf(stderr, "couldn't mount %s: %s\n", bootdev, out);
            return 0;
        }
        made_mount = 1;
    }
    run(NULL, 0, "mount", "-o", "remount,rw", BOOT);
    room = free_bytes(BOOT) - (256LL << 20);
    size = room < 4095LL << 20 ? room : 4095LL << 20;
    size = size / (4LL << 20) * (4LL << 20);
    if (size < (1LL << 30)) {
        fprintf(stderr, "only %lldM free on the stick. a save file wants at least 1G\n", room >> 20);
        goto out;
    }
    printf("making a %lldM save file on %s. on a slow stick this takes a few minutes.\n\n",
           size >> 20, bootdev);
    fflush(stdout);
    snprintf(out, sizeof out, "%lld", size);
    if (run(NULL, 0, "fallocate", "-l", out, SAVEFILE)) {
        char count[32];
        snprintf(count, sizeof count, "count=%lld", size >> 22);
        if (runv(NULL, 0, NULL, (const char *const[]){"dd", "if=/dev/zero", "of=" SAVEFILE, "bs=4M", count,
                                                     "status=progress", "conv=fsync", NULL})) {
            unlink(SAVEFILE);
            fprintf(stderr, "couldn't write the save file\n");
            goto out;
        }
    }
    if (run(out, sizeof out, "mkfs.ext4", "-q", "-F", "-L", "LIFEBOAT", "-m", "0", SAVEFILE)) {
        unlink(SAVEFILE);
        fprintf(stderr, "%s\n", out);
        goto out;
    }
    sync();
    {
        FILE *f = fopen(SAVEMARK, "w");
        if (f) {
            fprintf(f, "%s\n", bootdev);
            fclose(f);
        }
    }
    printf("%s %lldG, as lifeboat/save.img\n", bootdev, size >> 30);
    *rc = 0;
out:
    if (made_mount)
        run(NULL, 0, "umount", BOOT);
    return 0;
}

/* Give a stick somewhere to save to: a LIFEBOAT partition in whatever space
 * nothing uses yet. This is what makes a flashed iso persistent. It only ever
 * adds a partition, so nothing on the disk is touched. */
int cmd_persist(const char *disk)
{
    char out[512], dev[128] = "", list[2048];
    long long size = 0;

    setvbuf(stdout, NULL, _IONBF, 0);

    if (need_root())
        return 1;
    if (stick_dev(dev, sizeof dev)) {
        fprintf(stderr, "there's a save place already, on %s\n", dev);
        return 1;
    }
    if (!save_file_here(disk, &size))
        return size;
    run(out, sizeof out, "blkid", "-o", "value", "-s", "PTTYPE", disk);
    if (strcmp(out, "gpt")) {
        fprintf(stderr, "%s has %s. this only adds to a gpt disk, like a flashed lifeboat iso\n", disk,
                out[0] ? "an old style partition table" : "no partition table");
        return 1;
    }
    /* a flashed image keeps its backup table where the image ended. move it to the real end */
    run(NULL, 0, "sgdisk", "-e", disk);
    if (run(out, sizeof out, "sgdisk", "-n", "0:0:0", "-t", "0:8300", "-c", "0:lifeboat-data", disk)) {
        fprintf(stderr, "no free space left on %s\n", disk);
        return 1;
    }
    run(NULL, 0, "partprobe", disk);
    run(NULL, 0, "partx", "-u", disk);
    run(NULL, 0, "udevadm", "settle");
    dev[0] = 0;
    for (int i = 0; i < 20 && !dev[0]; i++) {
        if (!run(list, sizeof list, "lsblk", "-lnpo", "NAME,PARTLABEL", disk))
            for (char *p = strtok(list, "\n"); p; p = strtok(NULL, "\n"))
                if (strstr(p, "lifeboat-data"))
                    sscanf(p, "%127s", dev);
        if (!dev[0])
            usleep(250000);
    }
    if (!dev[0]) {
        fprintf(stderr, "made the partition but it never showed up. unplug it and plug it back in\n");
        return 1;
    }
    if (!run(out, sizeof out, "blockdev", "--getsize64", dev))
        size = atoll(out);
    if (size < (1LL << 30)) {
        run(NULL, 0, "sgdisk", "-d", dev + strlen(dev) - 1, disk);
        fprintf(stderr, "only %lldM free on it, that's too little to be worth it\n", size >> 20);
        return 1;
    }
    if (run(out, sizeof out, "mkfs.ext4", "-q", "-F", "-L", "LIFEBOAT", "-m", "0", dev)) {
        fprintf(stderr, "%s\n", out);
        return 1;
    }
    run(NULL, 0, "udevadm", "settle");
    printf("%s %lldG\n", dev, size >> 30);
    return 0;
}

int cmd_timezone(const char *zone)
{
    char path[200];
    struct stat sb;

    if (need_root())
        return 1;
    if (strstr(zone, "..") || zone[0] == '/')
        return 1;
    snprintf(path, sizeof path, "/usr/share/zoneinfo/%s", zone);
    if (stat(path, &sb) || !S_ISREG(sb.st_mode)) {
        fprintf(stderr, "no such time zone\n");
        return 1;
    }
    unlink("/etc/localtime");
    return symlink(path, "/etc/localtime") ? 1 : 0;
}

/* LID= in /etc/lifeboat.conf */
int cmd_lid_mode(const char *mode)
{
    char line[256], text[4096] = "";
    size_t o = 0;
    int found = 0;
    FILE *f;

    if (need_root())
        return 1;
    if (strcmp(mode, "suspend") && strcmp(mode, "ignore"))
        return 1;
    f = fopen("/etc/lifeboat.conf", "r");
    while (f && fgets(line, sizeof line, f) && o < sizeof text - 300) {
        if (!strncmp(line, "LID=", 4)) {
            snprintf(line, sizeof line, "LID=%s\n", mode);
            found = 1;
        }
        o += snprintf(text + o, sizeof text - o, "%s", line);
    }
    if (f)
        fclose(f);
    if (!found)
        o += snprintf(text + o, sizeof text - o, "LID=%s\n", mode);
    f = fopen("/etc/lifeboat.conf", "w");
    if (!f)
        return 1;
    fputs(text, f);
    fclose(f);
    return 0;
}

/* stop whatever is moving: the changes, or the system image into ram */
int cmd_cancel(void)
{
    char where[8], loop[64];

    if (need_root())
        return 1;
    read_state(&st);
    where_now(where, loop);
    if (!strcmp(st.moving, "back")) {
        fprintf(stderr, "it's already going back\n");
        return 1;
    }
    if (strcmp(where, "moving") && strcmp(st.sys, "moving")) {
        fprintf(stderr, "nothing is moving\n");
        return 1;
    }
    close(creat(CANCEL, 0644));
    return 0;
}

/* lifeboat encrypt on|off. "on" reads the passphrase from stdin and keeps it
   in ram (root only) for the rest of the session; the stick gets a marker so
   every save from then on is encrypted, here or on the next boot */
int cmd_encrypt(const char *how)
{
    char dev[128], pass[256] = "", out[256];
    int mine = 0;

    if (need_root())
        return 1;
    if (!strcmp(how, "on")) {
        if (!fgets(pass, sizeof pass, stdin))
            return 1;
        pass[strcspn(pass, "\r\n")] = 0;
        if (strlen(pass) < 6) {
            fprintf(stderr, "at least 6 characters\n");
            return 1;
        }
        int fd = open(KEY, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (fd < 0 || write(fd, pass, strlen(pass)) != (ssize_t)strlen(pass)) {
            if (fd >= 0)
                close(fd);
            return 1;
        }
        close(fd);
    }
    if (take_lock())
        return 1;
    if (!stick_dev(dev, sizeof dev)) {
        lock_drop();
        if (!strcmp(how, "off"))
            unlink(KEY);
        fprintf(stderr, "no save stick plugged in\n");
        return !strcmp(how, "on") ? 0 : 1;
    }
    if (!mounted_at(USB)) {
        if (open_stick(dev, 0, out, sizeof out)) {
            lock_drop();
            fprintf(stderr, "%s\n", out);
            return 1;
        }
        mine = 1;
    }
    run(NULL, 0, "mount", "-o", "remount,rw", USB);
    if (!strcmp(how, "on"))
        close(creat(USB "/encrypt", 0644));
    else {
        unlink(USB "/encrypt");
        unlink(KEY);
    }
    sync();
    if (mine)
        close_stick();
    lock_drop();
    read_state(&st);
    st.encrypt = !strcmp(how, "on");
    write_state(&st);
    return 0;
}
