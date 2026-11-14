/* A 32-bit (i386) program that checks the compat layer: built with gcc -m32 (static and
 * dynamic) by tests/test-sandbox.py and run on AuxoBSD. Each check prints "ok <name>" or
 * "FAIL <name>"; the last line is "compat32: N ok, M failed". */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int oks, fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
    if (ok) oks++; else fails++;
    fflush(stdout);
}

static __thread int tls_value = 7;
static void *thread_fn(void *arg)
{
    tls_value = (int)(long)arg;          /* each thread its own copy */
    usleep(20000);
    return (void *)(long)(tls_value * 2);
}

static volatile int got_sig, got_info_pid;
static void on_usr1(int s) { got_sig = s; }
static void on_usr2(int s, siginfo_t *si, void *uc) { (void)uc; got_sig = s; got_info_pid = si->si_pid; }

int main(int argc, char **argv)
{
    (void)argc;
    check(sizeof(void *) == 4, "pointers are 32 bits");
    check((unsigned long)&argc < 0xFFFFFFFFul && (unsigned long)argv > 0x80000000ul, "the stack is high in the 4 GB space");

    char *m = malloc(1 << 20);
    memset(m, 'x', 1 << 20);
    check(m && m[12345] == 'x', "malloc 1 MB");
    free(m);
    void *big = mmap(0, 64 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(big != MAP_FAILED && (unsigned long)big < 0xF7000000ul, "mmap 64 MB below 4 GB");
    if (big != MAP_FAILED) { ((char *)big)[(64 << 20) - 1] = 1; munmap(big, 64 << 20); }

    /* files */
    int fd = open("/tmp/c32.txt", O_CREAT | O_TRUNC | O_RDWR, 0644);
    struct iovec iov[2] = { { "hello ", 6 }, { "world\n", 6 } };
    check(fd >= 0 && writev(fd, iov, 2) == 12, "open + writev");
    struct stat st;
    check(fstat(fd, &st) == 0 && st.st_size == 12 && S_ISREG(st.st_mode), "fstat (stat64)");
    check(stat("/tmp/c32.txt", &st) == 0 && st.st_size == 12 && st.st_mtime > 1600000000, "stat with a sane mtime");
    check(lseek(fd, 6, SEEK_SET) == 6, "lseek");
    char buf[64] = { 0 };
    check(read(fd, buf, sizeof buf) == 6 && !strcmp(buf, "world\n"), "read");
    off_t o64 = lseek(fd, 0, SEEK_END);
    check(o64 == 12, "_llseek");
    char *map = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 0);
    check(map != MAP_FAILED && !memcmp(map, "hello", 5), "mmap a file");
    close(fd);
    check(access("/tmp/c32.txt", R_OK) == 0 && unlink("/tmp/c32.txt") == 0, "access + unlink");
    check(open("/nonexistent", O_RDONLY) < 0 && errno == ENOENT, "errno (thread-local) is ENOENT");

    DIR *d = opendir("/");
    int n = 0, has_tmp = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) { n++; if (!strcmp(e->d_name, "tmp")) has_tmp = 1; }
    if (d) closedir(d);
    check(n > 3 && has_tmp, "readdir / (getdents64)");

    char cwd[256];
    check(getcwd(cwd, sizeof cwd) != 0, "getcwd");
    struct utsname u;
    check(uname(&u) == 0 && !strcmp(u.sysname, "Linux"), "uname");

    /* time */
    time_t t0 = time(0);
    struct timespec ts;
    check(t0 > 1600000000 && clock_gettime(CLOCK_REALTIME, &ts) == 0 && ts.tv_sec >= t0, "time + clock_gettime");
    struct timeval tv;
    check(gettimeofday(&tv, 0) == 0 && tv.tv_sec >= t0, "gettimeofday");
    struct timespec a, b, nap = { 0, 50 * 1000 * 1000 };
    clock_gettime(CLOCK_MONOTONIC, &a);
    nanosleep(&nap, 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    check(ms >= 40 && ms < 2000, "nanosleep 50 ms");

    struct rlimit rl;
    check(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur > 0, "getrlimit");

    /* threads: TLS through %gs, clone, futex (pthread_join) */
    pthread_t th[4];
    int tok = 1;
    for (long i = 0; i < 4; i++) if (pthread_create(&th[i], 0, thread_fn, (void *)(i + 10))) tok = 0;
    for (long i = 0; i < 4; i++) { void *r; pthread_join(th[i], &r); if ((long)r != (i + 10) * 2) tok = 0; }
    check(tok && tls_value == 7, "4 threads, each with its own __thread variable");

    /* processes */
    int p[2];
    check(pipe(p) == 0, "pipe");
    pid_t c = fork();
    if (c == 0) { close(p[0]); write(p[1], "child", 5); _exit(42); }
    close(p[1]);
    memset(buf, 0, sizeof buf);
    int got = (int)read(p[0], buf, sizeof buf);
    int status = 0;
    check(c > 0 && waitpid(c, &status, 0) == c && WIFEXITED(status) && WEXITSTATUS(status) == 42 && got == 5, "fork, pipe, waitpid");
    close(p[0]);

    /* select on a pipe */
    pipe(p);
    write(p[1], "x", 1);
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(p[0], &rs);
    struct timeval to = { 1, 0 };
    check(select(p[0] + 1, &rs, 0, 0, &to) == 1 && FD_ISSET(p[0], &rs), "select");
    close(p[0]); close(p[1]);

    /* signals: plain and SA_SIGINFO handlers, then back to the interrupted code */
    signal(SIGUSR1, on_usr1);
    kill(getpid(), SIGUSR1);
    check(got_sig == SIGUSR1, "a signal handler runs and returns (sigreturn)");
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_usr2;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &sa, 0);
    got_sig = 0;
    kill(getpid(), SIGUSR2);
    check(got_sig == SIGUSR2 && got_info_pid == getpid(), "an SA_SIGINFO handler gets siginfo (rt_sigreturn)");
    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR1);
    got_sig = 0;
    sigprocmask(SIG_BLOCK, &blk, &old);
    kill(getpid(), SIGUSR1);
    int blocked = got_sig == 0;
    sigprocmask(SIG_SETMASK, &old, 0);
    check(blocked && got_sig == SIGUSR1, "sigprocmask blocks, then delivers");

    /* sockets: a file descriptor passed over a UNIX socket (sendmsg / recvmsg, SCM_RIGHTS) */
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    int pp[2];
    pipe(pp);
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    struct iovec one = { "F", 1 };
    mh.msg_iov = &one; mh.msg_iovlen = 1;
    mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &pp[1], sizeof(int));
    int sent = (int)sendmsg(sv[0], &mh, 0);
    char rb[4];
    char rcbuf[CMSG_SPACE(sizeof(int))];
    struct iovec rio = { rb, sizeof rb };
    struct msghdr rh;
    memset(&rh, 0, sizeof rh);
    rh.msg_iov = &rio; rh.msg_iovlen = 1;
    rh.msg_control = rcbuf; rh.msg_controllen = sizeof rcbuf;
    int rcv = (int)recvmsg(sv[1], &rh, 0);
    struct cmsghdr *rc = CMSG_FIRSTHDR(&rh);
    int nfd = -1;
    if (rc && rc->cmsg_type == SCM_RIGHTS) memcpy(&nfd, CMSG_DATA(rc), sizeof(int));
    int pass_ok = sent == 1 && rcv == 1 && nfd >= 0 && write(nfd, "Q", 1) == 1;
    char q = 0;
    if (pass_ok) pass_ok = read(pp[0], &q, 1) == 1 && q == 'Q';
    check(pass_ok, "a file descriptor passed with sendmsg / recvmsg");

    /* exec: a 64-bit program from this 32-bit one */
    c = fork();
    if (c == 0) {
        int nul = open("/dev/null", O_WRONLY);
        dup2(nul, 1);
        char *av[] = { "busybox", "true", 0 };
        execv("/bin/busybox", av);
        _exit(99);
    }
    waitpid(c, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "execve a 64-bit program from a 32-bit one");

    printf("compat32: %d ok, %d failed\n", oks, fails);
    return fails ? 1 : 0;
}
