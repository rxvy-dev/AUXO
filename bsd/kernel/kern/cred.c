/* Users and groups (see cred.h).
 *
 * Every task carries Linux's set of ids (real, effective, saved and filesystem user and group
 * ids, supplementary groups) and every file an owner and a group. Access is the classic
 * owner / group / other check, with root's powers coming from capabilities (CAP_DAC_OVERRIDE
 * and friends), so a root program that drops them is held to the same rules. The capability
 * fix-ups when ids change and at execve follow Linux, so programs that drop privileges
 * (dbus-daemon, sudo, bubblewrap) see what they expect.
 *
 * The accounts are the usual text files; passwords are SHA-512 crypt ($6$), as on Ubuntu.
 * The session's user is the first ordinary account (uid 1000 and up) in /etc/passwd; a live
 * system makes "auxo" with no password, and the installer renames it and gives it one. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "fs.h"
#include "vfs.h"
#include "uproc.h"
#include "ns.h"
#include "cred.h"
#include "bearssl.h"

#define EPERM_  1
#define EACCES_ 13
#define EINVAL_ 22

static int has_cap(uint64_t caps, int cap) { return (caps >> cap) & 1; }

int cred_ingroup(const struct ucred *c, uint32_t gid)
{
    if (c->fsgid == gid) return 1;
    for (int i = 0; i < c->ngroups && i < 32; i++) if (c->groups[i] == gid) return 1;
    return 0;
}

int cred_perm_as(const struct ucred *c, uint64_t caps, struct node *n, int mask)
{
    mask &= 7;
    if (!mask || !n) return 0;
    if (n->type == N_LNK) return 0;                            /* a link's own mode means nothing */
    uint32_t m = n->mode, bits;
    if (c->fsuid == n->uid) bits = (m >> 6) & 7;
    else if (cred_ingroup(c, n->gid)) bits = (m >> 3) & 7;
    else bits = m & 7;
    if ((bits & (uint32_t)mask) == (uint32_t)mask) return 0;
    int dir = n->type == N_DIR;
    if (has_cap(caps, CAP_DAC_OVERRIDE) && (!(mask & 1) || dir || (m & 0111))) return 0;
    if (has_cap(caps, CAP_DAC_READ_SEARCH) && (mask == 4 || (dir && !(mask & 2)))) return 0;
    return -EACCES_;
}

int cred_perm(struct task *t, struct node *n, int mask)
{
    if (!t) return 0;
    return cred_perm_as(&t->ns.cred, t->ns.cap_eff, n, mask);
}

int cred_owner(struct task *t, struct node *n)
{
    return !t || t->ns.cred.fsuid == n->uid || ns_capable(t, CAP_FOWNER);
}

int cred_sticky(struct task *t, struct node *dir, struct node *victim)
{
    if (!t || !(dir->mode & 01000) || !victim) return 0;
    if (t->ns.cred.fsuid == victim->uid || t->ns.cred.fsuid == dir->uid || ns_capable(t, CAP_FOWNER)) return 0;
    return -EPERM_;
}

int cred_may_delete(struct task *t, struct node *dir, struct node *victim)
{
    int r = cred_perm(t, dir, 3);
    return r < 0 ? r : cred_sticky(t, dir, victim);
}

int cred_kill(struct task *t, struct task *o, int sig)
{
    if (!t || ns_capable(t, CAP_KILL)) return 1;
    const struct ucred *a = &t->ns.cred, *b = &o->ns.cred;
    if (a->uid == b->uid || a->uid == b->suid || a->euid == b->uid || a->euid == b->suid) return 1;
    if (sig == 18 && t->pgid == o->pgid) return 1;              /* SIGCONT in the same job */
    return 0;
}

/* a node being made: its owner is whoever is making it. The kernel's own threads make
 * things on someone's behalf (the desktop saving into a home folder), so they get the
 * directory's owner. A set-group-ID directory hands its group on. */
void cred_new_node(struct node *dir, struct node *n)
{
    struct task *t = task_current();
    if (t) { n->uid = t->ns.cred.fsuid; n->gid = t->ns.cred.fsgid; }
    else if (dir && dir != n) { n->uid = dir->uid; n->gid = dir->gid; }
    if (dir && dir != n && (dir->mode & 02000)) {
        n->gid = dir->gid;
        if (n->type == N_DIR) n->mode |= 02000;
    }
}

void cred_chown_tree(struct node *n, uint32_t uid, uint32_t gid)
{
    n->uid = uid; n->gid = gid;
    if (n->type == N_DIR) for (struct node *c = n->child; c; c = c->next) cred_chown_tree(c, uid, gid);
}

