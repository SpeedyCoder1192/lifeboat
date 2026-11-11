/* lifeboat drivers: load a driver for every device there is, fix up what
   depended on them, and leave a report on the stick */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glob.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "boat.h"

#define REPORT   RUN_DIR "/drivers.txt"
#define BEFORE   RUN_DIR "/modules.before"
#define GPU_LATE RUN_DIR "/gpu-late"

static void bold(const char *s)
{
    printf("\033[1m%s\033[0m\n", s);
    fflush(stdout);
}

/* drm cards that have a real driver, not the firmware framebuffer */
static int real_gpus(void)
{
    glob_t g;
    int n = 0;
    if (glob("/sys/class/drm/card[0-9]/device/driver", 0, NULL, &g))
        return 0;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char link[256] = "";
        ssize_t len = readlink(g.gl_pathv[i], link, sizeof link - 1);
        if (len > 0) {
            link[len] = 0;
            if (!strstr(link, "simple-framebuffer") && !strstr(link, "efi-framebuffer") &&
                !strstr(link, "vesa-framebuffer"))
                n++;
        }
    }
    globfree(&g);
    return n;
}

static int sound_cards(void)
{
    char out[4096];
    if (run(out, sizeof out, "cat", "/proc/asound/cards") || strstr(out, "no soundcards"))
        return 0;
    int n = 0;
    for (char *p = out; (p = strstr(p, " [")); p += 2)
        n++;
    return n;
}

static int sinks(void)
{
    char out[8192], *p, *end;
    int n = 0;
    if (run(out, sizeof out, "wpctl", "status") || !(p = strstr(out, "Sinks:")))
        return 0;
    end = strstr(p, "Sources:");
    for (char *q = strtok(p, "\n"); q && (!end || q < end); q = strtok(NULL, "\n"))
        if (strstr(q, ". ") && strchr(q, '['))
            n++;
    return n;
}

static int as_root(void)
{
    struct state st;
    int gpus = real_gpus();

    read_state(&st);
    ram_refresh(&st);
    int trimmed = !strcmp(st.hw, "this");
    run(NULL, 0, "sh", "-c", "cut -d' ' -f1 /proc/modules | sort > " BEFORE);

    if (trimmed) {
        bold("putting every driver back in reach");
        if (hw_full(&st))
            printf("the stick isn't in, so only the drivers loaded at boot are here.\n"
                   "plug it in and run this again to search all of them.\n");
    }
    bold("loading a driver for every device");
    system("udevadm trigger --action=add; udevadm settle --timeout=60");
    system("find /sys/devices -name modalias -exec cat {} + 2>/dev/null | sort -u |"
           " xargs -r modprobe -a -q -b 2>/dev/null; udevadm settle --timeout=60");

    /* keep what just loaded in ram too, so the stick can still come out */
    if (strcmp(st.hw, "all"))
        hw_trim(&st);
    write_state(&st);

    if (real_gpus() > gpus)
        close(creat(GPU_LATE, 0644));
    else
        unlink(GPU_LATE);

    run(NULL, 0, "sh", "-c", "/usr/local/lib/lifeboat/driver-report " BEFORE " > " REPORT " 2>&1");
    chmod(REPORT, 0644);
    return 0;
}

/* the report onto the save stick, or the boot stick if that's writable */
static int save_report(const char *file)
{
    char dev[128] = "", mp[256] = "", dst[300];
    const char *tmp = RUN_DIR "/report-mnt";
    int mine = 0;

    if (!stick_dev(dev, sizeof dev) && run(dev, sizeof dev, "cat", RUN_DIR "/bootdev"))
        return 1;
    dev[strcspn(dev, "\n")] = 0;
    if (run(mp, sizeof mp, "findmnt", "-nro", "TARGET", "-S", dev) || !mp[0]) {
        mkdir(tmp, 0755);
        if (run(NULL, 0, "mount", dev, tmp))
            return 1;
        snprintf(mp, sizeof mp, "%s", tmp);
        mine = 1;
    }
    mp[strcspn(mp, "\n")] = 0;
    run(NULL, 0, "mount", "-o", "remount,rw", mp);
    snprintf(dst, sizeof dst, "%s/lifeboat-drivers.txt", mp);
    int r = run(NULL, 0, "cp", file, dst);
    sync();
    if (mine)
        run(NULL, 0, "umount", tmp);
    if (!r)
        printf("and on the stick as lifeboat-drivers.txt\n");
    return r;
}

int cmd_drivers(void)
{
    char home[256], cmd[512];

    if (geteuid() == 0)
        return as_root();

    if (system("doas " SELF " drivers"))
        return 1;

    /* a sound card that turned up after pipewire started */
    if (sound_cards() && !sinks()) {
        bold("restarting sound");
        system("pkill -x wireplumber; pkill -x pipewire-pulse; pkill -x pipewire; sleep 1;"
               " (setsid pipewire >/dev/null 2>&1 &); sleep 1;"
               " (setsid wireplumber >/dev/null 2>&1 &); (setsid pipewire-pulse >/dev/null 2>&1 &); sleep 3");
    }

    snprintf(home, sizeof home, "%s/lifeboat-drivers.txt", getenv("HOME") ? getenv("HOME") : "/tmp");
    snprintf(cmd, sizeof cmd,
             "{ cat " REPORT "; echo; echo '== pipewire'; wpctl status 2>/dev/null; echo; echo '== screens'; wlr-randr; "
             "echo; echo '== brightnessctl'; brightnessctl -l; } > '%s' 2>&1", home);
    system(cmd);

    printf("\nsound cards %d, outputs %d, graphics %s, backlight %s\n", sound_cards(), sinks(),
           real_gpus() ? "ok" : "firmware framebuffer only",
           system("ls /sys/class/backlight/* >/dev/null 2>&1") ? "none" : "ok");
    printf("report: %s\n", home);
    snprintf(cmd, sizeof cmd, "doas " SELF " drivers save '%s'", home);
    system(cmd);

    if (!access(GPU_LATE, F_OK)) {
        char line[16];
        printf("\nthe graphics driver loaded after the desktop started.\n"
               "restart the desktop to use it? open windows close. [y/N] ");
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) && (line[0] == 'y' || line[0] == 'Y'))
            system("labwc --exit");
    }
    return 0;
}

int cmd_drivers_save(const char *file)
{
    return geteuid() == 0 ? save_report(file) : 1;
}
