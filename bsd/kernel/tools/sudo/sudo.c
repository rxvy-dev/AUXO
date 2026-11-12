/* sudo: run a command as root, for members of the sudo (or admin) group, after they type
 * their own password. A small set-user-ID program for AuxoBSD (installed /usr/bin/sudo, mode
 * 4755): the Terminal has `sudo` built in; this one is for scripts and other shells.
 *
 *   sudo COMMAND [ARGS]     run it as root
 *   sudo -s | -i            a root shell
 *   sudo -k                 forget the password (ask next time)
 *   sudo -v                 ask now, run nothing
 *   sudo -n ...             never ask: fail if a password would be needed
 *
 * A right password is remembered for 5 minutes (a timestamp in /run/sudo/ts, per user).
 * Accounts with no password (a live system's) aren't asked. Passwords are checked against
 * /etc/shadow with crypt(3). The environment is reset the way sudo's env_reset does. */
#define _GNU_SOURCE
#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TS_DIR "/run/sudo/ts"
#define TS_SECONDS 300

static void die(const char *m) { fprintf(stderr, "sudo: %s\n", m); exit(1); }

static int in_admin_group(const struct passwd *pw)
{
    gid_t groups[64];
    int n = 64;
    if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &n) < 0) n = 0;
    for (int i = 0; i < n; i++) {
        struct group *g = getgrgid(groups[i]);
        if (g && (!strcmp(g->gr_name, "sudo") || !strcmp(g->gr_name, "admin") || !strcmp(g->gr_name, "wheel"))) return 1;
    }
    return 0;
}

static char *read_password(const char *prompt)
{
    static char buf[256];
    int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (fd < 0) fd = 0;
    struct termios old, quiet;
    int have = tcgetattr(fd, &old) == 0;
    if (have) { quiet = old; quiet.c_lflag &= ~(tcflag_t)(ECHO | ECHONL); tcsetattr(fd, TCSAFLUSH, &quiet); }
    if (write(fd == 0 ? 2 : fd, prompt, strlen(prompt)) < 0) { }
    size_t n = 0;
    char c;
    while (read(fd, &c, 1) == 1 && c != '\n' && c != '\r') if (n + 1 < sizeof buf) buf[n++] = c;
    buf[n] = 0;
    if (have) tcsetattr(fd, TCSAFLUSH, &old);
    if (write(fd == 0 ? 2 : fd, "\n", 1) < 0) { }
    if (fd > 2) close(fd);
    return buf;
}

static void ts_path(uid_t uid, char *p, size_t n) { snprintf(p, n, TS_DIR "/%u", (unsigned)uid); }

static int ts_fresh(uid_t uid)
{
    char p[64]; struct stat st;
    ts_path(uid, p, sizeof p);
    return stat(p, &st) == 0 && st.st_uid == 0 && time(0) - st.st_mtime < TS_SECONDS && time(0) >= st.st_mtime;
}

static void ts_touch(uid_t uid)
{
    mkdir("/run/sudo", 0711); mkdir(TS_DIR, 0700);
    char p[64];
    ts_path(uid, p, sizeof p);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) close(fd);
    utimensat(AT_FDCWD, p, 0, 0);
}

static void ts_forget(uid_t uid) { char p[64]; ts_path(uid, p, sizeof p); unlink(p); }