long cred_chmod(struct task *t, struct node *n, uint32_t mode)
{
    if (!cred_owner(t, n)) return -EPERM_;
    mode &= 07777;
    /* a set-group-ID bit only for a group you're in (or with CAP_FSETID) */
    if (t && (mode & 02000) && n->type != N_DIR && !cred_ingroup(&t->ns.cred, n->gid) && !ns_capable(t, CAP_FSETID)) mode &= ~02000u;
    n->mode = (uint16_t)mode;
    fs_touch(n);
    return 0;
}

/* ── changing ids ───────────────────────────────────────────────────── */
#define FS_CAPS ((1ULL << CAP_CHOWN) | (1ULL << CAP_DAC_OVERRIDE) | (1ULL << CAP_DAC_READ_SEARCH) | (1ULL << CAP_FOWNER) | \
                 (1ULL << CAP_FSETID) | (1ULL << 9) | (1ULL << CAP_MKNOD) | (1ULL << 32))

/* Linux's fix-ups (no securebits): losing root drops capabilities */
static void fixup(struct task *t, const struct ucred *o)
{
    struct ucred *c = &t->ns.cred;
    struct task_ns *n = &t->ns;
    int was_root = o->uid == 0 || o->euid == 0 || o->suid == 0;
    int is_root = c->uid == 0 || c->euid == 0 || c->suid == 0;
    if (was_root && !is_root) {
        if (!n->keepcaps) n->cap_perm = 0;
        n->cap_eff = 0;
        n->cap_amb = 0;
    }
    if (o->euid == 0 && c->euid != 0) n->cap_eff = 0;
    if (o->euid != 0 && c->euid == 0) n->cap_eff = n->cap_perm;
    if (o->fsuid == 0 && c->fsuid != 0) n->cap_eff &= ~FS_CAPS;
    if (o->fsuid != 0 && c->fsuid == 0) n->cap_eff |= n->cap_perm & FS_CAPS;
    if (c->euid != o->euid || c->egid != o->egid) n->dumpable = 0;   /* (Linux: unless suid_dumpable) */
}

/* -1 means "leave it" */
static int ok_uid(struct task *t, uint32_t v) { const struct ucred *c = &t->ns.cred; return v == c->uid || v == c->euid || v == c->suid; }
static int ok_gid(struct task *t, uint32_t v) { const struct ucred *c = &t->ns.cred; return v == c->gid || v == c->egid || v == c->sgid; }

static long setresuid(struct task *t, uint32_t r, uint32_t e, uint32_t s)
{
    int priv = ns_capable(t, CAP_SETUID);
    if (!priv && ((r != ~0u && !ok_uid(t, r)) || (e != ~0u && !ok_uid(t, e)) || (s != ~0u && !ok_uid(t, s)))) return -EPERM_;
    struct ucred o = t->ns.cred, *c = &t->ns.cred;
    if (r != ~0u) c->uid = r;
    if (e != ~0u) c->euid = e;
    if (s != ~0u) c->suid = s;
    c->fsuid = c->euid;
    fixup(t, &o);
    return 0;
}

static long setresgid(struct task *t, uint32_t r, uint32_t e, uint32_t s)
{
    int priv = ns_capable(t, CAP_SETGID);
    if (!priv && ((r != ~0u && !ok_gid(t, r)) || (e != ~0u && !ok_gid(t, e)) || (s != ~0u && !ok_gid(t, s)))) return -EPERM_;
    struct ucred o = t->ns.cred, *c = &t->ns.cred;
    if (r != ~0u) c->gid = r;
    if (e != ~0u) c->egid = e;
    if (s != ~0u) c->sgid = s;
    c->fsgid = c->egid;
    fixup(t, &o);
    return 0;
}

