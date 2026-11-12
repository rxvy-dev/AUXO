/* Tries to get out of a sandbox, the way a compromised web page renderer would. Built and run
 * inside AuxoBSD by tests/test-sandbox.py:
 *   escape mkfilter  - write WebKit's seccomp filter (the calls its sandbox refuses) to stdout,
 *                      for bwrap --seccomp
 *   escape inside PORT [filtered]
 *                    - run in bubblewrap with every namespace: each way out must fail. Without
 *                      the filter it's root in its namespaces (as on Linux, root gets its
 *                      capabilities back on exec), so mounts work but stay inside; with it,
 *                      mount, unshare, chroot and pivot_root are refused outright
 *   escape seccomp   - install filters and check they're enforced, inherited and stacked
 * Prints "OK what" or "FAIL what: why" per check, then "DONE". */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <netinet/in.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>

static int bad;
static void expect(int ok, const char *what, const char *why)
{
    if (ok) printf("OK %s\n", what);
    else { printf("FAIL %s: %s\n", what, why); bad = 1; }
    fflush(stdout);
}

static void expect_errno(long r, int want, const char *what)
{
    char why[96];
    snprintf(why, sizeof why, "returned %ld, errno %d (%s), wanted %s", r, errno, strerror(errno), strerror(want));
    expect(r < 0 && errno == want, what, why);
}

static int inside(const char *outside_port, int filtered)
{
    struct utsname u;
    uname(&u);
    expect(!strcmp(u.nodename, "sbx"), "its own hostname", u.nodename);
    /* the host's files that weren't bound in are not there, even through ".." */
    expect_errno(open("/home/root/secret.txt", O_RDONLY), ENOENT, "a file that wasn't bound in can't be opened");
    expect_errno(open("/../../../home/root/secret.txt", O_RDONLY), ENOENT, "nor through /../..");
    if (chdir("/")) {}
    if (chdir("..")) {}
    char cwd[256] = "";
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    expect(!strcmp(cwd, "/"), "cd .. from / stays at /", cwd);
    /* read-only binds stay read-only */
    expect_errno(open("/usr/escape", O_CREAT | O_WRONLY, 0644), EROFS, "can't create files in a read-only bind");
    expect_errno(open("/usr/bin/busybox", O_WRONLY), EROFS, "can't write over a program in it");
    expect_errno(mkdir("/usr/escape", 0755), EROFS, "can't make directories in it");
    expect_errno(unlink("/usr/bin/busybox"), EROFS, "can't delete from it");
    expect_errno(truncate("/usr/bin/busybox", 0), EROFS, "can't truncate in it");
    /* the tmpfs is writable and private */
    int fd = open("/tmp/mine", O_CREAT | O_WRONLY, 0644);
    expect(fd >= 0, "the sandbox's own /tmp is writable", strerror(errno));
    if (fd >= 0) close(fd);
    if (filtered) {
        /* WebKit's filter: the ways to rearrange the namespaces are refused outright */
        expect_errno(mount("none", "/usr", 0, MS_REMOUNT | MS_BIND, 0), EPERM, "remounting /usr read-write is refused");
        expect_errno(mount("tmpfs", "/tmp", "tmpfs", 0, 0), EPERM, "mounting is refused");
        expect_errno(umount2("/usr", MNT_DETACH), EPERM, "unmounting a bind is refused");
        expect_errno(unshare(CLONE_NEWNS), EPERM, "new namespaces are refused");
        expect_errno(chroot("/tmp"), EPERM, "chroot is refused");
        expect_errno(syscall(SYS_pivot_root, "/tmp", "/tmp"), EPERM, "pivot_root is refused");
        expect_errno(syscall(435, 0, 0), ENOSYS, "clone3 is refused (so clone's flags can be checked)");
    } else {
        /* root in its own namespaces: what it rearranges stays inside them */
        expect(mount("tmpfs", "/mnt", "tmpfs", 0, 0) == 0, "root can mount a tmpfs, in its namespace", strerror(errno));
        expect(mount("proc", "/mnt", "proc", 0, 0) == 0, "and a procfs", strerror(errno));
        int k = 0;
        FILE *pc = popen("ls /mnt | grep -c '^[0-9]'", "r");
        if (pc) { if (fscanf(pc, "%d", &k) != 1) k = -1; pclose(pc); }
        char w2[64]; snprintf(w2, sizeof w2, "%d processes", k);
        expect(k > 0 && k <= 5, "which shows only the sandbox's processes", w2);
        pid_t c = fork();                                     /* the classic chroot escape, in a child */
        if (c == 0) {
            if (chroot("/tmp") != 0) _exit(2);
            for (int i = 0; i < 20; i++) if (chdir("..")) break;
            /* (as on Linux, chroot isn't a wall: this gets back to the sandbox's own root, and no further) */
            _exit(open("home/root/secret.txt", O_RDONLY) >= 0 ? 1 : open("esc/escape", O_RDONLY) >= 0 ? 0 : 3);
        }
        int st = 0;
        waitpid(c, &st, 0);
        expect(WIFEXITED(st) && WEXITSTATUS(st) == 0, "root can chroot, and climbing out of it stops at the sandbox's root",
               !WIFEXITED(st) ? "child died" : WEXITSTATUS(st) == 2 ? "chroot failed" : WEXITSTATUS(st) == 1 ? "reached the host's files" : "lost");
    }
    expect_errno(unshare(CLONE_NEWUSER), filtered ? EPERM : EINVAL, "no user namespaces");
    expect(sethostname("sbx2", 4) == 0 || errno == EPERM, "a hostname change stays in the sandbox (checked outside)", strerror(errno));
    /* its own PID namespace: it is 1 or 2, and can't see or signal anything outside */
    pid_t me = getpid();
    char why[64]; snprintf(why, sizeof why, "pid %d", me);
    expect(me <= 3, "a PID namespace of its own (low pid)", why);
    expect_errno(kill(400, 0), ESRCH, "processes outside can't be signalled");
    int n = 0;
    FILE *ps = popen("ls /proc | grep -c '^[0-9]'", "r");
    if (ps) { if (fscanf(ps, "%d", &n) != 1) n = -1; pclose(ps); }
    snprintf(why, sizeof why, "%d processes visible", n);
    expect(n > 0 && n <= 5, "/proc shows only the sandbox's processes", why);
    /* its own network: only lo */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)atoi(outside_port)) };
    inet_pton(AF_INET, "10.0.2.2", &a.sin_addr);
    expect_errno(connect(s, (struct sockaddr *)&a, sizeof a), ENETUNREACH, "the network outside is unreachable");
    close(s);
    if (!filtered) {                                          /* last: everything after this needs /usr */
        expect(umount2("/usr", MNT_DETACH) == 0, "root can unmount its own /usr bind", strerror(errno));
        expect_errno(open("/usr/bin/busybox", O_RDONLY), ENOENT, "and under it is the sandbox's own empty directory, not the host's");
    }
    return 0;
}

