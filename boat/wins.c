#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>

#include "boat.h"
#include "toplevel-protocol.h"
#include "wins.h"

#define MAX 32

static struct slot {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char title[80], app[48];
    int active;
} slots[MAX];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct wl_display *display;
static struct wl_seat *seat;
static struct zwlr_foreign_toplevel_manager_v1 *manager;

static void on_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title)
{
    struct slot *s = data;
    pthread_mutex_lock(&lock);
    snprintf(s->title, sizeof s->title, "%s", title);
    pthread_mutex_unlock(&lock);
}

static void on_app(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *app)
{
    struct slot *s = data;
    pthread_mutex_lock(&lock);
    snprintf(s->app, sizeof s->app, "%s", app);
    pthread_mutex_unlock(&lock);
}

static void on_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *state)
{
    struct slot *s = data;
    uint32_t *v;
    int active = 0;

    wl_array_for_each(v, state)
        if (*v == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
            active = 1;
    pthread_mutex_lock(&lock);
    s->active = active;
    pthread_mutex_unlock(&lock);
}

static void on_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    struct slot *s = data;
    pthread_mutex_lock(&lock);
    memset(s, 0, sizeof *s);
    pthread_mutex_unlock(&lock);
    zwlr_foreign_toplevel_handle_v1_destroy(h);
}

static void on_output(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {}
static void on_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) {}
static void on_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                      struct zwlr_foreign_toplevel_handle_v1 *parent) {}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    .title = on_title, .app_id = on_app, .output_enter = on_output, .output_leave = on_output,
    .state = on_state, .done = on_done, .closed = on_closed, .parent = on_parent,
};

static void on_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *m,
                        struct zwlr_foreign_toplevel_handle_v1 *h)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX; i++)
        if (!slots[i].handle) {
            slots[i].handle = h;
            pthread_mutex_unlock(&lock);
            zwlr_foreign_toplevel_handle_v1_add_listener(h, &handle_listener, &slots[i]);
            return;
        }
    pthread_mutex_unlock(&lock);
    zwlr_foreign_toplevel_handle_v1_destroy(h);
}

static void on_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *m) {}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = on_toplevel, .finished = on_finished,
};

static void on_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
    if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
        manager = wl_registry_bind(reg, name, &zwlr_foreign_toplevel_manager_v1_interface,
                                   ver < 3 ? ver : 3);
        zwlr_foreign_toplevel_manager_v1_add_listener(manager, &manager_listener, NULL);
    } else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
        seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    }
}

static void on_global_remove(void *data, struct wl_registry *reg, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {on_global, on_global_remove};

static void *loop(void *arg)
{
    while (wl_display_dispatch(display) >= 0)
        ;
    return NULL;
}

void wins_start(void)
{
    pthread_t th;

    if (demo() || !getenv("WAYLAND_DISPLAY"))
        return;
    display = wl_display_connect(NULL);
    if (!display)
        return;
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!manager || pthread_create(&th, NULL, loop, NULL))
        return;
    pthread_detach(th);
}

/* everything that's open, minus lifeboat's own windows */
int wins_get(struct win *out, int max)
{
    int n = 0;

    if (demo()) {
        static const struct win fake[] = {
            {"Mozilla Firefox", "firefox", 0, 0}, {"notes.md - nano", "foot", 1, 0},
            {"holiday.mkv - mpv", "mpv", 2, 0},
        };
        memcpy(out, fake, sizeof fake);
        return 3;
    }
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX && n < max; i++) {
        if (!slots[i].handle || !strncmp(slots[i].app, "lifeboat-", 9))
            continue;
        snprintf(out[n].title, sizeof out[n].title, "%s", slots[i].title);
        snprintf(out[n].app, sizeof out[n].app, "%s", slots[i].app);
        out[n].id = i;
        out[n].active = slots[i].active;
        n++;
    }
    pthread_mutex_unlock(&lock);
    return n;
}

void wins_focus(int id)
{
    if (!display || !seat || id < 0 || id >= MAX)
        return;
    pthread_mutex_lock(&lock);
    if (slots[id].handle) {
        zwlr_foreign_toplevel_handle_v1_unset_minimized(slots[id].handle);
        zwlr_foreign_toplevel_handle_v1_activate(slots[id].handle, seat);
    }
    pthread_mutex_unlock(&lock);
    wl_display_flush(display);
}
