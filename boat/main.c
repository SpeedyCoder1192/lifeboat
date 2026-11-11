/* lifeboat: one binary for the home screen, the alerts, the watcher and the
 * root side that moves changes between ram and the stick.
 *
 *   lifeboat                 the home screen, in this terminal
 *   lifeboat home            open (or reopen) the home window
 *   lifeboat status          where the changes are
 *   lifeboat spill           move the changes onto the stick      (root)
 *   lifeboat unspill         pull them back into ram              (root)
 *   lifeboat cancel          stop a move half way, going back     (root)
 *   lifeboat off save|drop [reboot]                               (root)
 *   lifeboat mount DEV [rw], lifeboat umount DEV                  (root)
 *   lifeboat chroot DEV [passwd]   a shell inside an installed system (root)
 *   lifeboat fsck DEV              check and repair, whatever the filesystem
 *   lifeboat health DISK           one line of smart verdict        (root)
 *   lifeboat clone SRC DST [used|all], image SRC FILE, restore FILE DST   (root)
 *   lifeboat systems, fixboot DEV, fixboot windows ESP                     (root)
 *   lifeboat scan DEV, bench DISK, surface DISK, wipe DISK quick|secure|zeros (root)
 *   lifeboat update ISO-OR-LINK    a newer lifeboat onto the stick, for the next boot
 *   lifeboat persist DISK          add a save partition in its free space (root)
 *   lifeboat vol +|-|mute, lifeboat light +|-
 *   lifeboat daemon          the ram watcher, started at boot     (root)
 *   lifeboat apply           put the screen settings back, from labwc's autostart
 *
 * --bg in front of a root command detaches it, which is how the screens
 * start things that have to outlive them.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "boat.h"
#include "extras.h"

static void detach(void)
{
    int fd;
    if (fork() > 0)
        _exit(0);
    setsid();
    signal(SIGHUP, SIG_IGN);
    fd = open("/var/log/lifeboat.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        fd = open("/dev/null", O_WRONLY);
    dup2(open("/dev/null", O_RDONLY), 0);
    dup2(fd, 1);
    dup2(fd, 2);
}

static int status(void)
{
    struct state st;
    long total, avail;
    char dev[128];

    read_state(&st);
    meminfo(&total, &avail);
    printf("changes  %lldM %s%s%s\n", used_bytes(CHANGES) >> 20,
           strcmp(st.where, "stick") ? "in ram" : "on the stick",
           st.moving[0] ? ", moving to " : "", st.moving);
    printf("ram      %ld%% used\n", total ? 100 - avail * 100 / total : 0);
    printf("stick    %s\n", stick_dev(dev, sizeof dev) ? dev : "not plugged in");
    printf("system   %s%s\n", !strcmp(st.sys, "ram") ? "in ram" : !strcmp(st.sys, "stick") ? "read off the stick" :
           !strcmp(st.sys, "moving") ? "moving into ram" : "-", st.sys_size ? "" : "");
    printf("drivers  %s\n", !strcmp(st.hw, "all") ? "all of them, in ram" :
           !strcmp(st.hw, "this") ? "this machine's, in ram" : !strcmp(st.hw, "stick") ? "on the stick" : "-");
    if (st.error[0])
        printf("last error  %s\n", st.error);
    return 0;
}

/* one home window at a time: a new one takes over from the old one */
static int home(void)
{
    char pid[32] = "";
    FILE *f = fopen(USER_RUN "/lifeboat-home.pid", "r");

    if (f) {
        if (fgets(pid, sizeof pid, f) && atoi(pid) > 1)
            kill(atoi(pid), SIGTERM);
        fclose(f);
    }
    f = fopen(USER_RUN "/lifeboat-home.pid", "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
    execlp("foot", "foot", "--app-id", "lifeboat-home", "--maximized", SELF, NULL);
    perror("foot");
    return 1;
}

static int knob(const char *what, const char *dir)
{
    if (!strcmp(what, "vol")) {
        if (!strcmp(dir, "mute"))
            return run(NULL, 0, "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle");
        return run(NULL, 0, "wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@",
                   dir[0] == '+' ? "5%+" : "5%-");
    }
    return run(NULL, 0, "brightnessctl", "-q", "-c", "backlight", "set", dir[0] == '+' ? "5%+" : "5%-");
}

static int lid(void)
{
    struct conf c;
    int fd;

    load_conf(&c);
    if (strcmp(c.lid, "suspend"))
        return 0;
    fd = open("/sys/power/state", O_WRONLY);
    if (fd >= 0 && write(fd, "mem", 3) < 0)
        perror("suspend");
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd, *a, *b;
    int bg = argc > 1 && !strcmp(argv[1], "--bg");

    if (bg) {
        argv++;
        argc--;
    }
    cmd = argc > 1 ? argv[1] : "tui";
    a = argc > 2 ? argv[2] : "";
    b = argc > 3 ? argv[3] : "";

    if (bg && geteuid() == 0)
        detach();

    if (!strcmp(cmd, "tui")) return tui_home(NULL);
    if (!strcmp(cmd, "ask")) return tui_home("off");
    if (!strcmp(cmd, "alert")) return tui_alert(a, b);
    if (!strcmp(cmd, "home")) return home();
    if (!strcmp(cmd, "status")) return status();
    if (!strcmp(cmd, "spill")) return cmd_spill();
    if (!strcmp(cmd, "unspill")) return cmd_unspill();
    if (!strcmp(cmd, "cancel")) return cmd_cancel();
    if (!strcmp(cmd, "encrypt") && a[0]) return cmd_encrypt(a);
    if (!strcmp(cmd, "off") && (!strcmp(a, "save") || !strcmp(a, "drop")))
        return cmd_off(a, !strcmp(b, "reboot"));
    if (!strcmp(cmd, "daemon")) return cmd_daemon();
    if (!strcmp(cmd, "mount") && a[0]) return cmd_mount(a, !strcmp(b, "rw"));
    if (!strcmp(cmd, "umount") && a[0]) return cmd_umount(a);
    if (!strcmp(cmd, "wifi-key") && a[0]) return cmd_wifi_key(a);
    if (!strcmp(cmd, "health") && a[0]) return cmd_health(a);
    if (!strcmp(cmd, "unlock") && a[0]) return cmd_unlock(a);
    if (!strcmp(cmd, "chroot") && a[0]) return cmd_chroot(a, b);
    if (!strcmp(cmd, "fsck") && a[0]) return cmd_fsck(a);
    if (!strcmp(cmd, "persist") && a[0]) return cmd_persist(a);
    if (!strcmp(cmd, "timezone") && a[0]) return cmd_timezone(a);
    if (!strcmp(cmd, "clone") && a[0] && b[0]) return cmd_clone(a, b, argc > 4 ? argv[4] : "used");
    if (!strcmp(cmd, "image") && a[0] && b[0]) return cmd_image(a, b);
    if (!strcmp(cmd, "restore") && a[0] && b[0]) return cmd_restore(a, b);
    if (!strcmp(cmd, "systems")) return cmd_systems();
    if (!strcmp(cmd, "fixboot") && a[0]) return strcmp(a, "windows") ? cmd_chroot(a, "fixboot") : cmd_fixboot_windows(b);
    if (!strcmp(cmd, "scan") && a[0]) return cmd_scan(a);
    if (!strcmp(cmd, "bench") && a[0]) return cmd_bench(a);
    if (!strcmp(cmd, "surface") && a[0]) return cmd_surface(a);
    if (!strcmp(cmd, "wipe") && a[0] && b[0]) return cmd_wipe(a, b);
    if (!strcmp(cmd, "update") && a[0]) return cmd_update(a);
    if (!strcmp(cmd, "drivers")) return !strcmp(a, "save") && b[0] ? cmd_drivers_save(b) : cmd_drivers();
    if (!strcmp(cmd, "lid-mode") && a[0]) return cmd_lid_mode(a);
    if (!strcmp(cmd, "apply")) return cmd_apply();
    if ((!strcmp(cmd, "vol") || !strcmp(cmd, "light")) && a[0]) return knob(cmd, a);
    if (!strcmp(cmd, "lid")) return lid();
    if (!strcmp(cmd, "power")) {
        popup("lifeboat-ask", "52x20", "ask", NULL);
        return 0;
    }

    fprintf(stderr, "usage: lifeboat [home|status|drivers|spill|unspill|off save|drop [reboot]|"
                    "mount DEV [rw]|umount DEV|vol +|-|mute|light +|-]\n");
    return 1;
}
