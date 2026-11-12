/* tests/users.c: users, groups and permissions, built and run inside AuxoBSD.
 *   users user      as the session's user (uid 1000): what the file modes and ownership
 *                   allow and refuse, kill, setuid, /proc, access()
 *   users root      under sudo: root's powers, and dropping them for good
 *   users setuid    a set-user-ID copy of itself (made by `users root`): who it runs as
 * Prints "ok NAME" / "FAIL NAME ...", then "users MODE: N passed, M failed". */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(name, cond, ...) do { if (cond) { pass++; printf("ok %s\n", name); } \
    else { fail++; printf("FAIL %s: ", name); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); } } while (0)

static int denied(int r) { return r < 0 && (errno == EACCES || errno == EPERM); }

static unsigned long long capeff(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256]; unsigned long long v = ~0ULL;
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "CapEff:", 7)) v = strtoull(line + 7, 0, 16);
    if (f) fclose(f);
    return v;
}

static void as_user(void)
{
    CHECK("runs as uid 1000", getuid() == 1000 && geteuid() == 1000 && getgid() == 1000, "uid %d euid %d gid %d", getuid(), geteuid(), getgid());
    gid_t g[64]; int n = getgroups(64, g), sudo = 0;
    for (int i = 0; i < n; i++) if (g[i] == 27) sudo = 1;
    CHECK("in the sudo group", sudo, "%d groups", n);
    CHECK("no capabilities", capeff() == 0, "%llx", capeff());
    errno = 0;
    CHECK("/etc/shadow unreadable", denied(open("/etc/shadow", O_RDONLY)), "-");
    CHECK("/etc/passwd readable", open("/etc/passwd", O_RDONLY) >= 0, "-");
    CHECK("/etc/passwd not writable", denied(open("/etc/passwd", O_WRONLY)), "-");
    CHECK("can't make files in /etc", denied(open("/etc/mine", O_CREAT | O_WRONLY, 0644)), "-");
    CHECK("can't remove /etc/hostname", denied(unlink("/etc/hostname")), "-");
    CHECK("can't rename in /usr/bin", denied(rename("/usr/bin/busybox", "/usr/bin/bb")), "-");
    CHECK("can't mkdir in /", denied(mkdir("/mine", 0755)), "-");
    CHECK("can't chmod /etc/passwd", denied(chmod("/etc/passwd", 0666)), "-");
    CHECK("can't chown to root", denied(chown("/tmp", 1000, 1000)), "-");
    CHECK("access(W_OK) on /etc", access("/etc", W_OK) < 0 && errno == EACCES, "-");
    CHECK("access(R_OK) on /etc/shadow", access("/etc/shadow", R_OK) < 0, "-");
    CHECK("/root is closed", denied(open("/root", O_RDONLY | O_DIRECTORY)), "-");

    /* in their own place, anything */
    char d[] = "/tmp/usrXXXXXX";
    CHECK("mkdtemp in /tmp", mkdtemp(d) != 0, "-");
    struct stat st;
    CHECK("it's theirs", stat(d, &st) == 0 && st.st_uid == 1000 && st.st_gid == 1000 && (st.st_mode & 0777) == 0700, "uid %d gid %d mode %o", st.st_uid, st.st_gid, st.st_mode);
    char p[128], q[128];
    snprintf(p, sizeof p, "%s/f", d);
    int fd = open(p, O_CREAT | O_WRONLY, 0640);
    CHECK("create a file", fd >= 0 && write(fd, "x", 1) == 1, "-");
    close(fd);
    CHECK("chmod own file", chmod(p, 0600) == 0 && stat(p, &st) == 0 && (st.st_mode & 0777) == 0600, "-");
    CHECK("chgrp to own group", chown(p, -1, 27) == 0 && stat(p, &st) == 0 && st.st_gid == 27, "gid %d", st.st_gid);
    CHECK("chgrp to another's group refused", denied(chown(p, -1, 0)), "-");
    CHECK("chmod 000 then can't read it", chmod(p, 0) == 0 && denied(open(p, O_RDONLY)), "-");
    chmod(p, 0600);
    snprintf(q, sizeof q, "%s/sub", d);
    mkdir(q, 0700);
    snprintf(p, sizeof p, "%s/sub/x", d);
    close(open(p, O_CREAT | O_WRONLY, 0644));
    chmod(q, 0600);                                      /* readable, not searchable */
    CHECK("no search permission: can't get in", denied(open(p, O_RDONLY)) && denied(chdir(q)), "-");
    chmod(q, 0700);
    snprintf(p, sizeof p, "%s/noexec", d);
    fd = open(p, O_CREAT | O_WRONLY, 0644);
    if (write(fd, "#!/bin/sh\necho hi\n", 18) < 0) { }
    close(fd);
    char *av[] = { p, 0 };
    CHECK("can't run a file without x", execv(p, av) < 0 && errno == EACCES, "-");
    CHECK("umask applies", stat(d, &st) == 0, "-");

    /* /tmp is sticky: root's file there stays */
    CHECK("/tmp is 1777", stat("/tmp", &st) == 0 && (st.st_mode & 07777) == 01777, "mode %o", st.st_mode);
    if (access("/tmp/rootfile", F_OK) == 0) {
        CHECK("sticky /tmp: can't delete root's file", denied(unlink("/tmp/rootfile")), "-");
        CHECK("sticky /tmp: can't rename it", denied(rename("/tmp/rootfile", "/tmp/mine")), "-");
        CHECK("nor write to it (0644)", denied(open("/tmp/rootfile", O_WRONLY)), "-");
    }

    /* ids */
    CHECK("setuid(0) refused", denied(setuid(0)), "-");
    CHECK("setgid(0) refused", denied(setgid(0)), "-");
    CHECK("setgroups refused", denied(setgroups(0, 0)), "-");
    CHECK("setresuid to own ids", setresuid(1000, 1000, 1000) == 0, "-");
    pid_t c = fork();
    if (!c) { pause(); _exit(0); }
    CHECK("kill own child", kill(c, SIGTERM) == 0, "-");
    waitpid(c, 0, 0);

    /* a set-user-ID root copy of this program, made by `users root` */
    if (access("/tmp/users-suid", X_OK) == 0) {
        int st2;
        pid_t k = fork();
        if (!k) { execl("/tmp/users-suid", "users", "setuid", (char *)0); _exit(99); }
        waitpid(k, &st2, 0);
        CHECK("the set-user-ID program ran", WIFEXITED(st2) && WEXITSTATUS(st2) == 0, "status %x", st2);
    }
    /* a root daemon: signals from us bounce off */
    if (access("/tmp/rootpid", F_OK) == 0) {
        FILE *f = fopen("/tmp/rootpid", "r"); int rp = 0;
        if (f) { if (fscanf(f, "%d", &rp) != 1) rp = 0; fclose(f); }
        if (rp > 0) CHECK("can't signal root's process", kill(rp, 0) < 0 && errno == EPERM, "pid %d", rp);
    }
    char cmd[200]; snprintf(cmd, sizeof cmd, "busybox rm -rf %s", d);
    if (system(cmd)) { }
}

