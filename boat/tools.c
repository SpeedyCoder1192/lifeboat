/* The heavier rescue jobs, run as root in a terminal of their own so their
 * progress shows: cloning, images, boot repair, virus scans, speed tests,
 * wiping, and updating lifeboat itself. The fix page (fix.c) picks what to
 * run them on and asks before anything destructive.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "boat.h"

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("\033[1m");
    vprintf(fmt, ap);
    printf("\033[0m\n");
    va_end(ap);
    fflush(stdout);
}

/* run something with its output going straight to the terminal */
static int sh(const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    int st;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    printf("\033[2m$ %s\033[0m\n", cmd);
    fflush(stdout);
    st = system(cmd);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static int root_only(void)
{
    if (geteuid() == 0)
        return 0;
    fprintf(stderr, "that needs root\n");
    return 1;
}

static void lsblk1(const char *dev, const char *col, char *out, size_t n)
{
    run(out, n, "lsblk", "-ndo", col, dev);
}

static long long dev_size(const char *dev)
{
    char out[64];
    lsblk1(dev, "SIZE", out, sizeof out);
    run(out, sizeof out, "lsblk", "-bndo", "SIZE", dev);
    return atoll(out);
}

static int is_disk(const char *dev)
{
    char t[16];
    lsblk1(dev, "TYPE", t, sizeof t);
    return !strcmp(t, "disk") || !strcmp(t, "loop");
}

/* the whole disk a partition sits on */
static void parent_of(const char *dev, char *out, size_t n)
{
    char pk[64];
    lsblk1(dev, "PKNAME", pk, sizeof pk);
    if (pk[0])
        snprintf(out, n, "/dev/%s", pk);
    else
        snprintf(out, n, "%s", dev);
}

/* anything on it mounted, or the stick lifeboat runs from */
static int busy(const char *dev, char *why, size_t n)
{
    char out[2048], disk[64], bootdev[128] = "", bootdisk[64] = "";
    FILE *f;

    parent_of(dev, disk, sizeof disk);
    if ((f = fopen(RUN_DIR "/bootdev", "r"))) {
        if (fscanf(f, "%127s", bootdev) != 1)
            bootdev[0] = 0;
        fclose(f);
    }
    if (bootdev[0]) {
        parent_of(bootdev, bootdisk, sizeof bootdisk);
        if (!strcmp(bootdisk, disk)) {
            snprintf(why, n, "that's the stick lifeboat is running from");
            return 1;
        }
    }
    run(out, sizeof out, "lsblk", "-nlo", "MOUNTPOINT", dev);
    for (char *p = strtok(out, "\n"); p; p = strtok(NULL, "\n"))
        if (*p && *p != ' ') {
            snprintf(why, n, "something on it is mounted at %s. unmount it first", p);
            return 1;
        }
    return 0;
}

/* which partclone handles this filesystem, or NULL for a raw copy */
static const char *partclone_for(const char *dev)
{
    static char bin[48];
    char fs[32];
    lsblk1(dev, "FSTYPE", fs, sizeof fs);
    if (!strncmp(fs, "ext", 3) || !strcmp(fs, "xfs") || !strcmp(fs, "btrfs") || !strcmp(fs, "ntfs") ||
        !strcmp(fs, "exfat") || !strcmp(fs, "f2fs"))
        snprintf(bin, sizeof bin, "partclone.%s", fs);
    else if (!strcmp(fs, "vfat"))
        snprintf(bin, sizeof bin, "partclone.fat");
    else
        return NULL;
    return bin;
}

static int confirm_line(void)
{
    char line[16];
    printf("\ntype yes to go on: ");
    fflush(stdout);
    return fgets(line, sizeof line, stdin) && !strncmp(line, "yes", 3);
}

/* --- cloning --------------------------------------------------------------- */

static int copy_part(const char *src, const char *dst, int used)
{
    const char *pc = used ? partclone_for(src) : NULL;
    char map[128];
    if (pc)
        return sh("%s -b -s '%s' -o '%s'", pc, src, dst);
    snprintf(map, sizeof map, "/tmp/clone-%ld.map", (long)time(NULL));
    if (sh("ddrescue -f -d -n '%s' '%s' '%s'", src, dst, map))
        return 1;
    /* a second pass for whatever didn't read the first time */
    return sh("ddrescue -f -d -r2 '%s' '%s' '%s'", src, dst, map);
}

/* the partitions of a disk, in order */
static int list_parts(const char *disk, char out[32][64])
{
    char text[4096];
    int n = 0;
    if (run(text, sizeof text, "lsblk", "-nlpo", "NAME,TYPE", disk))
        return 0;
    for (char *p = strtok(text, "\n"); p && n < 32; p = strtok(NULL, "\n")) {
        char name[64], type[16];
        if (sscanf(p, "%63s %15s", name, type) == 2 && !strcmp(type, "part"))
            snprintf(out[n++], 64, "%s", name);
    }
    return n;
}

/* lifeboat clone SRC DST [used|all]: disk to disk or partition to partition */
int cmd_clone(const char *src, const char *dst, const char *how)
{
    char why[160], tab[32];
    int used = strcmp(how, "all") != 0, bad = 0;

    if (root_only())
        return 1;
    if (!strcmp(src, dst) || busy(dst, why, sizeof why)) {
        fprintf(stderr, "can't write to %s: %s\n", dst, strcmp(src, dst) ? why : "it's the same one");
        return 1;
    }
    if (is_disk(src) != is_disk(dst)) {
        fprintf(stderr, "a disk goes onto a disk, a partition onto a partition\n");
        return 1;
    }
    if (dev_size(dst) < dev_size(src) && !(used && !is_disk(src))) {
        fprintf(stderr, "%s is smaller than %s\n", dst, src);
        return 1;
    }
    say("%s onto %s, %s", src, dst, used ? "only the used space" : "every byte");
    printf("everything on %s gets replaced.", dst);
    if (!confirm_line())
        return 1;

    if (!is_disk(src))
        return copy_part(src, dst, used);

    if (!used)
        bad = copy_part(src, dst, 0);
    else {
        /* the partition table first, then each partition the quick way */
        run(tab, sizeof tab, "blkid", "-o", "value", "-s", "PTTYPE", src);
        if (!strcmp(tab, "gpt"))
            bad = sh("sgdisk -R '%s' '%s' && sgdisk -e '%s'", dst, src, dst);
        else if (!strcmp(tab, "dos"))
            bad = sh("sfdisk -d '%s' | sfdisk -q '%s'", src, dst);
        else
            return copy_part(src, dst, 0);     /* no table: just copy it all */
        if (bad)
            return 1;
        sh("partprobe '%s'; udevadm settle", dst);
        char sp[32][64], dp[32][64];
        int ns = list_parts(src, sp), nd = list_parts(dst, dp);
        for (int i = 0; i < ns; i++) {
            if (i >= nd) {
                fprintf(stderr, "the copy has no partition for %s\n", sp[i]);
                bad = 1;
                continue;
            }
            say("\n%s -> %s", sp[i], dp[i]);
            bad |= copy_part(sp[i], dp[i], 1);
        }
    }
    sync();
    if (!bad)
        say("\ndone. the copy has the same disk and partition ids as the original, so take one of "
            "them out before booting from the other.");
    return bad;
}

/* lifeboat image SRC FILE: a disk or partition into a file */
int cmd_image(const char *src, const char *file)
{
    const char *pc;
    if (root_only())
        return 1;
    if (!access(file, F_OK)) {
        fprintf(stderr, "%s is there already\n", file);
        return 1;
    }
    pc = is_disk(src) ? NULL : partclone_for(src);
    if (pc) {
        say("%s -> %s (only the used space, compressed)", src, file);
        return sh("%s -c -s '%s' -o - | zstd -T0 -3 -q -o '%s'", pc, src, file);
    }
    say("%s -> %s (every byte, sparse, keeps going past bad spots)", src, file);
    return sh("ddrescue -d -S -n '%s' '%s' '%s.map' && ddrescue -d -S -r2 '%s' '%s' '%s.map'",
              src, file, file, src, file, file);
}

/* lifeboat restore FILE DST: an image back onto a disk or partition */
int cmd_restore(const char *file, const char *dst)
{
    char why[160];
    if (root_only())
        return 1;
    if (busy(dst, why, sizeof why)) {
        fprintf(stderr, "can't write to %s: %s\n", dst, why);
        return 1;
    }
    say("%s -> %s", file, dst);
    printf("everything on %s gets replaced.", dst);
    if (!confirm_line())
        return 1;
    if (strstr(file, ".pcl"))
        return sh("zstd -dc '%s' | partclone.restore -s - -o '%s'", file, dst);
    return sh("ddrescue -f -d '%s' '%s' /tmp/restore-%ld.map", file, dst, (long)time(NULL));
}

/* --- boot repair ---------------------------------------------------------------- */

/* every system on the disks: "linux DEV name" and "windows ESP" lines */
int cmd_systems(void)
{
    char out[8192], mp[] = RUN_DIR "/look";

    if (root_only())
        return 1;
    mkdir(mp, 0755);
    if (run(out, sizeof out, "lsblk", "-nlpo", "NAME,FSTYPE,TYPE,MOUNTPOINT"))
        return 1;
    for (char *p = strtok(out, "\n"); p; p = strtok(NULL, "\n")) {
        char name[64], fs[32] = "", type[16] = "", path[256], line[256];
        if (sscanf(p, "%63s %31s %15s", name, fs, type) < 3 || strcmp(type, "part"))
            continue;
        if (strcmp(fs, "vfat") && strncmp(fs, "ext", 3) && strcmp(fs, "xfs") && strcmp(fs, "btrfs") &&
            strcmp(fs, "f2fs"))
            continue;
        if (run(NULL, 0, "mount", "-o", "ro", name, mp))
            continue;
        if (!strcmp(fs, "vfat")) {
            snprintf(path, sizeof path, "%s/EFI/Microsoft/Boot/bootmgfw.efi", mp);
            if (!access(path, F_OK))
                printf("windows %s Windows\n", name);
        } else {
            FILE *f;
            snprintf(path, sizeof path, "%s/etc/os-release", mp);
            if (!strcmp(fs, "btrfs") && access(path, F_OK)) {
                /* the usual @ subvolume */
                run(NULL, 0, "umount", mp);
                run(NULL, 0, "mount", "-o", "ro,subvol=@", name, mp);
            }
            if ((f = fopen(path, "r"))) {
                char pretty[128] = "Linux";
                while (fgets(line, sizeof line, f))
                    if (!strncmp(line, "PRETTY_NAME=", 12)) {
                        char *v = line + 12;
                        v[strcspn(v, "\n")] = 0;
                        if (*v == '"') {
                            v++;
                            v[strcspn(v, "\"")] = 0;
                        }
                        snprintf(pretty, sizeof pretty, "%s", v);
                    }
                fclose(f);
                printf("linux %s %s\n", name, pretty);
            }
        }
        run(NULL, 0, "umount", mp);
    }
    rmdir(mp);
    return 0;
}

/* Windows: put its boot entry back, and the fallback loader if it's missing */
int cmd_fixboot_windows(const char *esp)
{
    char disk[64], num[16], mp[] = RUN_DIR "/esp";
    if (root_only())
        return 1;
    parent_of(esp, disk, sizeof disk);
    run(num, sizeof num, "lsblk", "-ndo", "PARTN", esp);
    mkdir(mp, 0755);
    if (sh("mount '%s' '%s'", esp, mp))
        return 1;
    say("windows boot manager on %s", esp);
    sh("efibootmgr | grep -i 'windows boot manager' || "
       "efibootmgr -c -d '%s' -p %s -L 'Windows Boot Manager' -l '\\EFI\\Microsoft\\Boot\\bootmgfw.efi'",
       disk, num);
    sh("[ -e '%s/EFI/Boot/bootx64.efi' ] || { mkdir -p '%s/EFI/Boot' && "
       "cp '%s/EFI/Microsoft/Boot/bootmgfw.efi' '%s/EFI/Boot/bootx64.efi'; }", mp, mp, mp, mp);
    sync();
    sh("umount '%s'", mp);
    rmdir(mp);
    say("\nboot entries now:");
    sh("efibootmgr");
    return 0;
}

/* --- virus scan ------------------------------------------------------------------- */

int cmd_scan(const char *dev)
{
    char mp[160] = "", out[256];
    int r;
    if (root_only())
        return 1;
    run(mp, sizeof mp, "lsblk", "-ndo", "MOUNTPOINT", dev);
    if (!mp[0]) {
        if (run(out, sizeof out, SELF, "mount", dev)) {
            fprintf(stderr, "%s\n", out);
            return 1;
        }
        snprintf(mp, sizeof mp, "%s", out);
    }
    if (access("/usr/bin/clamscan", X_OK)) {
        say("getting clamav (about 180M, plus its virus list)");
        if (sh("pacman -Sy --needed --noconfirm clamav"))
            return 1;
    }
    say("updating the virus list");
    sh("mkdir -p /var/lib/clamav && chown clamav:clamav /var/lib/clamav 2>/dev/null; freshclam --stdout");
    say("\nscanning %s. only what's infected gets listed", mp);
    r = sh("clamscan -r -i --stdout --max-filesize=200M --max-scansize=400M "
           "--log=/home/" USER "/virus-scan.txt '%s'", mp);
    say(r == 1 ? "\nfound some. the list is in ~/virus-scan.txt. nothing was deleted" :
        r == 0 ? "\nnothing found" : "\nthe scan had trouble, see above");
    return 0;
}

/* --- speed test ---------------------------------------------------------------------- */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* reads only, so it's safe on anything */
int cmd_bench(const char *dev)
{
    long long size = dev_size(dev), done = 0;
    void *buf;
    int fd;
    double t0;
    long ios = 0;

    if (root_only())
        return 1;
    fd = open(dev, O_RDONLY | O_DIRECT);
    if (fd < 0 || posix_memalign(&buf, 4096, 1 << 20)) {
        perror(dev);
        return 1;
    }
    say("%s, %.1fG. reading only, nothing gets written", dev, size / 1073741824.0);

    printf("\nbig reads, one after another   ");
    fflush(stdout);
    t0 = now_s();
    while (now_s() - t0 < 8 && done + (1 << 20) <= size) {
        if (pread(fd, buf, 1 << 20, done) <= 0)
            break;
        done += 1 << 20;
    }
    printf("%.0f MB/s\n", done / (now_s() - t0) / 1e6);

    printf("small reads, all over the disk ");
    fflush(stdout);
    srand(time(NULL));
    t0 = now_s();
    while (now_s() - t0 < 8) {
        long long off = ((long long)rand() * rand()) % (size / 4096) * 4096;
        if (pread(fd, buf, 4096, off) <= 0)
            break;
        ios++;
    }
    double iops = ios / (now_s() - t0);
    printf("%.0f per second (%.2f ms each)\n", iops, 1000 / iops);
    printf("\n%s\n", iops > 5000 ? "that's an ssd, and a healthy speed for one" :
                     iops > 300 ? "an ssd on a slow link, or an old one" :
                     iops > 40 ? "a spinning disk, normal speeds" :
                     "very slow. a struggling disk, or a usb stick");
    close(fd);
    free(buf);
    return 0;
}

/* --- wiping --------------------------------------------------------------------------- */

/* lifeboat wipe DISK quick|secure|zeros */
int cmd_wipe(const char *dev, const char *how)
{
    char why[160], line[64], tran[16], name[64];
    if (root_only())
        return 1;
    if (!is_disk(dev)) {
        fprintf(stderr, "wipe takes a whole disk\n");
        return 1;
    }
    if (busy(dev, why, sizeof why)) {
        fprintf(stderr, "can't wipe %s: %s\n", dev, why);
        return 1;
    }
    lsblk1(dev, "TRAN", tran, sizeof tran);
    snprintf(name, sizeof name, "%s", strrchr(dev, '/') + 1);
    say("this destroys everything on %s for good", dev);
    sh("lsblk -o NAME,SIZE,MODEL,FSTYPE,LABEL '%s'", dev);
    printf("\ntype the disk's name (%s) to go on: ", name);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin) || strncmp(line, name, strlen(name)) || !isspace((unsigned char)line[strlen(name)])) {
        printf("left alone\n");
        return 1;
    }
    if (!strcmp(how, "quick")) {
        /* signatures off first, so nothing mounts or shows up even where the
           drive ignores the discard. then tell an ssd everything is free */
        if (sh("for p in $(lsblk -nlpo NAME '%s' | tac); do wipefs -qaf \"$p\"; done", dev))
            return 1;
        if (sh("blkdiscard -f '%s'", dev))
            say("no discard support: only the partition table and labels are gone");
        return 0;
    }
    if (!strcmp(how, "secure")) {
        if (!strcmp(tran, "nvme")) {
            say("asking the drive to erase itself (nvme format, secure erase)");
            return sh("nvme format '%s' --ses=1 --force", dev);
        }
        char out[4096];
        run(out, sizeof out, "hdparm", "-I", dev);
        if (!strstr(out, "Security:") || strstr(out, "not\tsupported")) {
            fprintf(stderr, "this drive has no secure erase. use zeros instead\n");
            return 1;
        }
        if (strstr(out, "\tfrozen") && !strstr(out, "not\tfrozen")) {
            fprintf(stderr, "the drive's security is frozen. suspend the machine for a moment (close the lid)\n"
                            "and try again, that usually unfreezes it\n");
            return 1;
        }
        say("asking the drive to erase itself (ata secure erase). this can take a long time");
        if (sh("hdparm --user-master u --security-set-pass lifeboat '%s'", dev))
            return 1;
        return sh("hdparm --user-master u --security-erase lifeboat '%s'", dev);
    }
    say("writing zeros over all of it");
    return sh("shred -n 0 -z -v '%s' && sync", dev);
}

