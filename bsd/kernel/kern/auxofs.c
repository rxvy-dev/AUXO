/* AuxoFS: the installed system's root filesystem, on its disk.
 *
 * The tree of names, owners and modes is kept in memory (as the live system's is); what's on
 * the disk is the contents of the files and a durable record of the tree. A file is read from
 * the disk the first time something needs it, and when memory runs short the files nobody has
 * used for a while are dropped again (they come back from the disk), so the system no longer
 * has to fit in RAM, and starting it no longer means loading a whole image.
 *
 * Layout, in 4 KiB blocks of the partition:
 *   0, 1     two superblocks, written alternately; the valid one with the higher generation wins
 *   2 ..     the log: one segment per commit, numbered; replayed in order after the checkpoint
 *   the rest file contents and checkpoints, each in one contiguous extent
 *
 * Crash safety is by never overwriting what the last durable state points at:
 *   - a changed file's contents go to a new extent (the old one is freed only after the
 *     record pointing at the new one is on the disk);
 *   - every few seconds the changed and deleted nodes go to the disk as one log segment, with
 *     a checksum: a segment cut short by a crash fails it and replay stops before it;
 *   - when the log fills, the whole tree is written as a checkpoint to a new extent, and then
 *     the other superblock is pointed at it.
 * Each disk write is followed by a cache flush (the drivers do that), so the order holds.
 * After a crash you lose at most the last few seconds; you never get a half-written tree. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "fs.h"
#include "disk.h"
#include "auxofs.h"

#define BS 4096u
#define SPB (BS / 512)                       /* sectors per block */
#define LOG_MIN 2048u                        /* 8 MiB of log, at least */
#define IOBUF (1u << 20)                     /* bounce buffers below 4 GiB, for the drivers */
#define COMMIT_SECONDS 5

struct sb {
    char     magic[8];                       /* "AUXOFS01" */
    uint32_t version, bs;
    uint64_t nblocks, gen;
    uint64_t ckpt_blk, ckpt_bytes;
    uint32_t ckpt_crc, pad;
    uint64_t log_start, log_blocks, log_seq0;
    uint64_t next_ino;
    uint32_t crc;                            /* of everything before it */
} __attribute__((packed));

struct seg {                                 /* a log segment's header (then the records) */
    char     magic[8];                       /* "AUXOLOG1" */
    uint64_t seq;
    uint32_t bytes, crc, hcrc, pad;
} __attribute__((packed));

/* one node, as the disk keeps it; then the name and (links) the target */
enum { R_NODE = 1, R_DELETE = 2 };
struct rec {
    uint8_t  kind, type;
    uint16_t mode;
    uint32_t uid, gid, mtime;
    uint64_t ino, parent, size, blk;
    uint32_t nblk;
    uint16_t nlen, llen;
} __attribute__((packed));

static struct {
    struct disk *d;
    uint64_t base;                           /* first sector */
    uint64_t nblocks;
    struct sb sb;
    int slot;                                /* which superblock is current */
    uint64_t log_head;                       /* next free block in the log */
    uint64_t seq;                            /* next segment's number */
    uint32_t committed;                      /* fs generation the disk has */
    int mounted;
    uint8_t *rbuf, *wbuf;                    /* bounce buffers: loading, committing */
    volatile int io_owner, cm_owner;
    uint64_t loads, evictions, commits, checkpoints;
} fs;

/* deleted nodes waiting for their record to reach the disk */
struct del { uint64_t ino, blk; uint32_t nblk; };
static struct del *dels; static int ndels, capdels;
/* extents to give back once the state that stopped using them is durable */
struct ext { uint64_t s, n; };
static struct ext *pend; static int npend, cappend;
/* free space, sorted by start */
static struct ext *fr; static int nfr, capfr;

