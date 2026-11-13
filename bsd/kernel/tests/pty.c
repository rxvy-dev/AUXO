/* tests/pty.c: pseudo-terminals, driven the way a terminal emulator drives them. Built and
 * run inside AuxoBSD; prints "ok NAME" / "FAIL NAME ..." and "pty: N passed, M failed". */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(name, cond, ...) do { if (cond) { pass++; printf("ok %s\n", name); } \
    else { fail++; printf("FAIL %s: ", name); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); } } while (0)

static char got[16384]; static size_t ngot;

/* read from the master until want shows up (or 5 s) */
static int expect(int m, const char *want)
{
    time_t end = time(0) + 5;
    while (time(0) <= end) {
        got[ngot] = 0;
        if (strstr(got, want)) return 1;
        struct pollfd p = { m, POLLIN, 0 };
        if (poll(&p, 1, 200) > 0 && (p.revents & POLLIN)) {
            ssize_t r = read(m, got + ngot, sizeof got - 1 - ngot);
            if (r <= 0) { got[ngot] = 0; return strstr(got, want) != 0; }
            ngot += (size_t)r;
        }
    }
    got[ngot] = 0;
    return strstr(got, want) != 0;
}
static void say(int m, const char *s) { if (write(m, s, strlen(s)) < 0) { } }

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "sleep")) { sleep(30); return 0; }
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK("posix_openpt", m >= 0, "m %d", m);
    CHECK("grantpt + unlockpt", grantpt(m) == 0 && unlockpt(m) == 0, "-");
    char *name = ptsname(m);
    CHECK("ptsname is /dev/pts/N", name && !strncmp(name, "/dev/pts/", 9), "%s", name ? name : "(null)");
    struct winsize ws = { 40, 120, 0, 0 };
    CHECK("TIOCSWINSZ on the master", ioctl(m, TIOCSWINSZ, &ws) == 0, "-");

    pid_t c = fork();
    if (!c) {
        setsid();
        int s = open(name, O_RDWR);                            /* becomes the controlling terminal */
        if (s < 0) _exit(90);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        execl("/usr/bin/busybox", "sh", "-i", (char *)0);
        _exit(91);
    }
    CHECK("the shell prints a prompt", expect(m, "$ ") || expect(m, "# "), "got [%s]", got);
    ngot = 0;
    say(m, "busybox tty; echo hello-$((6*7))\r");
    CHECK("commands run and print", expect(m, "hello-42"), "got [%s]", got);
    CHECK("echo of what was typed comes back", strstr(got, "echo hello-$((6*7))") != 0, "got [%s]", got);
    CHECK("tty says /dev/pts/N", strstr(got, name) != 0, "got [%s]", got);
    CHECK("newlines come out as CR LF", strstr(got, "hello-42\r\n") != 0, "-");
    ngot = 0;
    say(m, "busybox stty size\r");
    CHECK("the window size reaches the shell", expect(m, "40 120"), "got [%s]", got);
    ngot = 0;
    say(m, "read x; echo got:$x\r");
    usleep(200000);
    say(m, "abd\x7f" "c\r");                                   /* (a typo, erased) */
    CHECK("line editing: erase", expect(m, "got:abc"), "got [%s]", got);
    ngot = 0;
    say(m, "/tmp/ptyt sleep\r");                               /* (an external program: busybox's sleep is the shell's own) */
    usleep(800000);
    say(m, "\x03");
    usleep(800000);
    ngot = 0;
    say(m, "echo status-$?\r");
    CHECK("^C interrupts the foreground program (SIGINT)", expect(m, "status-130"), "got [%s]", got);
    ngot = 0;
    say(m, "busybox stty raw -echo; busybox dd bs=1 count=3 2>/dev/null | busybox od -c | busybox head -1; busybox stty sane\r");
    usleep(1500000);
    say(m, "q\x01z");
    CHECK("raw mode: bytes go through as they are", expect(m, "q 001   z") || expect(m, "q 001 z"), "got [%s]", got);
    ngot = 0;
    say(m, "exit\r");
    int st = 0;
    for (int i = 0; i < 50 && waitpid(c, &st, WNOHANG) == 0; i++) usleep(100000);
    CHECK("the shell exits", WIFEXITED(st), "status %x", st);
    char b[64];
    ssize_t r = read(m, b, sizeof b);
    while (r > 0) r = read(m, b, sizeof b);
    CHECK("then the master reads EIO", r < 0 && errno == EIO, "r %zd", r);
    close(m);

    /* closing the master hangs up whoever's on the slave */
    m = posix_openpt(O_RDWR | O_NOCTTY); grantpt(m); unlockpt(m);
    name = ptsname(m);
    c = fork();
    if (!c) {
        close(m);                                              /* (or the master stays open in here) */
        setsid();
        int s = open(name, O_RDWR);
        signal(SIGHUP, SIG_DFL);
        char x;
        ssize_t n = read(s, &x, 1);
        _exit(n == 0 ? 3 : 4);
    }
    usleep(300000);
    close(m);
    for (int i = 0; i < 50 && waitpid(c, &st, WNOHANG) == 0; i++) usleep(100000);
    CHECK("closing the master hangs up the slave (SIGHUP)", (WIFSIGNALED(st) && WTERMSIG(st) == SIGHUP) || (WIFEXITED(st) && WEXITSTATUS(st) == 3), "status %x", st);
    printf("pty: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
