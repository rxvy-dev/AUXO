/* The D-Bus session bus for the desktop's programs. GNOME programs expect one: GApplication
 * registers on it, libportal and WebKit's sandbox (its xdg-dbus-proxy) talk through it.
 * dbus-daemon comes with the full root image; it's started the first time a program wants
 * it and started again if it has gone. Without it, programs get "disabled:" as before. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "fs.h"
#include "vfs.h"
#include "uproc.h"

#define BUS_PATH "/run/user/0/bus"

static int alive(void)
{
    struct node *n;
    return fs_lookup(0, BUS_PATH, 1, &n) == 0 && n->type == N_SOCK && n->priv;
}

static void machine_id(void)
{
    if (vfs_exists("/etc/machine-id")) return;
    char id[34];
    uint64_t a = rdtsc() * 0x9E3779B97F4A7C15ULL, b = (a ^ (rdtsc() << 17)) * 0xBF58476D1CE4E5B9ULL;
    ksnprintf(id, sizeof id, "%016lx%016lx\n", (unsigned long)a, (unsigned long)b);
    vfs_write("/etc/machine-id", id, 33);
}

const char *session_bus_env(void)
{
    static const char *const off = "DBUS_SESSION_BUS_ADDRESS=disabled:";
    if (!vfs_exists("/usr/bin/dbus-daemon")) return off;
    if (alive()) return "DBUS_SESSION_BUS_ADDRESS=unix:path=" BUS_PATH;
    machine_id();
    struct node *run;
    if (fs_lookup(0, "/run/user/0", 1, &run) < 0) {
        struct node *r;
        if (fs_lookup(0, "/run", 1, &r) == 0) {
            struct node *u = fs_create(r, "user", N_DIR, 0755);
            if (u) { struct node *z = fs_create(u, "0", N_DIR, 0700); (void)z; }
        }
    }
    struct node *old;
    if (fs_lookup(0, BUS_PATH, 0, &old) == 0) vfs_remove(BUS_PATH);      /* left by one that died */
    char *argv[] = { "dbus-daemon", "--session", "--address=unix:path=" BUS_PATH, "--nofork", "--nopidfile", "--nosyslog", 0 };
    char *envp[] = { "PATH=/usr/bin:/usr/sbin", "HOME=/home/root", "USER=root", "XDG_RUNTIME_DIR=/run/user/0", "LANG=C.UTF-8", 0 };
    if (uproc_spawn_detached("/usr/bin/dbus-daemon", argv, envp, "/") < 0) return off;
    for (int i = 0; i < 300 && !alive(); i++) rs_sleep(1);                /* up to 3 s */
    return alive() ? "DBUS_SESSION_BUS_ADDRESS=unix:path=" BUS_PATH : off;
}