long cred_syscall(struct task *t, long nr, uint64_t a, uint64_t b, uint64_t c)
{
    struct ucred *cr = &t->ns.cred;
    uint32_t A = (uint32_t)a, B = (uint32_t)b, C = (uint32_t)c;
    switch (nr) {
    case 102: return cr->uid;                                   /* getuid */
    case 104: return cr->gid;                                   /* getgid */
    case 107: return cr->euid;                                  /* geteuid */
    case 108: return cr->egid;                                  /* getegid */
    case 105:                                                   /* setuid */
        if (ns_capable(t, CAP_SETUID)) return setresuid(t, A, A, A);
        if (A != cr->uid && A != cr->suid) return -EPERM_;
        return setresuid(t, ~0u, A, ~0u);
    case 106:                                                   /* setgid */
        if (ns_capable(t, CAP_SETGID)) return setresgid(t, A, A, A);
        if (A != cr->gid && A != cr->sgid) return -EPERM_;
        return setresgid(t, ~0u, A, ~0u);
    case 113: {                                                 /* setreuid */
        if (!ns_capable(t, CAP_SETUID) && ((A != ~0u && A != cr->uid && A != cr->euid) || (B != ~0u && !ok_uid(t, B)))) return -EPERM_;
        uint32_t s = ~0u;
        if (A != ~0u || (B != ~0u && B != cr->uid)) s = B != ~0u ? B : cr->euid;
        struct ucred o = *cr;
        if (A != ~0u) cr->uid = A;
        if (B != ~0u) cr->euid = B;
        if (s != ~0u) cr->suid = s;
        cr->fsuid = cr->euid;
        fixup(t, &o);
        return 0;
    }
    case 114: {                                                 /* setregid */
        if (!ns_capable(t, CAP_SETGID) && ((A != ~0u && A != cr->gid && A != cr->egid) || (B != ~0u && !ok_gid(t, B)))) return -EPERM_;
        uint32_t s = ~0u;
        if (A != ~0u || (B != ~0u && B != cr->gid)) s = B != ~0u ? B : cr->egid;
        if (A != ~0u) cr->gid = A;
        if (B != ~0u) cr->egid = B;
        if (s != ~0u) cr->sgid = s;
        cr->fsgid = cr->egid;
        return 0;
    }
    case 117: return setresuid(t, A, B, C);
    case 119: return setresgid(t, A, B, C);
    case 118: case 120: {                                       /* getresuid, getresgid */
        uint32_t v[3] = { nr == 118 ? cr->uid : cr->gid, nr == 118 ? cr->euid : cr->egid, nr == 118 ? cr->suid : cr->sgid };
        if (copyout(t->mm, a, &v[0], 4) < 0 || copyout(t->mm, b, &v[1], 4) < 0 || copyout(t->mm, c, &v[2], 4) < 0) return -14;
        return 0;
    }
    case 122: case 123: {                                       /* setfsuid, setfsgid: return the old one */
        uint32_t old = nr == 122 ? cr->fsuid : cr->fsgid;
        int priv = ns_capable(t, nr == 122 ? CAP_SETUID : CAP_SETGID);
        int ok = priv || (nr == 122 ? (ok_uid(t, A) || A == cr->fsuid) : (ok_gid(t, A) || A == cr->fsgid));
        if (ok && A != ~0u) {
            struct ucred o = *cr;
            if (nr == 122) cr->fsuid = A; else cr->fsgid = A;
            fixup(t, &o);
        }
        return old;
    }
    case 115: {                                                 /* getgroups */
        long n = (long)(int)a;
        if (n < 0) return -EINVAL_;
        if (n == 0) return cr->ngroups;
        if (n < cr->ngroups) return -EINVAL_;
        return copyout(t->mm, b, cr->groups, (size_t)cr->ngroups * 4) < 0 ? -14 : cr->ngroups;
    }
    case 116: {                                                 /* setgroups */
        if (!ns_capable(t, CAP_SETGID)) return -EPERM_;
        long n = (long)(int)a;
        if (n < 0 || n > 32) return -EINVAL_;
        uint32_t g[32];
        if (n && copyin(t->mm, g, b, (size_t)n * 4) < 0) return -14;
        memcpy(cr->groups, g, (size_t)n * 4);
        cr->ngroups = (int)n;
        return 0;
    }
    }
    return -38;
}

/* ── execve ─────────────────────────────────────────────────────────── */
void cred_exec_prepare(struct task *t, struct node *n, int nosuid, struct cred_exec *ce)
{
    const struct ucred *c = &t->ns.cred;
    ce->euid = c->euid; ce->egid = c->egid; ce->setid = 0;
    if (!nosuid && !t->ns.no_new_privs && t->ns.seccomp_mode == 0) {
        if (n->mode & 04000) { ce->euid = n->uid; ce->setid = 1; }
        if ((n->mode & 02010) == 02010) { ce->egid = n->gid; ce->setid = 1; }
    }
    ce->secure = ce->euid != c->uid || ce->egid != c->gid;
}

void cred_exec_commit(struct task *t, const struct cred_exec *ce)
{
    struct ucred *c = &t->ns.cred;
    struct task_ns *n = &t->ns;
    c->euid = c->suid = c->fsuid = ce->euid;
    c->egid = c->sgid = c->fsgid = ce->egid;
    if (c->uid == 0 || c->euid == 0) {                          /* root: the whole bounding set */
        n->cap_perm = (n->cap_bset | (n->cap_inh & n->cap_bset)) & CAP_ALL;
        n->cap_eff = c->euid == 0 ? n->cap_perm : 0;
        n->cap_amb = 0;
    } else {                                                    /* everyone else: what's ambient */
        if (ce->setid) n->cap_amb = 0;
        n->cap_perm = n->cap_amb & CAP_ALL;
        n->cap_eff = n->cap_amb & CAP_ALL;
    }
    n->keepcaps = 0;
    n->dumpable = !ce->secure;
    if (ce->secure) n->pdeathsig = 0;
}