/* --- updating lifeboat ------------------------------------------------------------------ */

/* lifeboat update FILE-OR-LINK: the files out of a newer lifeboat iso, onto the stick */
int cmd_update(const char *src)
{
    char bootdev[128] = "", fs[32], dir[] = RUN_DIR "/update", iso[300], target[200], ver_new[64] = "",
         ver_old[64] = "", path[400], save[128];
    static const char *files[] = {"vmlinuz", "initramfs.img", "system.sfs", "hw.sfs"};
    FILE *f;
    struct stat a, b;
    int boot_rw;

    if (root_only())
        return 1;
    if ((f = fopen(RUN_DIR "/bootdev", "r"))) {
        if (fscanf(f, "%127s", bootdev) != 1)
            bootdev[0] = 0;
        fclose(f);
    }
    if (!bootdev[0]) {
        fprintf(stderr, "lifeboat didn't boot from a stick, so there's nothing to update\n");
        return 1;
    }
    lsblk1(bootdev, "FSTYPE", fs, sizeof fs);
    boot_rw = !strcmp(fs, "vfat");

    mkdir(dir, 0755);
    if (!strncmp(src, "http://", 7) || !strncmp(src, "https://", 8)) {
        /* downloads land on the stick's save partition, or in ram */
        char room[160] = RUN_DIR "/update";
        if (stick_dev(save, sizeof save) && !run(NULL, 0, "mount", "-o", "remount,rw", RUN_DIR "/usb"))
            snprintf(room, sizeof room, RUN_DIR "/usb");
        snprintf(iso, sizeof iso, "%s/lifeboat-new.iso", room);
        say("downloading %s", src);
        if (sh("curl -fL --progress-bar -o '%s' '%s'", iso, src))
            return 1;
    } else {
        snprintf(iso, sizeof iso, "%s", src);
    }
    say("reading %s", iso);
    if (sh("bsdtar -xf '%s' -C '%s' lifeboat/version 2>/dev/null; true", iso, dir))
        return 1;
    snprintf(path, sizeof path, "%s/lifeboat/version", dir);
    if ((f = fopen(path, "r"))) {
        if (!fgets(ver_new, sizeof ver_new, f))
            ver_new[0] = 0;
        fclose(f);
    }
    if ((f = fopen("/etc/lifeboat-version", "r"))) {
        if (!fgets(ver_old, sizeof ver_old, f))
            ver_old[0] = 0;
        fclose(f);
    }
    ver_new[strcspn(ver_new, "\n")] = 0;
    ver_old[strcspn(ver_old, "\n")] = 0;
    if (!ver_new[0]) {
        fprintf(stderr, "that isn't a lifeboat iso (no lifeboat/version in it)\n");
        return 1;
    }
    say("this is %s, that one is %s", ver_old[0] ? ver_old : "an older build", ver_new);
    if (!strcmp(ver_new, ver_old))
        printf("same version. putting it on anyway\n");

    /* where the new files go: the boot partition when it's fat, otherwise the
       kernel onto the efi partition and the images onto the save partition,
       which init looks at first */
    if (boot_rw) {
        snprintf(target, sizeof target, RUN_DIR "/boot");
        mkdir(target, 0755);
        if (stat(target, &a) || stat(RUN_DIR, &b) || a.st_dev == b.st_dev) {
            if (sh("mount -o rw '%s' '%s'", bootdev, target))
                return 1;
        } else if (sh("mount -o remount,rw '%s'", target)) {
            return 1;
        }
    } else {
        if (!stick_dev(save, sizeof save)) {
            fprintf(stderr, "this stick was flashed from the iso and has nowhere to put the update.\n"
                            "make it a save stick first (d, then 9, on it), or flash the new iso.\n");
            return 1;
        }
        snprintf(target, sizeof target, RUN_DIR "/usb");
        if (sh("mountpoint -q '%s' || mount -o rw '%s' '%s'; mount -o remount,rw '%s'", target, save, target,
               target))
            return 1;
    }
    sh("mkdir -p '%s/lifeboat'", target);

    for (int i = 0; i < 4; i++) {
        char dest[300], tmp[320];
        int kernel = i < 2;
        if (!boot_rw && kernel) {
            /* the efi partition of a flashed iso */
            char efi[128];
            if (run(efi, sizeof efi, "blkid", "-L", "LBEFI") || !efi[0])
                return 1;
            snprintf(dest, sizeof dest, RUN_DIR "/efi/lifeboat/%s", files[i]);
            sh("mkdir -p " RUN_DIR "/efi; mountpoint -q " RUN_DIR "/efi || mount '%s' " RUN_DIR "/efi", efi);
        } else {
            snprintf(dest, sizeof dest, "%s/lifeboat/%s", target, files[i]);
        }
        snprintf(tmp, sizeof tmp, "%s.new", dest);
        say("%s", files[i]);
        /* small files: next to the old ones, then swapped. big ones straight over,
           when there isn't room for both */
        if (sh("bsdtar -xOf '%s' 'lifeboat/%s' | pv -pterb > '%s'", iso, files[i], tmp)) {
            unlink(tmp);
            if (sh("rm -f '%s' && bsdtar -xOf '%s' 'lifeboat/%s' | pv -pterb > '%s'", dest, iso, files[i], dest)) {
                fprintf(stderr, "couldn't write %s. the stick may need the old iso flashed again\n", dest);
                return 1;
            }
        } else if (rename(tmp, dest)) {
            perror(dest);
            return 1;
        }
        sync();
    }
    if (!boot_rw)
        sh("umount " RUN_DIR "/efi");
    sh("cp '%s/lifeboat/version' '%s/lifeboat/version'", dir, target);
    sync();
    say("\nupdated to %s. it starts with the next boot. saved sessions carry over;\n"
        "packages you added sit on top of the new system.", ver_new);
    return 0;
}
