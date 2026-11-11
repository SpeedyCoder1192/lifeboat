/* what the home screen knows about the machine, gathered off the ui thread */
#ifndef PROBE_H
#define PROBE_H
#include "boat.h"

struct appent { char name[64], exec[300], src[12]; int term; };
struct net { char name[64], path[200], known[200], type[8]; int signal, connected; };
struct disk {
    char dev[40], fs[16], label[40], mount[128], model[40], tran[8];
    long long size;
    int rm, stick;
    char parent[40], health[120];   /* the whole disk it sits on, and its smart verdict */
    long long psize;
    int hlevel;                     /* 0 unknown, 1 fine, 2 worn, 3 failing */
};
struct bt { char name[64], path[160]; int paired, connected, rssi; };
struct sink { char name[100]; int id, current; };

struct snap {
    struct appent apps[300];
    struct net nets[48];
    struct disk disks[48];
    int napps, nnets, ndisks;
    int wifi, scanning;             /* wifi: is there a card iwd can drive */
    char station[200], ssid[64];
    long mem_total, mem_avail;      /* kb */
    int bat, charging;              /* bat -1: no battery */
    int bat_mins;                   /* minutes left on battery, -1 unknown */
    struct bt bts[32];
    int nbt, bt, bt_on, bt_scanning;
    char adapter[160];
    int vol, muted, light;          /* -1: unknown */
    char route[16];                 /* interface with the default route */
    long long changes;              /* bytes used by this session's changes */
    int stick;
    struct state st;
};

void probe_start(void);
void probe_get(struct snap *out);
void probe_poke(void);
void probe_wifi(const char *verb, const char *path);
int probe_wifi_result(char *msg, size_t n);
void probe_bt(const char *verb, const char *path);
void probe_bt_scan(int on);
int probe_sinks(struct sink *out, int max);

#endif