/* ── small things ───────────────────────────────────────────────────── */
static uint32_t crc_tab[256];
static uint32_t crc32(const void *p, size_t n)
{
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t *b = p;
    while (n--) c = crc_tab[(c ^ *b++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void lock(volatile int *owner)
{
    int me = rs_current_pid();
    while (*owner != -1 && *owner != me) rs_yield();
    *owner = me;
}
static void unlock(volatile int *owner) { *owner = -1; }

static int grow(void **arr, int *cap, int need, size_t sz)
{
    if (need <= *cap) return 0;
    int nc = *cap ? *cap * 2 : 256;
    while (nc < need) nc *= 2;
    void *n = rs_kmalloc((size_t)nc * sz);
    if (!n) return -1;
    if (*arr) { memcpy(n, *arr, (size_t)*cap * sz); rs_kfree(*arr); }
    *arr = n; *cap = nc;
    return 0;
}

/* ── the disk ───────────────────────────────────────────────────────── */
static int io(uint64_t blk, uint64_t nblk, uint8_t *buf, int write)
{
    if (!nblk) return 0;
    if (blk + nblk > fs.nblocks) return -1;
    uint8_t *bb = write ? fs.wbuf : fs.rbuf;
    for (uint64_t done = 0; done < nblk; ) {
        uint64_t k = nblk - done > IOBUF / BS ? IOBUF / BS : nblk - done;
        lock(&fs.io_owner);                              /* (the bounce buffers are shared too) */
        if (write) memcpy(bb, buf + done * BS, k * BS);
        int r = write ? fs.d->write(fs.d, fs.base + (blk + done) * SPB, (uint32_t)(k * SPB), bb)
                      : fs.d->read(fs.d, fs.base + (blk + done) * SPB, (uint32_t)(k * SPB), bb);
        if (r == 0 && !write) memcpy(buf + done * BS, bb, k * BS);
        unlock(&fs.io_owner);
        if (r < 0 || fs.d->gone) return -1;
        done += k;
    }
    return 0;
}

/* ── free space ─────────────────────────────────────────────────────── */
static void free_ext(uint64_t s, uint64_t n)
{
    if (!n) return;
    int i = 0;
    while (i < nfr && fr[i].s < s) i++;
    if (i > 0 && fr[i - 1].s + fr[i - 1].n == s) {                 /* joins the one before */
        fr[i - 1].n += n;
        if (i < nfr && fr[i - 1].s + fr[i - 1].n == fr[i].s) { fr[i - 1].n += fr[i].n; memmove(&fr[i], &fr[i + 1], (size_t)(nfr - i - 1) * sizeof *fr); nfr--; }
        return;
    }
    if (i < nfr && s + n == fr[i].s) { fr[i].s = s; fr[i].n += n; return; }
    if (grow((void **)&fr, &capfr, nfr + 1, sizeof *fr) < 0) return;  /* (leaks the space until the next mount) */
    memmove(&fr[i + 1], &fr[i], (size_t)(nfr - i) * sizeof *fr);
    fr[i].s = s; fr[i].n = n; nfr++;
}

/* take [s, s+n) out of the free list (mount: what the tree uses) */
static void take(uint64_t s, uint64_t n)
{
    for (int i = 0; i < nfr && n; i++) {
        struct ext *e = &fr[i];
        if (s >= e->s + e->n || s + n <= e->s) continue;
        uint64_t a = s > e->s ? s : e->s, b = s + n < e->s + e->n ? s + n : e->s + e->n;
        if (a == e->s && b == e->s + e->n) { memmove(e, e + 1, (size_t)(nfr - i - 1) * sizeof *fr); nfr--; i--; }
        else if (a == e->s) { e->n -= b - a; e->s = b; }
        else if (b == e->s + e->n) e->n -= b - a;
        else {                                                       /* the middle: split */
            uint64_t tail_s = b, tail_n = e->s + e->n - b;
            e->n = a - e->s;
            free_ext(tail_s, tail_n);
        }
    }
}

/* best fit: the smallest free run that holds n blocks */
static uint64_t alloc_ext(uint64_t n)
{
    int best = -1;
    for (int i = 0; i < nfr; i++) if (fr[i].n >= n && (best < 0 || fr[i].n < fr[best].n)) best = i;
    if (best < 0) return 0;
    uint64_t s = fr[best].s;
    fr[best].s += n; fr[best].n -= n;
    if (!fr[best].n) { memmove(&fr[best], &fr[best + 1], (size_t)(nfr - best - 1) * sizeof *fr); nfr--; }
    return s;
}

static void later_free(uint64_t s, uint64_t n)
{
    if (!n) return;
    if (grow((void **)&pend, &cappend, npend + 1, sizeof *pend) < 0) return;
    pend[npend].s = s; pend[npend].n = n; npend++;
}

uint64_t auxofs_free_blocks(void) { uint64_t t = 0; for (int i = 0; i < nfr; i++) t += fr[i].n; return t; }
uint64_t auxofs_total_blocks(void) { return fs.nblocks; }

/* ── which nodes are kept ───────────────────────────────────────────── */
static int kept(struct node *n)
{
    if (n->type == N_SOCK || n->type == N_CHR || n->dev == 0x50524F43u) return 0;   /* (procfs) */
    struct node *top = n;
    while (top->parent && top->parent != top && top->parent != fs_root()) top = top->parent;
    if (top->parent == fs_root()) {
        static const char *const not[] = { "dev", "proc", "run", "tmp", "sys", 0 };
        for (int i = 0; not[i]; i++) if (!strcmp(top->name, not[i])) return 0;
    }
    return 1;
}

/* ── loading a file's contents ──────────────────────────────────────── */
static int load(struct node *n)
{
    if (!fs.mounted || fs_resident(n)) return 0;
    uint64_t cap = (n->size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t need = ((n->size + BS - 1) / BS) * BS;
    if (need > (uint64_t)n->dnblk * BS) return -5;
    uint64_t pg = pmm_alloc_contig_any(cap / PAGE_SIZE);
    if (!pg) { auxofs_reclaim(cap / PAGE_SIZE); pg = pmm_alloc_contig_any(cap / PAGE_SIZE); }
    if (!pg) return -12;
    uint8_t *d = (uint8_t *)(uintptr_t)pg;
    uint64_t blk = n->dblk;
    if (io(blk, need / BS, d, 0) < 0) {
        for (uint64_t o = 0; o < cap; o += PAGE_SIZE) pmm_free_page(pg + o);
        kprintf("auxofs: couldn't read %s from the disk\n", n->name);
        return -5;
    }
    if (cap > need) memset(d + need, 0, cap - need);
    fs_lock();
    if (!fs_resident(n) && n->dblk == blk) { n->data = d; n->cap = cap; d = 0; fs.loads++; }   /* (someone else may have) */
    fs_unlock();
    if (d) for (uint64_t o = 0; o < cap; o += PAGE_SIZE) pmm_free_page(pg + o);
    return 0;
}

static void deleted(struct node *n)
{
    if (!fs.mounted) return;
    if (grow((void **)&dels, &capdels, ndels + 1, sizeof *dels) < 0) return;
    dels[ndels].ino = n->ino; dels[ndels].blk = n->dblk; dels[ndels].nblk = n->dnblk; ndels++;
    n->dblk = 0; n->dnblk = 0;
}

/* ── memory pressure: drop what hasn't been used lately ─────────────── */
struct cand { struct node *n; uint64_t t; };
static void collect_clean(struct node *d, struct cand *c, int *nc, int max, uint64_t older)
{
    for (struct node *x = d->child; x; x = x->next) {
        if (x->type == N_DIR) { collect_clean(x, c, nc, max, older); continue; }
        if (x->type != N_REG || !x->data || !x->cap || x->maps || x->refs > 0 || x->ddirty || !x->dblk || x->atick > older) continue;
        if (*nc < max) { c[(*nc)++] = (struct cand){ x, x->atick }; continue; }
        int w = 0;                                                   /* full: replace the most recent */
        for (int i = 1; i < max; i++) if (c[i].t > c[w].t) w = i;
        if (x->atick < c[w].t) c[w] = (struct cand){ x, x->atick };
    }
}

static volatile int rc_owner = -1;
size_t auxofs_reclaim(size_t want_pages)
{
    if (!fs.mounted) return 0;
    lock(&rc_owner);
    static struct cand c[512];
    int nc = 0;
    uint64_t now = kernel_ticks(), older = now > 5 * AUXO_HZ ? now - 5 * AUXO_HZ : 0;
    fs_lock();
    collect_clean(fs_root(), c, &nc, 512, older);
    /* oldest first */
    for (int i = 1; i < nc; i++) { struct cand k = c[i]; int j = i; while (j > 0 && c[j - 1].t > k.t) { c[j] = c[j - 1]; j--; } c[j] = k; }
    size_t got = 0;
    for (int i = 0; i < nc && got < want_pages; i++) { got += c[i].n->cap / PAGE_SIZE; fs_evict(c[i].n); fs.evictions++; }
    fs_unlock();
    unlock(&rc_owner);
    return got;
}

/* ── records ────────────────────────────────────────────────────────── */
struct buf { uint8_t *p; size_t n, cap; uint64_t pages; };

static int bput(struct buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1 << 20;
        while (nc < b->n + n) nc *= 2;
        uint64_t pg = pmm_alloc_contig_any(nc / PAGE_SIZE);
        if (!pg) return -1;
        if (b->p) { memcpy((void *)(uintptr_t)pg, b->p, b->n); for (size_t o = 0; o < b->cap; o += PAGE_SIZE) pmm_free_page((uint64_t)(uintptr_t)b->p + o); }
        b->p = (uint8_t *)(uintptr_t)pg; b->cap = nc;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
    return 0;
}
static void bfree(struct buf *b) { if (b->p) for (size_t o = 0; o < b->cap; o += PAGE_SIZE) pmm_free_page((uint64_t)(uintptr_t)b->p + o); b->p = 0; b->n = b->cap = 0; }

static uint64_t ino_of(struct node *n) { return n == fs_root() ? 1 : n->ino; }

/* (fs_lock held) */
static int put_node(struct buf *b, struct node *n, uint64_t size, uint64_t blk, uint32_t nblk)
{
    struct rec r;
    memset(&r, 0, sizeof r);
    r.kind = R_NODE; r.type = n->type; r.mode = n->mode; r.uid = n->uid; r.gid = n->gid; r.mtime = n->mtime;
    r.ino = ino_of(n); r.parent = n == fs_root() ? 0 : ino_of(n->parent);
    r.size = n->type == N_REG ? size : 0; r.blk = blk; r.nblk = nblk;     /* (the size the extent holds) */
    r.nlen = n == fs_root() ? 0 : (uint16_t)strlen(n->name);
    char link[1024];
    if (n->type == N_LNK) { size_t l = n->size < sizeof link ? (size_t)n->size : sizeof link; if (n->data) memcpy(link, n->data, l); else l = 0; r.llen = (uint16_t)l; }
    if (bput(b, &r, sizeof r) < 0 || bput(b, n == fs_root() ? "" : n->name, r.nlen) < 0) return -1;
    if (r.llen && bput(b, link, r.llen) < 0) return -1;
    return 0;
}

/* ── writing contents back ──────────────────────────────────────────── */
/* a dirty file's contents to a new extent; the old one is freed after the next commit */
static int write_back(struct node *n)
{
    fs_lock();
    if (!n->ddirty || n->type != N_REG) { fs_unlock(); return 0; }
    n->ddirty = 0;
    uint64_t size = n->size;
    fs_unlock();
    uint64_t nblk = (size + BS - 1) / BS, blk = nblk ? alloc_ext(nblk) : 0;
    if (nblk && !blk) { fs_lock(); n->ddirty = 1; fs_unlock(); return -28; }     /* the disk is full */
    for (uint64_t done = 0; done < nblk; ) {
        uint64_t k = nblk - done > IOBUF / BS ? IOBUF / BS : nblk - done;
        static uint8_t *chunk;
        if (!chunk) chunk = (uint8_t *)(uintptr_t)pmm_alloc_contig(IOBUF / PAGE_SIZE);
        if (!chunk) { free_ext(blk, nblk); fs_lock(); n->ddirty = 1; fs_unlock(); return -12; }
        fs_lock();                                       /* it may change under us: then it's dirty again */
        uint64_t off = done * BS, want = k * BS;
        memset(chunk, 0, want);
        if (n->data && off < n->size) memcpy(chunk, n->data + off, off + want <= n->size ? want : n->size - off);
        fs_unlock();
        if (io(blk + done, k, chunk, 1) < 0) { free_ext(blk, nblk); fs_lock(); n->ddirty = 1; fs_unlock(); return -5; }
        done += k;
    }
    fs_lock();
    later_free(n->dblk, n->dnblk);
    n->dblk = blk; n->dnblk = (uint32_t)nblk; n->dsize = size;
    fs_unlock();
    return 0;
}

/* ── superblocks and checkpoints ────────────────────────────────────── */
static int write_sb(void)
{
    static uint8_t blk[BS];
    fs.sb.gen++;
    fs.sb.crc = crc32(&fs.sb, offsetof(struct sb, crc));
    memset(blk, 0, BS);
    memcpy(blk, &fs.sb, sizeof fs.sb);
    int slot = (int)(fs.sb.gen & 1);
    if (io((uint64_t)slot, 1, blk, 1) < 0) return -1;
    fs.slot = slot;
    return 0;
}

static void walk_all(struct node *d, struct node ***arr, int *n, int *cap)
{
    for (struct node *x = d->child; x; x = x->next) {
        if (!kept(x)) continue;
        if (grow((void **)arr, cap, *n + 1, sizeof **arr) < 0) return;
        (*arr)[(*n)++] = x;
        x->refs++;
        if (x->type == N_DIR) walk_all(x, arr, n, cap);
    }
}

static uint64_t next_ino(void);
static int need_ckpt;                       /* deletions that did not reach the disk: the next commit is a checkpoint */

/* the whole tree to a new extent, then the superblock to it; the log starts over */
static int checkpoint(void)
{
    struct node **all = 0; int n = 0, cap = 0;
    fs_lock();
    uint32_t gen = fs_generation();
    walk_all(fs_root(), &all, &n, &cap);
    fs_unlock();
    int err = 0;
    for (int i = 0; i < n && !err; i++) if (all[i]->type == N_REG && all[i]->ddirty) err = write_back(all[i]);
    struct buf b = { 0 };
    fs_lock();
    if (!err) err = put_node(&b, fs_root(), 0, 0, 0);
    for (int i = 0; i < n && !err; i++) {
        struct node *x = all[i];
        if (x->unlinked) continue;
        if (x->type == N_REG && x->ddirty) gen = 0;     /* changed while we wrote: the next commit has it */
        err = put_node(&b, x, x->dsize, x->dblk, x->dnblk);
    }
    uint64_t ni = next_ino();
    fs_unlock();
    uint64_t nb = (b.n + BS - 1) / BS, at = 0;
    size_t bytes = b.n;
    if (!err && b.n) {
        memset(b.p + b.n, 0, nb * BS - b.n);             /* (the buffer is whole pages: room for the padding) */
        at = alloc_ext(nb);
        if (!at) err = -28;
        else if (io(at, nb, b.p, 1) < 0) { free_ext(at, nb); err = -5; }
    }
    if (!err) {
        uint64_t old = fs.sb.ckpt_blk, oldn = (fs.sb.ckpt_bytes + BS - 1) / BS;
        fs.sb.ckpt_blk = at;
        fs.sb.ckpt_bytes = bytes;
        fs.sb.ckpt_crc = crc32(b.p, bytes);
        fs.sb.log_seq0 = fs.seq;
        fs.sb.next_ino = ni;
        if (write_sb() < 0) { free_ext(at, nb); err = -5; }
        else {
            later_free(old, oldn);
            for (int i = 0; i < npend; i++) free_ext(pend[i].s, pend[i].n);
            npend = 0;
            fs.log_head = fs.sb.log_start;
            if (gen) fs.committed = gen;
            fs.checkpoints++;
            need_ckpt = 0;
        }
    }
    bfree(&b);
    for (int i = 0; i < n; i++) fs_put(all[i]);
    if (all) rs_kfree(all);
    return err;
}

/* ── commits: what changed since the last one, as one log segment ───── */
static void changed_since(struct node *d, uint32_t since, struct node ***arr, int *n, int *cap)
{
    for (struct node *x = d->child; x; x = x->next) {
        if (!kept(x)) continue;
        if (x->wgen > since) {
            if (grow((void **)arr, cap, *n + 1, sizeof **arr) < 0) return;
            (*arr)[(*n)++] = x;
            x->refs++;
        }
        if (x->type == N_DIR) changed_since(x, since, arr, n, cap);
    }
}

static uint64_t max_ino;
static uint64_t next_ino(void) { return max_ino + 1; }



int auxofs_commit(int force_checkpoint)
{
    if (!fs.mounted) return -1;
    lock(&fs.cm_owner);
    if (need_ckpt) force_checkpoint = 1;
    struct node **ch = 0; int n = 0, cap = 0;
    fs_lock();
    uint32_t gen = fs_generation();
    if (gen == fs.committed && !ndels && !force_checkpoint) { fs_unlock(); unlock(&fs.cm_owner); return 0; }
    changed_since(fs_root(), fs.committed, &ch, &n, &cap);
    struct del *dl = dels; int nd = ndels;
    dels = 0; ndels = capdels = 0;
    fs_unlock();
    /* deleted files' blocks are free once their deletion is on the disk */
    for (int i = 0; i < nd; i++) { later_free(dl[i].blk, dl[i].nblk); dl[i].blk = 0; dl[i].nblk = 0; }
    int err = 0;
    for (int i = 0; i < n && err != -5; i++) if (ch[i]->type == N_REG && ch[i]->ddirty) { int r = write_back(ch[i]); if (r < 0) err = r; }
    struct buf b = { 0 };
    struct seg h;
    memset(&h, 0, sizeof h);
    int oom = bput(&b, &h, sizeof h) < 0;                    /* (filled in below) */
    for (int i = 0; i < nd && !oom; i++) {                   /* deletions first: a name can be reused in the same commit */
        struct rec r;
        memset(&r, 0, sizeof r);
        r.kind = R_DELETE; r.ino = dl[i].ino;
        oom = bput(&b, &r, sizeof r) < 0;
    }
    fs_lock();
    for (int i = 0; i < n && !oom; i++) {
        struct node *x = ch[i];
        if (x->unlinked || !kept(x)) continue;
        if (x->type == N_REG && x->ddirty) gen = gen < fs.committed ? gen : fs.committed;   /* not written yet: stays "changed" */
        if (x->ino > max_ino) max_ino = x->ino;
        oom = put_node(&b, x, x->dsize, x->dblk, x->dnblk) < 0;
    }
    fs_unlock();
    int done = 0;
    if (oom) err = -12;
    else if (err != -5 && !force_checkpoint) {
        uint64_t nb = (b.n + BS - 1) / BS;
        if (fs.log_head + nb + 1 <= fs.sb.log_start + fs.sb.log_blocks) {
            memcpy(h.magic, "AUXOLOG1", 8);
            h.seq = fs.seq;
            h.bytes = (uint32_t)(b.n - sizeof h);
            h.crc = crc32(b.p + sizeof h, h.bytes);
            h.hcrc = crc32(&h, offsetof(struct seg, hcrc));
            memcpy(b.p, &h, sizeof h);
            memset(b.p + b.n, 0, nb * BS - b.n);
            if (io(fs.log_head, nb, b.p, 1) == 0) {
                fs.log_head += nb;
                fs.seq++;
                static uint8_t zero[BS];                     /* the next block mustn't look like a segment */
                io(fs.log_head, 1, zero, 1);
                for (int i = 0; i < npend; i++) free_ext(pend[i].s, pend[i].n);
                npend = 0;
                fs.committed = gen;
                fs.commits++;
                done = 1;
            } else err = -5;
        }
    }
    bfree(&b);
    if (!done && !err) {                                     /* the log is full (or asked): a checkpoint covers it all */
        fs.seq++;
        err = checkpoint();
    }
    if (dl) rs_kfree(dl);
    for (int i = 0; i < n; i++) fs_put(ch[i]);
    if (ch) rs_kfree(ch);
    unlock(&fs.cm_owner);
    if (err) {
        /* what didn't go down goes next time (the changed nodes are collected again); deletions
           that were lost make the next commit a checkpoint, which holds the whole tree */
        if (!done && nd) need_ckpt = 1;
        kprintf("auxofs: couldn't save changes to %s (%s)\n", fs.d->name, err == -28 ? "the disk is full" : err == -12 ? "out of memory" : "write error");
    }
    return err;
}

static void commit_thread(uintptr_t arg)
{
    (void)arg;
    for (;;) {
        rs_sleep(COMMIT_SECONDS * AUXO_HZ);
        auxofs_commit(0);
        /* keep an eighth of memory free for programs: files nobody's using go back to the disk */
        size_t total = pmm_total_pages(), free = pmm_free_pages();
        if (free < total / 8) auxofs_reclaim(total / 8 - free);
    }
}

/* ── mounting ───────────────────────────────────────────────────────── */
struct slot { uint64_t ino; struct node *n; };
static struct slot *tab; static uint64_t tabn;

static struct node *find(uint64_t ino)
{
    if (ino == 1) return fs_root();
    for (uint64_t i = (ino * 0x9E3779B97F4A7C15ULL) % tabn, k = 0; k < tabn; i = (i + 1) % tabn, k++) {
        if (!tab[i].ino) return 0;
        if (tab[i].ino == ino) return tab[i].n;
    }
    return 0;
}

static void remember(uint64_t ino, struct node *n)
{
    if (ino == 1) return;
    for (uint64_t i = (ino * 0x9E3779B97F4A7C15ULL) % tabn, k = 0; k < tabn; i = (i + 1) % tabn, k++)
        if (!tab[i].ino || tab[i].ino == ino) { tab[i].ino = ino; tab[i].n = n; return; }
}

static void forget(uint64_t ino)
{
    for (uint64_t i = (ino * 0x9E3779B97F4A7C15ULL) % tabn, k = 0; k < tabn; i = (i + 1) % tabn, k++) {
        if (!tab[i].ino) return;
        if (tab[i].ino == ino) { tab[i].n = 0; return; }   /* (left as a tombstone: lookups go past it) */
    }
}

static void free_tree_data(struct node *n)
{
    if (n->type == N_DIR) while (n->child) { struct node *c = n->child; fs_detach_node(c); free_tree_data(c); }
    if (n->cap && n->data) for (uint64_t o = 0; o < n->cap; o += PAGE_SIZE) pmm_free_page((uint64_t)(uintptr_t)n->data + o);
    else if (n->type == N_LNK && n->data) rs_kfree(n->data);
    rs_kfree(n->name);
    rs_kfree(n);
}

/* apply one record (fs_lock held) */
static void apply(const struct rec *r, const char *name, const char *link)
{
    if (r->kind == R_DELETE) {
        struct node *x = find(r->ino);
        if (!x || x == fs_root()) return;
        fs_detach_node(x);
        forget(r->ino);
        free_tree_data(x);
        return;
    }
    if (r->ino > max_ino) max_ino = r->ino;
    struct node *x = find(r->ino);
    if (r->ino == 1) x = fs_root();
    else {
        struct node *dir = find(r->parent);
        if (!dir || dir->type != N_DIR) return;              /* (an orphan: its directory went) */
        char nm[256];
        size_t l = r->nlen < 255 ? r->nlen : 255;
        memcpy(nm, name, l); nm[l] = 0;
        if (!x) {
            x = fs_child(dir, nm);                           /* a directory the kernel made already (/home …) */
            if (x && x->type != r->type) { fs_detach_node(x); forget(x->ino); free_tree_data(x); x = 0; }
            if (x) forget(x->ino);
            if (!x) x = fs_mknode(dir, nm, r->type, r->mode);
            if (!x) return;
            x->ino = (uint32_t)r->ino;
            remember(r->ino, x);
        } else if (x->parent != dir || strcmp(x->name, nm)) {   /* renamed or moved */
            struct node *there = fs_child(dir, nm);
            if (there && there != x) { fs_detach_node(there); forget(there->ino); free_tree_data(there); }
            fs_detach_node(x);
            fs_attach_node(x, dir, nm);
        }
    }
    x->mode = r->mode; x->uid = r->uid; x->gid = r->gid; x->mtime = r->mtime;
    if (r->type == N_REG) {
        if (x->cap && x->data) { for (uint64_t o = 0; o < x->cap; o += PAGE_SIZE) pmm_free_page((uint64_t)(uintptr_t)x->data + o); }
        x->data = 0; x->cap = 0;
        x->size = x->dsize = r->size; x->dblk = r->blk; x->dnblk = r->nblk;
    } else if (r->type == N_LNK) {
        if (x->data && !x->cap) rs_kfree(x->data);
        x->data = 0; x->size = 0;
        if (r->llen) { x->data = rs_kmalloc(r->llen + 1u); if (x->data) { memcpy(x->data, link, r->llen); x->data[r->llen] = 0; x->size = r->llen; } }
        x->cap = 0;
    }
    x->changed = 0; x->ddirty = 0; x->wgen = 0;
}

static int apply_all(const uint8_t *p, size_t n)
{
    size_t o = 0;
    int count = 0;
    while (o + sizeof(struct rec) <= n) {
        struct rec r;
        memcpy(&r, p + o, sizeof r);
        o += sizeof r;
        if ((r.kind != R_NODE && r.kind != R_DELETE) || o + r.nlen + r.llen > n) return -1;
        apply(&r, (const char *)p + o, (const char *)p + o + r.nlen);
        o += r.nlen + r.llen;
        count++;
    }
    return count;
}

/* the extents the tree uses aren't free */
static void claim(struct node *d)
{
    for (struct node *x = d->child; x; x = x->next) {
        if (x->type == N_REG && x->dnblk) take(x->dblk, x->dnblk);
        if (x->type == N_DIR) claim(x);
    }
}

static int read_sb(int slot, struct sb *out)
{
    static uint8_t blk[BS];
    if (io((uint64_t)slot, 1, blk, 0) < 0) return -1;
    memcpy(out, blk, sizeof *out);
    if (memcmp(out->magic, "AUXOFS01", 8) || out->bs != BS || out->crc != crc32(out, offsetof(struct sb, crc))) return -1;
    return 0;
}

static int setup(struct disk *d, uint64_t base, uint64_t nsect)
{
    memset(&fs, 0, sizeof fs);
    fs.io_owner = fs.cm_owner = -1;
    fs.d = d; fs.base = base; fs.nblocks = nsect / SPB;
    if (!fs.rbuf) fs.rbuf = (uint8_t *)(uintptr_t)pmm_alloc_contig(IOBUF / PAGE_SIZE);
    if (!fs.wbuf) fs.wbuf = (uint8_t *)(uintptr_t)pmm_alloc_contig(IOBUF / PAGE_SIZE);
    return fs.rbuf && fs.wbuf ? 0 : -1;
}

int auxofs_probe(struct disk *d, uint64_t base)
{
    uint8_t *s = rs_kmalloc(512);
    if (!s) return 0;
    int ok = d->read(d, base, 1, s) == 0 && !memcmp(s, "AUXOFS01", 8);
    if (!ok && d->read(d, base + SPB, 1, s) == 0 && !memcmp(s, "AUXOFS01", 8)) ok = 1;
    rs_kfree(s);
    return ok;
}

int auxofs_mount(struct disk *d, uint64_t base, uint64_t nsect)
{
    if (setup(d, base, nsect) < 0) return -12;
    struct sb a, b;
    int ra = read_sb(0, &a), rb = read_sb(1, &b);
    if (ra < 0 && rb < 0) return -22;
    fs.sb = ra < 0 ? b : rb < 0 ? a : a.gen > b.gen ? a : b;
    fs.slot = (int)(fs.sb.gen & 1);
    nfr = 0; npend = 0; ndels = 0;
    /* the tree: checkpoint, then the log */
    uint64_t cb = (fs.sb.ckpt_bytes + BS - 1) / BS;
    uint8_t *cp = 0;
    uint64_t cpages = (cb * BS + PAGE_SIZE - 1) / PAGE_SIZE;
    if (cb) {
        uint64_t pg = pmm_alloc_contig_any(cpages);
        if (!pg) return -12;
        cp = (uint8_t *)(uintptr_t)pg;
        if (io(fs.sb.ckpt_blk, cb, cp, 0) < 0 || crc32(cp, fs.sb.ckpt_bytes) != fs.sb.ckpt_crc) {
            kprintf("auxofs: the checkpoint on %s is damaged\n", d->name);
            for (uint64_t i = 0; i < cpages; i++) pmm_free_page(pg + i * PAGE_SIZE);
            return -5;
        }
    }
    tabn = 1 << 16;
    while (tabn < fs.sb.ckpt_bytes / 32 + 4096) tabn <<= 1;
    tabn *= 2;
    uint64_t tpages = (tabn * sizeof *tab + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t tpg = pmm_alloc_contig_any(tpages);
    if (!tpg) return -12;
    tab = (struct slot *)(uintptr_t)tpg;
    memset(tab, 0, tpages * PAGE_SIZE);
    max_ino = fs.sb.next_ino ? fs.sb.next_ino - 1 : 1;
    fs_lock();
    fs_set_mounting(1);
    int nodes = cp ? apply_all(cp, fs.sb.ckpt_bytes) : 0;
    fs_set_mounting(0);
    fs_unlock();
    if (cp) for (uint64_t i = 0; i < cpages; i++) pmm_free_page((uint64_t)(uintptr_t)cp + i * PAGE_SIZE);
    /* replay */
    fs.seq = fs.sb.log_seq0;
    fs.log_head = fs.sb.log_start;
    int segs = 0;
    static uint8_t first[BS];
    while (fs.log_head < fs.sb.log_start + fs.sb.log_blocks) {
        if (io(fs.log_head, 1, first, 0) < 0) break;
        struct seg h;
        memcpy(&h, first, sizeof h);
        if (memcmp(h.magic, "AUXOLOG1", 8) || h.seq != fs.seq || h.hcrc != crc32(&h, offsetof(struct seg, hcrc))) break;
        uint64_t nb = (sizeof h + h.bytes + BS - 1) / BS;
        if (fs.log_head + nb > fs.sb.log_start + fs.sb.log_blocks) break;
        uint64_t pages = (nb * BS + PAGE_SIZE - 1) / PAGE_SIZE, pg = pmm_alloc_contig_any(pages);
        if (!pg) break;
        uint8_t *sp = (uint8_t *)(uintptr_t)pg;
        int ok = io(fs.log_head, nb, sp, 0) == 0 && crc32(sp + sizeof h, h.bytes) == h.crc;
        if (ok) {
            fs_lock(); fs_set_mounting(1);
            apply_all(sp + sizeof h, h.bytes);
            fs_set_mounting(0); fs_unlock();
        }
        for (uint64_t i = 0; i < pages; i++) pmm_free_page(pg + i * PAGE_SIZE);
        if (!ok) break;                                      /* cut short by a crash: stop here */
        fs.log_head += nb; fs.seq++; segs++;
    }
    for (uint64_t i = 0; i < tpages; i++) pmm_free_page(tpg + i * PAGE_SIZE);
    tab = 0;
    /* free space: everything but the superblocks, the log, the checkpoint and the files */
    free_ext(2 + fs.sb.log_blocks, fs.nblocks - 2 - fs.sb.log_blocks);
    take(fs.sb.ckpt_blk, cb);
    fs_lock();
    claim(fs_root());
    fs.committed = fs_generation();
    fs_ino_floor((uint32_t)max_ino + 1);
    fs_unlock();
    fs_loader = load;
    fs_on_delete = deleted;
    fs.mounted = 1;
    kprintf("AuxoFS on %s, %d files and folders (+%d recent changes), %lu MB free of %lu MB\n", d->name, nodes, segs,
            (unsigned long)(auxofs_free_blocks() * BS >> 20), (unsigned long)(fs.nblocks * BS >> 20));
    if (segs > 0) auxofs_commit(1);                         /* fold the replayed log into a checkpoint */
    rs_spawn("auxofs", commit_thread, 0);
    return 0;
}

/* ── making one (the installer): format, then the live system's tree goes in ── */
int auxofs_format_copy(struct disk *d, uint64_t base, uint64_t nsect, void (*progress)(int pct))
{
    if (setup(d, base, nsect) < 0) return -12;
    if (fs.nblocks < 65536) return -28;                     /* 256 MiB at the least */
    memset(&fs.sb, 0, sizeof fs.sb);
    memcpy(fs.sb.magic, "AUXOFS01", 8);
    fs.sb.version = 1; fs.sb.bs = BS; fs.sb.nblocks = fs.nblocks;
    fs.sb.log_start = 2;
    fs.sb.log_blocks = fs.nblocks / 100 > LOG_MIN ? (fs.nblocks / 100 < 65536 ? fs.nblocks / 100 : 65536) : LOG_MIN;
    fs.sb.gen = 1;
    nfr = 0; npend = 0;
    free_ext(2 + fs.sb.log_blocks, fs.nblocks - 2 - fs.sb.log_blocks);
    static uint8_t zero[BS];
    io(0, 1, zero, 1); io(1, 1, zero, 1); io(2, 1, zero, 1);  /* no old superblocks or log to confuse a mount */
    /* every file's contents go down (the live tree's are in memory: from the image, or written since) */
    struct node **all = 0; int n = 0, cap = 0;
    fs_lock();
    walk_all(fs_root(), &all, &n, &cap);
    uint64_t total = 0;
    for (int i = 0; i < n; i++) if (all[i]->type == N_REG) { all[i]->ddirty = 1; all[i]->dblk = 0; all[i]->dnblk = 0; total += all[i]->size; if (all[i]->ino > max_ino) max_ino = all[i]->ino; }
    fs_unlock();
    fs.mounted = 1;                                          /* (for write_back; not the root yet) */
    uint64_t done = 0;
    int err = 0;
    for (int i = 0; i < n && !err; i++) {
        if (all[i]->type != N_REG) continue;
        err = write_back(all[i]);
        done += all[i]->size;
        if (progress && total) progress((int)(done * 100 / total));
    }
    if (!err) { fs.seq = 1; err = checkpoint(); }
    /* the live system goes on in memory: these extents mean nothing to it */
    fs_lock();
    for (int i = 0; i < n; i++) { all[i]->dblk = 0; all[i]->dnblk = 0; all[i]->ddirty = 0; }
    fs_unlock();
    for (int i = 0; i < n; i++) fs_put(all[i]);
    if (all) rs_kfree(all);
    fs.mounted = 0;
    return err;
}

/* boot: the root filesystem is on one of the disks (an MBR partition of type 0xDA) */
int auxofs_mount_root(void)
{
    uint8_t *s = rs_kmalloc(512);
    if (!s) return -12;
    for (int i = 0; i < disk_count(); i++) {
        struct disk *d = disk_get(i);
        if (d->gone || d->read(d, 0, 1, s) < 0 || s[510] != 0x55 || s[511] != 0xAA) continue;
        for (int p = 0; p < 4; p++) {
            const uint8_t *e = s + 446 + p * 16;
            if (e[4] != 0xDA) continue;
            uint32_t start, size;
            memcpy(&start, e + 8, 4); memcpy(&size, e + 12, 4);
            if (!auxofs_probe(d, start)) continue;
            rs_kfree(s);
            int r = auxofs_mount(d, start, size);
            if (r == 0) d->boot = 1;
            return r;
        }
    }
    rs_kfree(s);
    return -2;
}

int auxofs_mounted(void) { return fs.mounted; }
const char *auxofs_disk(void) { return fs.mounted ? fs.d->name : 0; }
void auxofs_stats(uint64_t *loads, uint64_t *evictions, uint64_t *commits, uint64_t *checkpoints)
{ *loads = fs.loads; *evictions = fs.evictions; *commits = fs.commits; *checkpoints = fs.checkpoints; }
