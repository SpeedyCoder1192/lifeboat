/* things every part of the lifeboat binary shares */
#ifndef BOAT_H
#define BOAT_H
#include <stddef.h>

#define RUN_DIR  "/run/lifeboat"
#define STATE    RUN_DIR "/state"
#define CHANGES  RUN_DIR "/changes"
#define USB      RUN_DIR "/usb"
#define META     RUN_DIR "/meta.img"
#define LEAVING  RUN_DIR "/leaving"
#define IMG      USB "/changes.img"
#define PREV     USB "/changes.img.prev"
#define DM       "lifeboat-changes"
#define SELF     "/usr/local/bin/lifeboat"
#define USER     "lifeboat"
#define USER_RUN "/run/user/1000"

struct conf {
    long long ram_size;     /* bytes */
    char comp[16], label[32], lid[16], sys_ram[8];
    int spill, warn, load;
};

/* /run/lifeboat/state, key=value lines. root writes it, the screens read it */
struct state {
    char where[8];          /* ram, stick */
    char moving[8];         /* "", ram, stick: where a running move is headed */
    char leaving[8];        /* "", off, reboot */
    char error[200], note[160];
    long done, total, base, since, error_at, note_at;
    int lost;
    int encrypt;            /* the stick wants saves encrypted */
    long long stick_free, stick_size;   /* bytes, measured by the watcher */
    char sys[8];            /* the system image: ram, stick, moving, none */
    char hw[8];             /* the drivers: all (in ram), stick, this (this machine's in ram) */
    long long sys_size, hw_kept;
};

void load_conf(struct conf *c);
void read_state(struct state *s);
void write_state(const struct state *s);

int runv(char *out, size_t n, const char *input, const char *const argv[]);
#define run(out, n, ...) runv(out, n, NULL, (const char *const[]){__VA_ARGS__, NULL})
void spawn(char *const argv[]);
int demo(void);

long long used_bytes(const char *path);
long long free_bytes(const char *path);
void meminfo(long *total_kb, long *avail_kb);
int stick_dev(char *dev, size_t n);

/* ram.c: root only */
void ram_refresh(struct state *st);
int ram_step(struct state *st, int pct, int stick_in, long avail_kb, long booted);
int hw_trim(struct state *st);
int hw_full(struct state *st);
int sys_to_stick(struct state *st);
int sys_to_ram(struct state *st);
void note_text(struct state *st, const char *text);

/* store.c: root only */
int cmd_spill(void);
int cmd_unspill(void);
int cmd_cancel(void);
int cmd_encrypt(const char *how);
int cmd_off(const char *mode, int reboot);
int cmd_daemon(void);
int cmd_mount(const char *dev, int rw);
int cmd_umount(const char *dev);
int cmd_wifi_key(const char *ssid);
int cmd_health(const char *disk);
int cmd_unlock(const char *dev);
int cmd_chroot(const char *dev, const char *mode);
int cmd_fsck(const char *dev);
int cmd_persist(const char *disk);
int cmd_timezone(const char *zone);
int cmd_clone(const char *src, const char *dst, const char *how);
int cmd_image(const char *src, const char *file);
int cmd_restore(const char *file, const char *dst);
int cmd_systems(void);
int cmd_fixboot_windows(const char *esp);
int cmd_scan(const char *dev);
int cmd_bench(const char *dev);
int cmd_wipe(const char *dev, const char *how);
int cmd_update(const char *src);
int cmd_drivers(void);
int cmd_drivers_save(const char *file);
int cmd_surface(const char *dev);
int cmd_lid_mode(const char *mode);
void popup(const char *app, const char *size, const char *what, const char *arg);

/* home.c */
int tui_home(const char *sheet);
int tui_alert(const char *kind, const char *text);

#endif
