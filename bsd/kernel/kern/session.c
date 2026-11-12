/* The desktop session: the environment programs started from the Terminal and the desktop
 * get, and the D-Bus session bus. GNOME programs expect a bus: GApplication registers on it,
 * libportal and WebKit's sandbox (its xdg-dbus-proxy) talk through it. dbus-daemon comes with
 * the full root image; it runs as the session's user in their runtime directory
 * (/run/user/UID), is started the first time a program wants it and again if it has gone.
 * Without it, programs get "disabled:" as before. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "fs.h"
#include "vfs.h"
#include "uproc.h"
#include "cred.h"
#include "session.h"

static char bus_path[48], bus_env[96];

static int alive(void)
{
    struct node *n;
    return bus_path[0] && fs_lookup(0, bus_path, 1, &n) == 0 && n->type == N_SOCK && n->priv;
}

static void machine_id(void)
{
    if (vfs_exists("/etc/machine-id")) return;
    char id[34];
    uint64_t a = rdtsc() * 0x9E3779B97F4A7C15ULL, b = (a ^ (rdtsc() << 17)) * 0xBF58476D1CE4E5B9ULL;
    ksnprintf(id, sizeof id, "%016lx%016lx\n", (unsigned long)a, (unsigned long)b);
    vfs_write("/etc/machine-id", id, 33);
}

const char *session_runtime_dir(void)
{
    static char d[32];
    ksnprintf(d, sizeof d, "/run/user/%u", session_uid());
    return d;
}

const char *session_bus_env(void)
{
    static const char *const off = "DBUS_SESSION_BUS_ADDRESS=disabled:";
    if (!vfs_exists("/usr/bin/dbus-daemon")) return off;
    ksnprintf(bus_path, sizeof bus_path, "%s/bus", session_runtime_dir());
    ksnprintf(bus_env, sizeof bus_env, "DBUS_SESSION_BUS_ADDRESS=unix:path=%s", bus_path);
    if (alive()) return bus_env;
    machine_id();
    struct node *old;
    if (fs_lookup(0, bus_path, 0, &old) == 0) vfs_remove(bus_path);    /* left by one that died */
    char addr[80], home[112], user[48], rt[48];
    ksnprintf(addr, sizeof addr, "--address=unix:path=%s", bus_path);
    ksnprintf(home, sizeof home, "HOME=%s", session_home());
    ksnprintf(user, sizeof user, "USER=%s", session_user());
    ksnprintf(rt, sizeof rt, "XDG_RUNTIME_DIR=%s", session_runtime_dir());
    char *argv[] = { "dbus-daemon", "--session", addr, "--nofork", "--nopidfile", "--nosyslog", 0 };
    char *envp[] = { "PATH=/usr/bin:/usr/sbin", home, user, rt, "LANG=C.UTF-8", 0 };
    if (uproc_spawn_as("/usr/bin/dbus-daemon", argv, envp, "/", 1, 0) < 0) return off;
    for (int i = 0; i < 300 && !alive(); i++) rs_sleep(1);                /* up to 3 s */
    return alive() ? bus_env : off;
}

/* The environment of a program started from the Terminal or the desktop: who it runs as
 * (root for `sudo`), where the desktop (Wayland) and the session bus are, and the settings
 * that make GTK draw in software. Strings go into buf; env gets the pointers, 0-terminated. */
int session_env(char *buf, size_t n, char **env, int max, int as_root, const char *pwd)
{
    size_t o = 0;
    int k = 0;
#define PUT(...) do { if (k < max - 1 && o < n) { int l_ = ksnprintf(buf + o, n - o, __VA_ARGS__); env[k++] = buf + o; o += (size_t)l_ + 1; } } while (0)
    int root = as_root || session_uid() == 0;
    PUT("PATH=%s", root ? "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" : "/usr/local/bin:/usr/bin:/bin:/usr/sbin");
    PUT("HOME=%s", root && as_root ? "/root" : session_home());
    PUT("USER=%s", root && as_root ? "root" : session_user());
    PUT("LOGNAME=%s", root && as_root ? "root" : session_user());
    PUT("SHELL=/bin/sh");
    PUT("TERM=linux");
    PUT("LANG=C.UTF-8");
    PUT("TMPDIR=/tmp");
    if (pwd) PUT("PWD=%s", pwd);
    PUT("XDG_RUNTIME_DIR=%s", session_runtime_dir());
    PUT("WAYLAND_DISPLAY=wayland-0");
    PUT("GDK_BACKEND=wayland");
    PUT("GSK_RENDERER=cairo");
    PUT("GDK_DEBUG=gl-disable,vulkan-disable");
    PUT("LIBGL_ALWAYS_SOFTWARE=1");
    PUT("GTK_A11Y=none");
    PUT("NO_AT_BRIDGE=1");
    PUT("%s", session_bus_env());
    PUT("XDG_DATA_DIRS=/usr/local/share:/usr/share");
    if (as_root) { PUT("SUDO_USER=%s", session_user()); PUT("SUDO_UID=%u", session_uid()); PUT("SUDO_GID=%u", session_gid()); }
#undef PUT
    env[k] = 0;
    return k;
}