/* ── the session ────────────────────────────────────────────────────── */
volatile int session_ready;
static struct ucred sess;                       /* all zero: root, until session_user_init */
static char sess_name[33] = "root", sess_home[96] = "/home/root";

const char *session_user(void) { return sess_name; }
const char *session_home(void) { return sess_home; }
uint32_t session_uid(void) { return sess.uid; }
uint32_t session_gid(void) { return sess.gid; }
const struct ucred *session_cred(void) { return &sess; }
/* what the kernel's own apps (Terminal commands, Aether, the built-in browser) may do for the
   user: the same check a program of theirs would get, every directory on the way included.
   (A path that doesn't exist is allowed here; opening it fails on its own.) */
int session_may(const char *path, int mask)
{
    if (sess.uid == 0) return 1;
    struct node *n, *dir; char name[256];
    if (fs_lookup(0, path, 1, &n) < 0) {
        if (!(mask & 2) || fs_lookup_parent(0, path, &dir, name, sizeof name) < 0) return 1;
        n = dir; mask = 3;                                   /* making it: write + search on the directory */
    }
    for (struct node *d = n->parent; d && d != d->parent; d = d->parent)
        if (cred_perm_as(&sess, 0, d, 1) < 0) return 0;
    return cred_perm_as(&sess, 0, n, mask) == 0;
}

const char *home_file(char *buf, size_t n, const char *rel) { ksnprintf(buf, n, "%s/%s", sess_home, rel); return buf; }

void cred_spawn(struct task *t, int as_root)
{
    if (as_root || sess.uid == 0) return;                      /* ns_task_init made it root */
    t->ns.cred = sess;
    t->ns.cap_eff = t->ns.cap_perm = t->ns.cap_inh = t->ns.cap_amb = 0;
}

/* ── the account files ──────────────────────────────────────────────── */
/* field i (0-based, ':' separated) of a line, into out */
static int field(const char *line, size_t ll, int i, char *out, size_t n)
{
    size_t s = 0;
    for (int k = 0; k < i; k++) {
        while (s < ll && line[s] != ':') s++;
        if (s >= ll) { if (n) out[0] = 0; return -1; }
        s++;
    }
    size_t e = s;
    while (e < ll && line[e] != ':') e++;
    size_t l = e - s < n - 1 ? e - s : n - 1;
    memcpy(out, line + s, l);
    out[l] = 0;
    return 0;
}

static int contains(const char *h, const char *n) { size_t l = strlen(n); for (; *h; h++) if (!strncmp(h, n, l)) return 1; return 0; }