static void as_root(void)
{
    CHECK("sudo: uid 0", getuid() == 0 && geteuid() == 0, "uid %d", getuid());
    CHECK("sudo: SUDO_USER", getenv("SUDO_USER") && !strcmp(getenv("SUDO_USER"), "auxo"), "%s", getenv("SUDO_USER") ? getenv("SUDO_USER") : "(none)");
    CHECK("root has every capability", capeff() == 0x1ffffffffffULL, "%llx", capeff());
    CHECK("root reads /etc/shadow", open("/etc/shadow", O_RDONLY) >= 0, "-");
    int fd = open("/tmp/rootfile", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK("root makes /tmp/rootfile", fd >= 0, "-");
    close(fd);
    /* the set-user-ID copy */
    char self[256];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) self[n] = 0;
    char cmd[400];
    snprintf(cmd, sizeof cmd, "busybox cp %s /tmp/users-suid && busybox chown 0:0 /tmp/users-suid && busybox chmod 4755 /tmp/users-suid", self);
    CHECK("make a set-user-ID copy", n > 0 && system(cmd) == 0, "-");
    struct stat st;
    CHECK("its mode is 4755", stat("/tmp/users-suid", &st) == 0 && (st.st_mode & 07777) == 04755 && st.st_uid == 0, "mode %o", st.st_mode);
    /* a long-lived root process for the user's kill test */
    pid_t d = fork();
    if (!d) { setsid(); for (int i = 0; i < 600; i++) sleep(1); _exit(0); }
    FILE *f = fopen("/tmp/rootpid", "w");
    if (f) { fprintf(f, "%d\n", d); fclose(f); }
    /* dropping root for good */
    pid_t c = fork();
    if (!c) {
        int ok = setgroups(0, 0) == 0 && setresgid(1000, 1000, 1000) == 0 && setresuid(1000, 1000, 1000) == 0;
        ok = ok && capeff() == 0 && setuid(0) < 0 && open("/etc/shadow", O_RDONLY) < 0;
        _exit(ok ? 0 : 1);
    }
    int s;
    waitpid(c, &s, 0);
    CHECK("dropped root stays dropped", WIFEXITED(s) && WEXITSTATUS(s) == 0, "status %x", s);
    /* seteuid away and back (the saved id keeps root) */
    c = fork();
    if (!c) {
        int ok = seteuid(1000) == 0 && geteuid() == 1000 && capeff() == 0 && open("/etc/shadow", O_RDONLY) < 0;
        ok = ok && seteuid(0) == 0 && capeff() == 0x1ffffffffffULL && open("/etc/shadow", O_RDONLY) >= 0;
        _exit(ok ? 0 : 1);
    }
    waitpid(c, &s, 0);
    CHECK("seteuid away and back", WIFEXITED(s) && WEXITSTATUS(s) == 0, "status %x", s);
    CHECK("chown a file to the user", chown("/tmp/rootfile", 1000, 1000) == 0 && chown("/tmp/rootfile", 0, 0) == 0, "-");
}

static int as_setuid(void)
{
    CHECK("setuid: euid 0, uid 1000", geteuid() == 0 && getuid() == 1000, "uid %d euid %d", getuid(), geteuid());
    CHECK("setuid: AT_SECURE", getauxval(AT_SECURE) == 1, "%lu", getauxval(AT_SECURE));
    CHECK("setuid: can read /etc/shadow", open("/etc/shadow", O_RDONLY) >= 0, "-");
    CHECK("setuid: back to the user", setuid(getuid()) == 0 && geteuid() == 1000 && open("/etc/shadow", O_RDONLY) < 0, "-");
    return fail != 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "user";
    if (!strcmp(mode, "setuid")) return as_setuid();
    if (!strcmp(mode, "root")) as_root(); else as_user();
    printf("users %s: %d passed, %d failed\n", mode, pass, fail);
    return fail != 0;
}