int main(int argc, char **argv)
{
    int shell = 0, validate = 0, noninteractive = 0, a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "--")) { a++; break; }
        for (const char *o = argv[a] + 1; *o; o++) {
            if (*o == 's' || *o == 'i') shell = 1;
            else if (*o == 'k' || *o == 'K') { if (geteuid() == 0) ts_forget(getuid()); if (a + 1 >= argc) return 0; }
            else if (*o == 'v') validate = 1;
            else if (*o == 'n') noninteractive = 1;
            else if (*o == 'E' || *o == 'H') { }
            else if (*o == 'h') { puts("usage: sudo [-n] COMMAND [ARGS] | sudo -s | sudo -k | sudo -v"); return 0; }
            else { fprintf(stderr, "sudo: -%c isn't supported here\n", *o); return 1; }
        }
    }
    if (a >= argc && !shell && !validate) die("usage: sudo COMMAND [ARGS]   (or sudo -s for a root shell)");
    if (geteuid() != 0) die("must be owned by root and have the set-user-ID bit (chmod 4755 /usr/bin/sudo)");

    uid_t uid = getuid();
    struct passwd *pw = getpwuid(uid);
    if (!pw) die("you don't have an account in /etc/passwd");
    char user[64];
    snprintf(user, sizeof user, "%s", pw->pw_name);
    if (uid != 0) {
        if (!in_admin_group(pw)) { fprintf(stderr, "sudo: %s isn't in the sudoers file (the sudo group). This incident will not be reported.\n", user); return 1; }
        struct spwd *sp = getspnam(user);
        const char *hash = sp ? sp->sp_pwdp : pw->pw_passwd;
        if (hash && hash[0] && !ts_fresh(uid)) {
            if (hash[0] == '!' || hash[0] == '*') die("your account is locked");
            if (noninteractive) die("a password is required");
            char prompt[128];
            snprintf(prompt, sizeof prompt, "[sudo] password for %s: ", user);
            int ok = 0;
            for (int t = 0; t < 3 && !ok; t++) {
                char *typed = read_password(prompt);
                const char *h = crypt(typed, hash);
                ok = h && !strcmp(h, hash);
                memset(typed, 0, strlen(typed));
                if (!ok) fprintf(stderr, "Sorry, try again.\n");
            }
            if (!ok) { fprintf(stderr, "sudo: 3 incorrect password attempts\n"); return 1; }
        }
        ts_touch(uid);
    }
    if (validate && a >= argc) return 0;

    /* become root, all of it */
    if (setgroups(0, 0) < 0 || setgid(0) < 0 || setuid(0) < 0) die("couldn't switch to root");

    const char *term = getenv("TERM"), *disp = getenv("WAYLAND_DISPLAY"), *rt = getenv("XDG_RUNTIME_DIR"),
               *lang = getenv("LANG"), *bus = getenv("DBUS_SESSION_BUS_ADDRESS");
    char *keep[5] = { term ? strdup(term) : 0, disp ? strdup(disp) : 0, rt ? strdup(rt) : 0, lang ? strdup(lang) : 0, bus ? strdup(bus) : 0 };
    clearenv();
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("HOME", "/root", 1); setenv("USER", "root", 1); setenv("LOGNAME", "root", 1); setenv("SHELL", "/bin/sh", 1);
    setenv("MAIL", "/var/mail/root", 1);
    if (keep[0]) setenv("TERM", keep[0], 1);
    if (keep[1]) setenv("WAYLAND_DISPLAY", keep[1], 1);
    if (keep[2]) setenv("XDG_RUNTIME_DIR", keep[2], 1);
    setenv("LANG", keep[3] ? keep[3] : "C.UTF-8", 1);
    if (keep[4]) setenv("DBUS_SESSION_BUS_ADDRESS", keep[4], 1);
    char num[16];
    setenv("SUDO_USER", user, 1);
    snprintf(num, sizeof num, "%u", (unsigned)uid); setenv("SUDO_UID", num, 1);
    snprintf(num, sizeof num, "%u", (unsigned)pw->pw_gid); setenv("SUDO_GID", num, 1);
    signal(SIGINT, SIG_DFL); signal(SIGQUIT, SIG_DFL);
    if (shell || a >= argc) {
        if (shell) { if (chdir("/root") < 0) { } }
        char *sh[] = { "sh", 0 };
        execv("/bin/sh", sh);
        perror("sudo: /bin/sh");
        return 127;
    }
    execvp(argv[a], argv + a);
    fprintf(stderr, "sudo: %s: %s\n", argv[a], errno == ENOENT ? "command not found" : strerror(errno));
    return errno == ENOENT ? 127 : 126;
}