/* seccomp: BPF that answers by system call number */
static int filter(int nr, unsigned ret)
{
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)nr, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, ret),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog p = { sizeof f / sizeof f[0], f };
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &p);
}

static int seccomp(const char *self)
{
    expect(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0, "no_new_privs", strerror(errno));
    expect(filter(SYS_mkdir, SECCOMP_RET_ERRNO | EPERM) == 0, "install a filter", strerror(errno));
    expect(prctl(PR_GET_SECCOMP) == 2, "PR_GET_SECCOMP says filter mode", "");
    expect_errno(mkdir("/tmp/seccomp-test", 0755), EPERM, "the filtered call fails with the filter's errno");
    expect(getpid() > 0, "other calls still work", "");
    /* a second filter stacks: the stricter answer wins */
    expect(filter(SYS_mkdir, SECCOMP_RET_ERRNO | EACCES) == 0, "stack a second filter", strerror(errno));
    expect_errno(mkdir("/tmp/seccomp-test", 0755), EACCES, "two filters: the later errno is as strict and wins");
    /* inherited by children and kept across execve */
    pid_t c = fork();
    if (c == 0) { execl(self, self, "seccomp-child", (char *)0); _exit(99); }
    int st = 0;
    waitpid(c, &st, 0);
    expect(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a forked and exec'd child is filtered too", "child didn't see the filter");
    /* RET_KILL: the process dies of SIGSYS */
    c = fork();
    if (c == 0) {
        filter(SYS_uname, SECCOMP_RET_KILL_PROCESS);
        struct utsname u;
        uname(&u);
        _exit(0);
    }
    waitpid(c, &st, 0);
    expect(WIFSIGNALED(st) && WTERMSIG(st) == SIGSYS, "RET_KILL kills with SIGSYS", "the child lived");
    /* can't be removed */
    expect_errno(prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT), EINVAL, "filters can't be swapped for strict mode");
    return 0;
}

/* WebKit's sandbox filter (Source/WebKit/UIProcess/Launcher/glib/BubblewrapLauncher.cpp):
 * these calls fail with EPERM (clone3 and the new mount API with ENOSYS), the rest are allowed */
static int mkfilter(void)
{
    static const int eperm[] = { SYS_syslog, SYS_uselib, SYS_acct, SYS_modify_ldt, SYS_quotactl, SYS_add_key, SYS_keyctl,
        SYS_request_key, SYS_move_pages, SYS_mbind, SYS_get_mempolicy, SYS_set_mempolicy, SYS_migrate_pages, SYS_unshare,
        SYS_setns, SYS_mount, SYS_umount2, SYS_pivot_root, SYS_chroot, SYS_perf_event_open, SYS_personality, SYS_ptrace };
    static const int enosys[] = { 435, 428, 429, 430, 431, 432, 433, 442 };   /* clone3, open_tree … mount_setattr */
    struct sock_filter f[128];
    int n = 0;
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
    for (size_t i = 0; i < sizeof eperm / sizeof eperm[0]; i++) {
        f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)eperm[i], 0, 1);
        f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    }
    for (size_t i = 0; i < sizeof enosys / sizeof enosys[0]; i++) {
        f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)enosys[i], 0, 1);
        f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS);
    }
    /* clone with CLONE_NEWUSER */
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 3);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]));
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, CLONE_NEWUSER, 0, 1);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    return fwrite(f, sizeof f[0], (size_t)n, stdout) == (size_t)n ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "mkfilter")) return mkfilter();
    if (argc > 1 && !strcmp(argv[1], "seccomp-child")) return mkdir("/tmp/seccomp-test", 0755) < 0 && (errno == EACCES || errno == EPERM) ? 0 : 1;
    if (argc > 1 && !strcmp(argv[1], "inside")) inside(argc > 2 ? argv[2] : "80", argc > 3 && !strcmp(argv[3], "filtered"));
    else if (argc > 1 && !strcmp(argv[1], "seccomp")) seccomp(argv[0]);
    else { fprintf(stderr, "usage: escape inside PORT | seccomp\n"); return 2; }
    printf("DONE %s\n", bad ? "with failures" : "all held");
    return bad;
}