static uint32_t num(const char *s) { uint32_t v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0'); return v; }

/* calls fn for each line; stops when it returns nonzero (and returns that) */
static int each_line(const char *path, int (*fn)(const char *l, size_t ll, void *arg), void *arg)
{
    const char *d; size_t len;
    if (vfs_read(path, &d, &len) < 0) return 0;
    size_t s = 0;
    while (s < len) {
        size_t e = s;
        while (e < len && d[e] != '\n') e++;
        if (e > s && d[s] != '#') { int r = fn(d + s, e - s, arg); if (r) return r; }
        s = e + 1;
    }
    return 0;
}

static void to_account(const char *l, size_t ll, struct account *a)
{
    char b[32];
    field(l, ll, 0, a->name, sizeof a->name);
    field(l, ll, 2, b, sizeof b); a->uid = num(b);
    field(l, ll, 3, b, sizeof b); a->gid = num(b);
    field(l, ll, 4, a->gecos, sizeof a->gecos);
    field(l, ll, 5, a->home, sizeof a->home);
    field(l, ll, 6, a->shell, sizeof a->shell);
}

struct find { const char *name; uint32_t uid; struct account *a; };
static int by_name(const char *l, size_t ll, void *arg)
{
    struct find *f = arg; char nm[33];
    field(l, ll, 0, nm, sizeof nm);
    if (strcmp(nm, f->name)) return 0;
    to_account(l, ll, f->a);
    return 1;
}
static int by_uid(const char *l, size_t ll, void *arg)
{
    struct find *f = arg; char b[32];
    field(l, ll, 2, b, sizeof b);
    if (!b[0] || num(b) != f->uid) return 0;
    to_account(l, ll, f->a);
    return 1;
}

int account_by_name(const char *name, struct account *a) { struct find f = { name, 0, a }; return each_line("/etc/passwd", by_name, &f) ? 0 : -1; }
int account_by_uid(uint32_t uid, struct account *a) { struct find f = { 0, uid, a }; return each_line("/etc/passwd", by_uid, &f) ? 0 : -1; }

/* the first ordinary account: uid 1000–59999 with a real shell */
static int first_user(const char *l, size_t ll, void *arg)
{
    struct account *a = arg, x;
    to_account(l, ll, &x);
    if (x.uid < 1000 || x.uid >= 60000 || contains(x.shell, "nologin") || contains(x.shell, "false")) return 0;
    *a = x;
    return 1;
}

struct grp { const char *user; uint32_t *out; int n, max; const char *want; uint32_t gid; };
static int member(const char *l, size_t ll, const char *user)
{
    char m[512];
    field(l, ll, 3, m, sizeof m);
    size_t ul = strlen(user);
    for (char *p = m; *p; ) {
        char *e = p;
        while (*e && *e != ',') e++;
        if ((size_t)(e - p) == ul && !strncmp(p, user, ul)) return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}
static int groups_of(const char *l, size_t ll, void *arg)
{
    struct grp *g = arg; char b[32];
    if (!member(l, ll, g->user)) return 0;
    field(l, ll, 2, b, sizeof b);
    if (g->n < g->max) g->out[g->n++] = num(b);
    return 0;
}

int account_groups(const char *name, uint32_t gid, uint32_t *out, int max)
{
    struct grp g = { name, out, 0, max, 0, 0 };
    if (max > 0) out[g.n++] = gid;
    each_line("/etc/group", groups_of, &g);
    return g.n;
}

static int in_group(const char *l, size_t ll, void *arg)
{
    struct grp *g = arg; char nm[64];
    field(l, ll, 0, nm, sizeof nm);
    return !strcmp(nm, g->want) && member(l, ll, g->user);
}
int account_in_group(const char *name, const char *group) { struct grp g = { name, 0, 0, 0, group, 0 }; return each_line("/etc/group", in_group, &g); }

/* rewrite a file line by line: fn writes the replacement (or the line) into out */
static int rewrite(const char *path, int (*fn)(const char *l, size_t ll, char *out, size_t n, void *arg), void *arg, const char *append)
{
    const char *d; size_t len;
    if (vfs_read(path, &d, &len) < 0) { d = ""; len = 0; }
    size_t cap = len + 4096 + (append ? strlen(append) : 0);
    char *nb = rs_kmalloc(cap);
    if (!nb) return -1;
    size_t o = 0, s = 0;
    while (s < len) {
        size_t e = s;
        while (e < len && d[e] != '\n') e++;
        char line[1024];
        int w = fn ? fn(d + s, e - s, line, sizeof line, arg) : -1;
        if (w < 0) { if (o + (e - s) + 1 < cap) { memcpy(nb + o, d + s, e - s); o += e - s; nb[o++] = '\n'; } }
        else if (w > 0) { size_t l = strlen(line); if (o + l + 1 < cap) { memcpy(nb + o, line, l); o += l; nb[o++] = '\n'; } }
        s = e + 1;
    }
    if (append) { size_t l = strlen(append); if (o + l < cap) { memcpy(nb + o, append, l); o += l; } }
    int r = vfs_write(path, nb, o);
    rs_kfree(nb);
    return r;
}

/* /etc/shadow: field 1 is the password: "" none, "*" or "!..." locked, "$6$..." a hash */
struct pwq { const char *name; char pw[160]; int found; };
static int shadow_of(const char *l, size_t ll, void *arg)
{
    struct pwq *q = arg; char nm[33];
    field(l, ll, 0, nm, sizeof nm);
    if (strcmp(nm, q->name)) return 0;
    field(l, ll, 1, q->pw, sizeof q->pw);
    q->found = 1;
    return 1;
}

int account_password_set(const char *name)
{
    struct pwq q; memset(&q, 0, sizeof q); q.name = name;
    each_line("/etc/shadow", shadow_of, &q);
    return q.found && q.pw[0] != 0;
}

int account_check_password(const char *name, const char *pw)
{
    struct pwq q; memset(&q, 0, sizeof q); q.name = name;
    each_line("/etc/shadow", shadow_of, &q);
    if (!q.found) return 0;
    if (!q.pw[0]) return 1;                                     /* no password */
    if (strncmp(q.pw, "$6$", 3)) return 0;                      /* locked, or a kind we don't do */
    const char *salt = q.pw + 3;
    int rounds = 5000;
    if (!strncmp(salt, "rounds=", 7)) { rounds = (int)num(salt + 7); salt = strchr(salt, '$'); if (!salt) return 0; salt++; }
    char sl[17]; size_t k = 0;
    while (salt[k] && salt[k] != '$' && k < 16) { sl[k] = salt[k]; k++; }
    sl[k] = 0;
    char out[160];
    sha512_crypt(pw, sl, rounds, out, sizeof out);
    size_t a = strlen(out), b = strlen(q.pw), diff = a ^ b;
    for (size_t i = 0; i < a && i < b; i++) diff |= (size_t)(out[i] ^ q.pw[i]);
    return diff == 0;
}

struct setpw { const char *name; const char *hash; int done; };
static int put_hash(const char *l, size_t ll, char *out, size_t n, void *arg)
{
    struct setpw *s = arg; char nm[33];
    field(l, ll, 0, nm, sizeof nm);
    if (strcmp(nm, s->name)) return -1;
    char rest[512];
    size_t p = 0, c = 0;
    while (p < ll && c < 2) { if (l[p] == ':') c++; p++; }     /* after "name:pw:" */
    size_t rl = ll - p < sizeof rest - 1 ? ll - p : sizeof rest - 1;
    memcpy(rest, l + p, rl); rest[rl] = 0;
    ksnprintf(out, n, "%s:%s:%s", s->name, s->hash, c == 2 ? rest : "19000:0:99999:7:::");
    s->done = 1;
    return 1;
}

static void random_salt(char *s, int n)
{
    static const char a[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    br_sha512_context h; uint8_t d[64]; uint64_t x = rdtsc();
    br_sha512_init(&h); br_sha512_update(&h, &x, 8); x = kernel_ticks(); br_sha512_update(&h, &x, 8); br_sha512_out(&h, d);
    for (int i = 0; i < n; i++) s[i] = a[d[i] & 63];
    s[n] = 0;
}

int account_set_password(const char *name, const char *pw)
{
    char hash[160], salt[17];
    if (pw && pw[0]) { random_salt(salt, 16); sha512_crypt(pw, salt, 5000, hash, sizeof hash); }
    else hash[0] = 0;
    struct setpw s = { name, hash, 0 };
    rewrite("/etc/shadow", put_hash, &s, 0);
    if (!s.done) {
        char line[256];
        ksnprintf(line, sizeof line, "%s:%s:19000:0:99999:7:::\n", name, hash);
        rewrite("/etc/shadow", 0, 0, line);
    }
    return 0;
}

/* rename an account (and its group and home): the installer's "who are you" */
struct ren { const char *o, *n, *gecos; };
static int ren_passwd(const char *l, size_t ll, char *out, size_t n, void *arg)
{
    struct ren *r = arg; struct account a;
    char nm[33]; field(l, ll, 0, nm, sizeof nm);
    if (strcmp(nm, r->o)) return -1;
    to_account(l, ll, &a);
    ksnprintf(out, n, "%s:x:%u:%u:%s:/home/%s:%s", r->n, a.uid, a.gid, r->gecos && r->gecos[0] ? r->gecos : a.gecos, r->n, a.shell[0] ? a.shell : "/bin/sh");
    return 1;
}
static int ren_shadow(const char *l, size_t ll, char *out, size_t n, void *arg)
{
    struct ren *r = arg; char nm[33];
    field(l, ll, 0, nm, sizeof nm);
    if (strcmp(nm, r->o)) return -1;
    size_t p = strlen(nm);
    char rest[512]; size_t rl = ll - p < sizeof rest - 1 ? ll - p : sizeof rest - 1;
    memcpy(rest, l + p, rl); rest[rl] = 0;
    ksnprintf(out, n, "%s%s", r->n, rest);
    return 1;
}
static int ren_group(const char *l, size_t ll, char *out, size_t n, void *arg)
{
    struct ren *r = arg;
    char nm[64], pw[16], gid[16], mem[512], nmem[600];
    field(l, ll, 0, nm, sizeof nm); field(l, ll, 1, pw, sizeof pw); field(l, ll, 2, gid, sizeof gid); field(l, ll, 3, mem, sizeof mem);
    size_t o = 0, ol = strlen(r->o);
    int changed = !strcmp(nm, r->o);
    nmem[0] = 0;
    for (char *p = mem; *p; ) {
        char *e = p; while (*e && *e != ',') e++;
        const char *who = p; size_t wl = (size_t)(e - p);
        if (wl == ol && !strncmp(p, r->o, ol)) { who = r->n; wl = strlen(r->n); changed = 1; }
        if (o + wl + 2 < sizeof nmem) { if (o) nmem[o++] = ','; memcpy(nmem + o, who, wl); o += wl; nmem[o] = 0; }
        p = *e ? e + 1 : e;
    }
    if (!changed) return -1;
    ksnprintf(out, n, "%s:%s:%s:%s", !strcmp(nm, r->o) ? r->n : nm, pw, gid, nmem);
    return 1;
}

int account_rename(const char *old, const char *new_name, const char *gecos)
{
    struct account a;
    if (account_by_name(old, &a) < 0) return -1;
    if (strcmp(old, new_name) && account_by_name(new_name, &(struct account){ 0 }) == 0) return -2;   /* taken */
    struct ren r = { old, new_name, gecos };
    rewrite("/etc/passwd", ren_passwd, &r, 0);
    rewrite("/etc/shadow", ren_shadow, &r, 0);
    rewrite("/etc/group", ren_group, &r, 0);
    if (strcmp(old, new_name)) {
        struct node *home;
        if (fs_lookup(0, "/home", 1, &home) == 0 && fs_child(home, old) && !fs_child(home, new_name)) fs_rename(home, old, home, new_name);
    }
    return 0;
}

/* ── making sure the session's user exists ──────────────────────────── */
static int has_line(const char *path, const char *name)
{
    struct account a; struct find f = { name, 0, &a };
    return each_line(path, by_name, &f);
}

struct addm { const char *user, *group; };
static int add_member(const char *l, size_t ll, char *out, size_t n, void *arg)
{
    struct addm *m = arg; char nm[64];
    field(l, ll, 0, nm, sizeof nm);
    if (strcmp(nm, m->group) || member(l, ll, m->user)) return -1;
    char lc[600]; size_t k = ll < sizeof lc - 1 ? ll : sizeof lc - 1;
    memcpy(lc, l, k); lc[k] = 0;
    ksnprintf(out, n, "%s%s%s", lc, lc[k ? k - 1 : 0] == ':' ? "" : ",", m->user);
    return 1;
}

static void ensure_group(const char *name, uint32_t gid, const char *user)
{
    if (!has_line("/etc/group", name)) {
        char line[96];
        ksnprintf(line, sizeof line, "%s:x:%u:\n", name, gid);
        rewrite("/etc/group", 0, 0, line);
    }
    if (user) { struct addm m = { user, name }; rewrite("/etc/group", add_member, &m, 0); }
}

static struct node *mkdir_owned(const char *path, uint16_t mode, uint32_t uid, uint32_t gid)
{
    struct node *n;
    if (fs_lookup(0, path, 1, &n) == 0) return n;
    struct node *dir; char name[64];
    if (fs_lookup_parent(0, path, &dir, name, sizeof name) < 0) return 0;
    n = fs_create(dir, name, N_DIR, mode);
    if (n) { n->uid = uid; n->gid = gid; n->mode = mode; }
    return n;
}

void session_set_user(const char *name)
{
    struct account a;
    if (account_by_name(name, &a) < 0) return;
    memset(&sess, 0, sizeof sess);
    sess.uid = sess.euid = sess.suid = sess.fsuid = a.uid;
    sess.gid = sess.egid = sess.sgid = sess.fsgid = a.gid;
    sess.ngroups = account_groups(a.name, a.gid, sess.groups, 32);
    strlcpy(sess_name, a.name, sizeof sess_name);
    strlcpy(sess_home, a.home[0] ? a.home : "/", sizeof sess_home);
    char run[48];
    ksnprintf(run, sizeof run, "/run/user/%u", a.uid);
    mkdir_owned("/run/user", 0755, 0, 0);
    mkdir_owned(run, 0700, a.uid, a.gid);
}

void session_user_init(void)
{
    /* places everyone writes to (a root image may have them 0755), and root's own */
    static const struct { const char *p; uint16_t m; } fix[] = { { "/tmp", 01777 }, { "/var/tmp", 01777 }, { "/dev/shm", 01777 }, { "/root", 0700 } };
    for (unsigned i = 0; i < sizeof fix / sizeof fix[0]; i++) { struct node *n; if (fs_lookup(0, fix[i].p, 1, &n) == 0 && n->type == N_DIR) { n->mode = fix[i].m; n->uid = n->gid = 0; } }
    { struct node *fb; if (fs_lookup(0, "/dev/fb0", 1, &fb) == 0) { fb->mode = 0660; fb->gid = 44; } }   /* group video */
    if (!vfs_exists("/etc/passwd")) return;                    /* no root filesystem: stay root */
    if (!vfs_exists("/etc/shadow")) {                          /* Debian's base leaves it to passwd's postinst */
        rewrite("/etc/shadow", 0, 0, "root:*:19000:0:99999:7:::\n");
        struct node *s;
        if (fs_lookup(0, "/etc/shadow", 1, &s) == 0) { s->mode = 0640; struct account g; (void)g; s->gid = 42; }
    }
    struct account a;
    if (!each_line("/etc/passwd", first_user, &a)) {
        rewrite("/etc/passwd", 0, 0, "auxo:x:1000:1000:AuxoBSD:/home/auxo:/bin/sh\n");
        if (!has_line("/etc/shadow", "auxo")) rewrite("/etc/shadow", 0, 0, "auxo::19000:0:99999:7:::\n");
        ensure_group("auxo", 1000, 0);
        static const struct { const char *n; uint32_t g; } groups[] = {
            { "sudo", 27 }, { "audio", 29 }, { "video", 44 }, { "plugdev", 46 }, { "users", 100 }, { "render", 105 }, { "input", 106 }, { "netdev", 107 },
        };
        for (unsigned i = 0; i < sizeof groups / sizeof groups[0]; i++) ensure_group(groups[i].n, groups[i].g, "auxo");
        if (account_by_name("auxo", &a) < 0) return;
    }
    char home[96];
    strlcpy(home, a.home[0] ? a.home : "/home/auxo", sizeof home);
    /* systems from before there were users kept everything in /home/root: it becomes theirs */
    struct node *h, *hd;
    if (fs_lookup(0, home, 1, &h) < 0 && fs_lookup(0, "/home", 1, &hd) == 0 && fs_child(hd, "root") && !strncmp(home, "/home/", 6) && !strchr(home + 6, '/')) {
        fs_rename(hd, "root", hd, home + 6);
        if (fs_lookup(0, home, 1, &h) == 0) { cred_chown_tree(h, a.uid, a.gid); fs_touch(h); }
    }
    if ((h = mkdir_owned(home, 0750, a.uid, a.gid)) != 0 && h->uid != a.uid) { cred_chown_tree(h, a.uid, a.gid); fs_touch(h); }
    mkdir_owned("/root", 0700, 0, 0);
    session_set_user(a.name);
    kprintf("user: %s (uid %u), home %s%s\n", sess_name, sess.uid, sess_home, account_password_set(sess_name) ? "" : ", no password");
}

/* ── SHA-512 crypt (Ulrich Drepper's specification, as glibc and libxcrypt do it) ── */
static void sha(br_sha512_context *c) { br_sha512_init(c); }

void sha512_crypt(const char *pw, const char *salt, int rounds, char *out, size_t n)
{
    size_t pl = strlen(pw), sl = strlen(salt);
    if (sl > 16) sl = 16;
    if (rounds < 1000) rounds = 1000;
    if (rounds > 999999999) rounds = 999999999;
    uint8_t A[64], B[64], DP[64], DS[64];
    br_sha512_context c;
    sha(&c); br_sha512_update(&c, pw, pl); br_sha512_update(&c, salt, sl); br_sha512_update(&c, pw, pl); br_sha512_out(&c, B);
    sha(&c); br_sha512_update(&c, pw, pl); br_sha512_update(&c, salt, sl);
    size_t k;
    for (k = pl; k > 64; k -= 64) br_sha512_update(&c, B, 64);
    br_sha512_update(&c, B, k);
    for (k = pl; k > 0; k >>= 1) { if (k & 1) br_sha512_update(&c, B, 64); else br_sha512_update(&c, pw, pl); }
    br_sha512_out(&c, A);
    sha(&c); for (k = 0; k < pl; k++) br_sha512_update(&c, pw, pl); br_sha512_out(&c, DP);
    sha(&c); for (k = 0; k < 16u + A[0]; k++) br_sha512_update(&c, salt, sl); br_sha512_out(&c, DS);
    uint8_t P[256], S[16];
    for (k = 0; k < pl && k < sizeof P; k++) P[k] = DP[k % 64];
    for (k = 0; k < sl; k++) S[k] = DS[k];
    size_t ppl = pl < sizeof P ? pl : sizeof P;
    uint8_t C[64];
    memcpy(C, A, 64);
    for (int i = 0; i < rounds; i++) {
        sha(&c);
        if (i & 1) br_sha512_update(&c, P, ppl); else br_sha512_update(&c, C, 64);
        if (i % 3) br_sha512_update(&c, S, sl);
        if (i % 7) br_sha512_update(&c, P, ppl);
        if (i & 1) br_sha512_update(&c, C, 64); else br_sha512_update(&c, P, ppl);
        br_sha512_out(&c, C);
    }
    static const char b64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static const uint8_t ord[21][3] = {
        {0,21,42},{22,43,1},{44,2,23},{3,24,45},{25,46,4},{47,5,26},{6,27,48},{28,49,7},{50,8,29},{9,30,51},{31,52,10},
        {53,11,32},{12,33,54},{34,55,13},{56,14,35},{15,36,57},{37,58,16},{59,17,38},{18,39,60},{40,61,19},{62,20,41} };
    char enc[90]; size_t o = 0;
    for (int i = 0; i < 21; i++) {
        uint32_t w = (uint32_t)C[ord[i][0]] << 16 | (uint32_t)C[ord[i][1]] << 8 | C[ord[i][2]];
        for (int j = 0; j < 4; j++) { enc[o++] = b64[w & 63]; w >>= 6; }
    }
    { uint32_t w = C[63]; for (int j = 0; j < 2; j++) { enc[o++] = b64[w & 63]; w >>= 6; } }
    enc[o] = 0;
    char sb[17]; memcpy(sb, salt, sl); sb[sl] = 0;
    if (rounds != 5000) ksnprintf(out, n, "$6$rounds=%d$%s$%s", rounds, sb, enc);
    else ksnprintf(out, n, "$6$%s$%s", sb, enc);
    memset(P, 0, sizeof P); memset(DP, 0, sizeof DP);
}
