/* tests/syscalls.c: the Linux calls everyday desktop programs lean on
 * (inotify, timerfd, POSIX timers, pidfd + waitid, sigtimedwait/sigqueue,
 * sendfile/copy_file_range, preadv/pwritev, mknod FIFOs, utimes, sched_*).
 * Built and run inside AuxoBSD; prints "ok NAME" / "FAIL NAME ..." and a
 * final "syscalls: N passed, M failed". */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(name, cond, ...) do { if (cond) { pass++; printf("ok %s\n", name); } \
    else { fail++; printf("FAIL %s: ", name); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); } } while (0)

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static volatile int got_sig, got_val;
static void on_timer(int s, siginfo_t *si, void *u) { (void)u; got_sig = s; got_val = si->si_value.sival_int + (si->si_code == SI_TIMER ? 1000 : 0); }

int main(void)
{
    char dir[] = "/tmp/sctestXXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char p[256], q[256];

    /* inotify: create, modify, close-write, rename, delete in a watched directory */
    {
        int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        CHECK("inotify_init1", fd >= 0, "fd %d", fd);
        int wd = inotify_add_watch(fd, dir, IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE);
        CHECK("inotify_add_watch", wd > 0, "wd %d", wd);
        snprintf(p, sizeof p, "%s/a", dir); snprintf(q, sizeof q, "%s/b", dir);
        int f = open(p, O_CREAT | O_WRONLY, 0644); write(f, "hi", 2); close(f);
        rename(p, q); unlink(q);
        struct pollfd pf = { fd, POLLIN, 0 };
        CHECK("inotify poll", poll(&pf, 1, 1000) == 1 && (pf.revents & POLLIN), "revents %x", pf.revents);
        char buf[4096]; unsigned seen = 0; uint32_t cookie_from = 0, cookie_to = 1; int named = 1;
        ssize_t n = read(fd, buf, sizeof buf);
        for (char *c = buf; n > 0 && c < buf + n; ) {
            struct inotify_event *e = (struct inotify_event *)c;
            seen |= e->mask;
            if (e->mask & IN_MOVED_FROM) cookie_from = e->cookie;
            if (e->mask & IN_MOVED_TO) cookie_to = e->cookie;
            if (e->len && strcmp(e->name, "a") && strcmp(e->name, "b")) named = 0;
            c += sizeof *e + e->len;
        }
        unsigned want = IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE;
        CHECK("inotify events", (seen & want) == want && named, "seen %x want %x", seen, want);
        CHECK("inotify rename cookie", cookie_from && cookie_from == cookie_to, "%u %u", cookie_from, cookie_to);
        CHECK("inotify_rm_watch", inotify_rm_watch(fd, wd) == 0, "-");
        n = read(fd, buf, sizeof buf);   /* IN_IGNORED */
        CHECK("inotify IN_IGNORED", n > 0 && (((struct inotify_event *)buf)->mask & IN_IGNORED), "n %zd", n);
        close(fd);
    }

    /* timerfd: a periodic timer counted through read, and poll waking on it */
    {
        int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        CHECK("timerfd_create", fd >= 0, "fd %d", fd);
        struct itimerspec its = { { 0, 50000000 }, { 0, 50000000 } };
        double t0 = now();
        CHECK("timerfd_settime", timerfd_settime(fd, 0, &its, 0) == 0, "-");
        uint64_t ex = 0;
        ssize_t n = read(fd, &ex, 8);
        double dt = now() - t0;
        CHECK("timerfd read blocks", n == 8 && ex >= 1 && dt > 0.03 && dt < 0.5, "n %zd ex %llu dt %.3f", n, (unsigned long long)ex, dt);
        usleep(260000);
        n = read(fd, &ex, 8);
        CHECK("timerfd periodic count", n == 8 && ex >= 4 && ex <= 7, "ex %llu", (unsigned long long)ex);
        struct itimerspec cur;
        CHECK("timerfd_gettime", timerfd_gettime(fd, &cur) == 0 && cur.it_interval.tv_nsec == 50000000, "-");
        close(fd);
        /* absolute realtime deadline */
        fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
        struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
        rt.tv_nsec += 100000000; if (rt.tv_nsec >= 1000000000) { rt.tv_sec++; rt.tv_nsec -= 1000000000; }
        struct itimerspec abs = { { 0, 0 }, rt };
        timerfd_settime(fd, TFD_TIMER_ABSTIME, &abs, 0);
        n = read(fd, &ex, 8);
        CHECK("timerfd nonblock EAGAIN", n < 0 && errno == EAGAIN, "n %zd", n);
        struct pollfd pf = { fd, POLLIN, 0 };
        t0 = now();
        int pr = poll(&pf, 1, 2000);
        dt = now() - t0;
        CHECK("timerfd abstime poll", pr == 1 && dt > 0.04 && dt < 0.6, "pr %d dt %.3f", pr, dt);
        close(fd);
    }

    /* POSIX timers delivering a signal with its value */
    {
        struct sigaction sa = { 0 };
        sa.sa_sigaction = on_timer; sa.sa_flags = SA_SIGINFO;
        sigaction(SIGRTMIN, &sa, 0);
        struct sigevent sev = { 0 };
        sev.sigev_notify = SIGEV_SIGNAL; sev.sigev_signo = SIGRTMIN; sev.sigev_value.sival_int = 42;
        timer_t tm;
        CHECK("timer_create", timer_create(CLOCK_MONOTONIC, &sev, &tm) == 0, "-");
        struct itimerspec its = { { 0, 0 }, { 0, 80000000 } };
        CHECK("timer_settime", timer_settime(tm, 0, &its, 0) == 0, "-");
        double t0 = now();
        while (!got_sig && now() - t0 < 2) usleep(5000);
        CHECK("timer signal + si_value", got_sig == SIGRTMIN && got_val == 1042, "sig %d val %d", got_sig, got_val);
        CHECK("timer_delete", timer_delete(tm) == 0, "-");
    }

    /* sigtimedwait and sigqueue */
    {
        sigset_t s; sigemptyset(&s); sigaddset(&s, SIGUSR1);
        sigprocmask(SIG_BLOCK, &s, 0);
        union sigval v = { .sival_int = 7 };
        CHECK("sigqueue", sigqueue(getpid(), SIGUSR1, v) == 0, "-");
        siginfo_t si; struct timespec to = { 1, 0 };
        int r = sigtimedwait(&s, &si, &to);
        CHECK("sigtimedwait value", r == SIGUSR1 && si.si_code == SI_QUEUE && si.si_value.sival_int == 7, "r %d code %d val %d", r, si.si_code, si.si_value.sival_int);
        struct timespec sh = { 0, 100000000 };
        double t0 = now();
        r = sigtimedwait(&s, &si, &sh);
        CHECK("sigtimedwait timeout", r < 0 && errno == EAGAIN && now() - t0 > 0.05, "r %d", r);
    }

    /* pidfd_open + poll + waitid(P_PIDFD), the way GLib watches children */
    {
        pid_t c = fork();
        if (!c) { usleep(100000); _exit(3); }
        int pfd = syscall(SYS_pidfd_open, c, 0);
        CHECK("pidfd_open", pfd >= 0, "pfd %d", pfd);
        struct pollfd pf = { pfd, POLLIN, 0 };
        CHECK("pidfd not ready while running", poll(&pf, 1, 0) == 0, "-");
        CHECK("pidfd ready at exit", poll(&pf, 1, 3000) == 1, "-");
        siginfo_t si; memset(&si, 0, sizeof si);
        int r = waitid(P_PIDFD, pfd, &si, WEXITED | WNOWAIT);
        CHECK("waitid P_PIDFD WNOWAIT", r == 0 && si.si_pid == c && si.si_code == CLD_EXITED && si.si_status == 3, "r %d pid %d code %d st %d", r, si.si_pid, si.si_code, si.si_status);
        int st;
        CHECK("still reapable after WNOWAIT", waitpid(c, &st, 0) == c && WEXITSTATUS(st) == 3, "-");
        close(pfd);
        c = fork();
        if (!c) { pause(); _exit(0); }
        pfd = syscall(SYS_pidfd_open, c, 0);
        usleep(50000);
        CHECK("pidfd_send_signal", syscall(SYS_pidfd_send_signal, pfd, SIGTERM, 0, 0) == 0, "-");
        memset(&si, 0, sizeof si);
        r = waitid(P_PID, c, &si, WEXITED);
        CHECK("waitid killed", r == 0 && si.si_code == CLD_KILLED && si.si_status == SIGTERM, "r %d code %d st %d", r, si.si_code, si.si_status);
        close(pfd);
        r = waitid(P_ALL, 0, &si, WEXITED | WNOHANG);
        CHECK("waitid no children", r < 0 && errno == ECHILD, "r %d", r);
    }

    /* sendfile, copy_file_range, preadv/pwritev */
    {
        snprintf(p, sizeof p, "%s/src", dir); snprintf(q, sizeof q, "%s/dst", dir);
        int a = open(p, O_CREAT | O_RDWR, 0644);
        char big[10000]; for (int i = 0; i < (int)sizeof big; i++) big[i] = 'a' + i % 26;
        write(a, big, sizeof big);
        int b = open(q, O_CREAT | O_RDWR | O_TRUNC, 0644);
        off_t off = 100;
        ssize_t n = sendfile(b, a, &off, 5000);
        CHECK("sendfile", n == 5000 && off == 5100, "n %zd off %ld", n, (long)off);
        char chk[5000]; pread(b, chk, 5000, 0);
        CHECK("sendfile data", !memcmp(chk, big + 100, 5000), "-");
        loff_t io = 0, oo = 6000;
        n = copy_file_range(a, &io, b, &oo, 3000, 0);
        CHECK("copy_file_range", n == 3000 && io == 3000 && oo == 9000, "n %zd", n);
        struct iovec iov[2]; char x[3], y[4];
        iov[0].iov_base = x; iov[0].iov_len = 3; iov[1].iov_base = y; iov[1].iov_len = 4;
        n = preadv(a, iov, 2, 26);
        CHECK("preadv", n == 7 && !memcmp(x, "abc", 3) && !memcmp(y, "defg", 4), "n %zd", n);
        iov[0].iov_base = "XY"; iov[0].iov_len = 2; iov[1].iov_base = "Z"; iov[1].iov_len = 1;
        n = pwritev(a, iov, 2, 0);
        pread(a, x, 3, 0);
        CHECK("pwritev", n == 3 && !memcmp(x, "XYZ", 3), "n %zd", n);
        close(a); close(b);
    }

    /* mknod FIFO, utimes, fchmodat */
    {
        snprintf(p, sizeof p, "%s/fifo", dir);
        CHECK("mknod fifo", mknod(p, S_IFIFO | 0600, 0) == 0, "-");
        struct stat st;
        CHECK("fifo type", stat(p, &st) == 0 && S_ISFIFO(st.st_mode), "mode %o", st.st_mode);
        CHECK("mknod chr refused", mknod(p, S_IFCHR | 0600, makedev(1, 3)) < 0 && (errno == EPERM || errno == EEXIST), "-");
        snprintf(q, sizeof q, "%s/src", dir);
        struct timeval tv[2] = { { 1000000000, 0 }, { 1234567890, 0 } };
        CHECK("utimes", utimes(q, tv) == 0 && stat(q, &st) == 0 && st.st_mtime == 1234567890, "mtime %ld", (long)st.st_mtime);
    }

    /* priorities and scheduling */
    {
        CHECK("setpriority", setpriority(PRIO_PROCESS, 0, 5) == 0, "-");
        errno = 0;
        int pr = getpriority(PRIO_PROCESS, 0);
        CHECK("getpriority", pr == 5 && errno == 0, "pr %d", pr);
        struct sched_param sp = { 0 };
        CHECK("sched_setscheduler idle", sched_setscheduler(0, SCHED_IDLE, &sp) == 0 && sched_getscheduler(0) == SCHED_IDLE, "-");
        sched_setscheduler(0, SCHED_OTHER, &sp);
        CHECK("sched_get_priority_max", sched_get_priority_max(SCHED_FIFO) == 99, "-");
        unsigned cpu = 9;
        CHECK("getcpu", getcpu(&cpu, 0) == 0 && cpu == 0, "cpu %u", cpu);
        CHECK("mlockall", mlockall(3) == 0, "-");
    }

    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); system(cmd);
    printf("syscalls: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
