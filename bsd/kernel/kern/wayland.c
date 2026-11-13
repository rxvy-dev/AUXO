/* Chrono's Wayland server: lets ordinary Wayland programs (GTK 4 apps such as Aether) open
 * windows on the AuxoBSD desktop.
 *
 * It listens on /run/user/UID/wayland-0 (an AF_UNIX socket, kern/unix.c) and implements the core
 * protocol the toolkits need: wl_compositor + wl_surface, wl_shm (client pixels in shared
 * memfd pages, read straight from those pages), wl_seat with pointer and keyboard (Linux
 * keycodes, an XKB keymap compiled by the client's libxkbcommon), wl_output, xdg_wm_base with
 * toplevels (client-side decorations, interactive move/resize, maximize) and popups, and a
 * clipboard manager stub. Runs on the Chrono thread. */
#include "kern.h"
#include "rs.h"
#include "fs.h"
#include "uproc.h"
#include "unix.h"
#include "gfx.h"
#include "wayland.h"
#include "vfs.h"
#include "cred.h"
#include "session.h"

#define MAXCLIENT 8
#define MAXOBJ 2048
#define MAXSURF 64
#define MAXBUF 128
#define MAXPOOL 64
#define MAXXS 32

enum { O_NONE, O_DISPLAY, O_REGISTRY, O_CALLBACK, O_COMPOSITOR, O_SURFACE, O_REGION, O_SHM, O_POOL, O_BUFFER,
       O_SEAT, O_POINTER, O_KEYBOARD, O_OUTPUT, O_WM_BASE, O_POSITIONER, O_XDG_SURFACE, O_TOPLEVEL, O_POPUP,
       O_DDM, O_DATA_DEVICE, O_DATA_SOURCE, O_INERT };

struct obj { uint8_t type, version; int16_t idx; };

struct client {
    int used;
    struct usock *s;
    struct obj *obj;                  /* indexed by object id */
    uint8_t *in; int inlen;
    struct file *fds[32]; int nfds;
    int dead;
};

struct pool { int used, client; struct file *f; uint64_t size; };
struct buffer { int used, client, id, pool, off, w, h, stride, fmt; };
struct positioner { int w, h, ax, ay, aw, ah, anchor, gravity, ox, oy; };
struct surface {
    int used, client, id;
    int pending_buf, cur_buf, attached;          /* buffer slots, -1 none */
    uint32_t pending_cb[16]; int npending_cb;
    uint32_t frame_cb[16]; int nframe_cb;
    int xs;                                      /* xdg surface slot, -1 */
};
struct xsurf {
    int used, client, id, surf;
    int role;                                    /* 1 toplevel, 2 popup */
    int role_id;
    int gx, gy, gw, gh, has_geom;
    uint32_t serial; int configured, mapped;
    int win;                                     /* Chrono window (toplevels) */
    char title[96];
    char app_id[64];
    int maximized;
    /* popups */
    int parent; struct positioner pos; int px, py;   /* px,py: relative to the parent's geometry */
    int ax, ay;                                  /* absolute screen position of the geometry */
};

static struct client cl[MAXCLIENT];
static struct pool pools[MAXPOOL];
static struct buffer bufs[MAXBUF];
static struct surface surfs[MAXSURF];
static struct xsurf xss[MAXXS];
static struct positioner posns[64];
static int nposn;
static struct usock *listener;
static uint32_t serial = 1;
static int screen_w, screen_h, dirty;
static int kb_focus = -1, ptr_focus = -1;        /* xsurf slots */
static uint32_t mods_dep, mods_lock;
static struct node *keymap_node;

/* ── output buffer ──────────────────────────────────────────────────── */
static uint8_t ob[8192];
static int on, ostart;

static void ev(uint32_t id, uint16_t op) { ostart = on; uint32_t h[2] = { id, op }; memcpy(ob + on, h, 8); on += 8; }
static void u32(uint32_t v) { memcpy(ob + on, &v, 4); on += 4; }
static void i32(int32_t v) { memcpy(ob + on, &v, 4); on += 4; }
static void fixed(int v) { i32(v * 256); }
static void str(const char *s)
{
    uint32_t l = (uint32_t)strlen(s) + 1, padded = (l + 3) & ~3u;
    u32(l);
    memset(ob + on, 0, padded);
    memcpy(ob + on, s, l);
    on += (int)padded;
}
static void arr(const void *d, uint32_t l)
{
    u32(l);
    memset(ob + on, 0, (l + 3) & ~3u);
    if (l) memcpy(ob + on, d, l);
    on += (int)((l + 3) & ~3u);
}
static void end(void) { uint32_t w = (uint32_t)(on - ostart) << 16 | *(uint32_t *)(ob + ostart + 4); memcpy(ob + ostart + 4, &w, 4); }

static void flush_to(int c, struct file **files, int nf)
{
    if (!on) return;
    if (cl[c].used && !cl[c].dead) {
        long r = unix_send(cl[c].s, ob, (size_t)on, files, nf);
        if (r < on) cl[c].dead = 1;              /* the client stopped reading: drop it */
    }
    on = 0;
}

static uint32_t now_ms(void) { return (uint32_t)(kernel_ticks() * (1000 / AUXO_HZ)); }

static void delete_id(int c, uint32_t id)
{
    if (id < MAXOBJ) cl[c].obj[id].type = O_NONE;
    ev(1, 1); u32(id); end();
}

/* ── objects ────────────────────────────────────────────────────────── */
static int new_obj(int c, uint32_t id, int type, int version, int idx)
{
    if (id == 0 || id >= MAXOBJ) { cl[c].dead = 1; return -1; }
    cl[c].obj[id] = (struct obj){ (uint8_t)type, (uint8_t)version, (int16_t)idx };
    return 0;
}

static int alloc_slot(void *arr_, int n, size_t sz)
{
    for (int i = 0; i < n; i++) { int *used = (int *)((uint8_t *)arr_ + (size_t)i * sz); if (!*used) { memset(used, 0, sz); *used = 1; return i; } }
    return -1;
}

/* ── globals ────────────────────────────────────────────────────────── */
static const struct { const char *iface; int version, type; } GLOBALS[] = {
    { "wl_compositor", 5, O_COMPOSITOR }, { "wl_shm", 1, O_SHM }, { "wl_seat", 7, O_SEAT },
    { "wl_output", 3, O_OUTPUT }, { "xdg_wm_base", 3, O_WM_BASE }, { "wl_data_device_manager", 3, O_DDM },
};
#define NGLOBALS (int)(sizeof GLOBALS / sizeof *GLOBALS)

#include "xkb_keymap.h"
#define KEYMAP XKB_KEYMAP

static void send_keymap(int c, uint32_t kid)
{
    if (!keymap_node) {
        keymap_node = rs_kmalloc(sizeof *keymap_node);
        if (!keymap_node) return;
        memset(keymap_node, 0, sizeof *keymap_node);
        keymap_node->name = rs_kmalloc(8);
        if (keymap_node->name) strlcpy(keymap_node->name, "keymap", 8);
        keymap_node->type = N_REG; keymap_node->mode = 0444; keymap_node->unlinked = 1;
        keymap_node->refs = 1;                   /* ours, forever */
        fs_write(keymap_node, 0, KEYMAP, sizeof KEYMAP);
    }
    struct file *f = file_new(0, keymap_node, 0);
    if (!f) return;
    ev(kid, 0); u32(1); u32(sizeof KEYMAP); end();          /* keymap(XKB_V1, fd, size) */
    flush_to(c, &f, 1);
    file_put(f);
    if (cl[c].obj[kid].version >= 4) { ev(kid, 5); i32(30); i32(400); end(); }   /* repeat_info */
}

static void bind_global(int c, int g, uint32_t id, int version)
{
    int type = GLOBALS[g].type;
    if (version > GLOBALS[g].version) version = GLOBALS[g].version;
    if (new_obj(c, id, type, version, 0) < 0) return;
    if (type == O_SHM) { ev(id, 0); u32(0); end(); ev(id, 0); u32(1); end(); }
    else if (type == O_SEAT) { ev(id, 0); u32(3); end(); if (version >= 2) { ev(id, 1); str("seat0"); end(); } }
    else if (type == O_OUTPUT) {
        ev(id, 0); i32(0); i32(0); i32(screen_w * 254 / 960); i32(screen_h * 254 / 960); i32(0); str("AuxoBSD"); str("Chrono"); i32(0); end();
        ev(id, 1); u32(3); i32(screen_w); i32(screen_h); i32(60000); end();
        if (version >= 2) { ev(id, 3); i32(1); end(); ev(id, 2); end(); }
    }
}

/* ── xdg ────────────────────────────────────────────────────────────── */
static void send_configure(int x)
{
    struct xsurf *s = &xss[x];
    int c = s->client;
    s->serial = serial++;
    if (s->role == 1) {
        uint32_t states[4]; int n = 0;
        if (s->maximized) states[n++] = 1;
        if (kb_focus == x) states[n++] = 4;
        int w = 0, h = 0;
        if (s->maximized) { chrono_wl_max_size(&w, &h); }
        else if (!s->mapped) {
            /* first show: suggest a size that fits the screen (the client may pick smaller) */
            int mw, mh;
            chrono_wl_max_size(&mw, &mh);
            w = mw - 60 > 1200 ? 1200 : mw - 60;
            h = mh - 50 > 800 ? 800 : mh - 50;
        }
        ev((uint32_t)s->role_id, 0); i32(w); i32(h); arr(states, (uint32_t)n * 4); end();
    } else if (s->role == 2) {
        ev((uint32_t)s->role_id, 0); i32(s->px); i32(s->py); i32(s->pos.w); i32(s->pos.h); end();
    }
    ev((uint32_t)s->id, 0); u32(s->serial); end();
    flush_to(c, 0, 0);
}

/* where a popup goes, relative to its parent's window geometry (xdg_positioner rules) */
static void place_popup(int x)
{
    struct xsurf *s = &xss[x];
    struct positioner *p = &s->pos;
    int ax = p->ax + p->aw / 2, ay = p->ay + p->ah / 2;
    switch (p->anchor) {
    case 1: ay = p->ay; break;                case 2: ay = p->ay + p->ah; break;
    case 3: ax = p->ax; break;                case 4: ax = p->ax + p->aw; break;
    case 5: ax = p->ax; ay = p->ay; break;    case 6: ax = p->ax; ay = p->ay + p->ah; break;
    case 7: ax = p->ax + p->aw; ay = p->ay; break; case 8: ax = p->ax + p->aw; ay = p->ay + p->ah; break;
    }
    int x0 = ax - p->w / 2, y0 = ay - p->h / 2;
    switch (p->gravity) {
    case 1: y0 = ay - p->h; break;            case 2: y0 = ay; break;
    case 3: x0 = ax - p->w; break;            case 4: x0 = ax; break;
    case 5: x0 = ax - p->w; y0 = ay - p->h; break; case 6: x0 = ax - p->w; y0 = ay; break;
    case 7: x0 = ax; y0 = ay - p->h; break;   case 8: x0 = ax; y0 = ay; break;
    }
    x0 += p->ox; y0 += p->oy;
    /* absolute position of the parent's geometry */
    struct xsurf *par = &xss[s->parent];
    int pax = par->ax, pay = par->ay;
    if (par->role == 1) chrono_wl_win_pos(par->win, &pax, &pay);
    /* keep it on screen: slide */
    if (pax + x0 + p->w > screen_w) x0 = screen_w - p->w - pax;
    if (pay + y0 + p->h > screen_h) {
        int flipped = p->ay - p->h - (y0 - (p->ay + p->ah));                 /* flip above the anchor */
        y0 = pay + flipped >= 0 ? flipped : screen_h - p->h - pay;
    }
    if (pax + x0 < 0) x0 = -pax;
    if (pay + y0 < 0) y0 = -pay;
    s->px = x0; s->py = y0;
    s->ax = pax + x0; s->ay = pay + y0;
}

static void destroy_xsurf(int x)
{
    struct xsurf *s = &xss[x];
    if (!s->used) return;
    if (s->role == 1 && s->win >= 0) chrono_wl_close(s->win);
    for (int i = 0; i < MAXXS; i++) if (xss[i].used && xss[i].role == 2 && xss[i].parent == x) destroy_xsurf(i);
    if (kb_focus == x) kb_focus = -1;
    if (ptr_focus == x) ptr_focus = -1;
    if (s->surf >= 0 && surfs[s->surf].used) surfs[s->surf].xs = -1;
    s->used = 0;
    dirty = 1;
}

/* ── requests ───────────────────────────────────────────────────────── */
struct args { const uint8_t *p, *e; int c; };
static uint32_t a_u32(struct args *a) { uint32_t v = 0; if (a->p + 4 <= a->e) memcpy(&v, a->p, 4); a->p += 4; return v; }
static int32_t a_i32(struct args *a) { return (int32_t)a_u32(a); }
static const char *a_str(struct args *a)
{
    uint32_t l = a_u32(a);
    const char *s = (const char *)a->p;
    a->p += (l + 3) & ~3u;
    if (a->p > a->e || !l) return "";
    return s;
}
static struct file *a_fd(struct args *a)
{
    struct client *k = &cl[a->c];
    if (!k->nfds) return 0;
    struct file *f = k->fds[0];
    memmove(k->fds, k->fds + 1, sizeof k->fds[0] * (size_t)(k->nfds - 1));
    k->nfds--;
    return f;
}

static void release_buffer(int c, int b)
{
    (void)c;
    if (b < 0 || !bufs[b].used) return;
    ev((uint32_t)bufs[b].id, 0); end();
}

static void surface_commit(int c, int si)
{
    struct surface *s = &surfs[si];
    for (int i = 0; i < s->npending_cb && s->nframe_cb < 16; i++) s->frame_cb[s->nframe_cb++] = s->pending_cb[i];
    s->npending_cb = 0;
    if (s->attached) {
        if (s->cur_buf >= 0 && s->cur_buf != s->pending_buf) release_buffer(c, s->cur_buf);
        s->cur_buf = s->pending_buf;
        s->attached = 0;
    }
    int x = s->xs;
    if (x < 0) {
        /* no role we draw (cursor images, drag icons): give the pixels straight back */
        if (s->cur_buf >= 0) { release_buffer(c, s->cur_buf); s->cur_buf = -1; }
        flush_to(c, 0, 0);
        return;
    }
    struct xsurf *xs = &xss[x];
    if (!xs->configured) { xs->configured = 1; if (xs->role == 2) place_popup(x); send_configure(x); return; }
    if (s->cur_buf >= 0) {
        struct buffer *b = &bufs[s->cur_buf];
        if (!xs->has_geom) { xs->gx = 0; xs->gy = 0; xs->gw = b->w; xs->gh = b->h; }
        if (xs->role == 1) {
            if (!xs->mapped) { xs->win = chrono_wl_new(x, xs->gw, xs->gh, xs->title); xs->mapped = 1; kb_focus = -1; }
            else chrono_wl_resized(xs->win, xs->gw, xs->gh);
        } else xs->mapped = 1;
    } else if (xs->mapped && xs->role == 1) { chrono_wl_close(xs->win); xs->mapped = 0; xs->win = -1; }
    dirty = 1;
    flush_to(c, 0, 0);
}

static void request(int c, uint32_t id, uint16_t op, struct args *a)
{
    struct client *k = &cl[c];
    if (id >= MAXOBJ) { k->dead = 1; return; }
    struct obj *o = &k->obj[id];
    switch (o->type) {
    case O_DISPLAY:
        if (op == 0) { uint32_t cb = a_u32(a); ev(cb, 0); u32(serial++); end(); delete_id(c, cb); }
        else if (op == 1) {
            uint32_t rid = a_u32(a);
            new_obj(c, rid, O_REGISTRY, 1, 0);
            for (int g = 0; g < NGLOBALS; g++) { ev(rid, 0); u32((uint32_t)g + 1); str(GLOBALS[g].iface); u32((uint32_t)GLOBALS[g].version); end(); }
        }
        break;
    case O_REGISTRY:
        if (op == 0) {
            uint32_t name = a_u32(a);
            const char *iface = a_str(a);
            uint32_t ver = a_u32(a), nid = a_u32(a);
            (void)iface;
            if (name >= 1 && name <= (uint32_t)NGLOBALS) bind_global(c, (int)name - 1, nid, (int)ver);
            else new_obj(c, nid, O_INERT, 1, 0);
        }
        break;
    case O_COMPOSITOR:
        if (op == 0) {
            uint32_t sid = a_u32(a);
            int s = alloc_slot(surfs, MAXSURF, sizeof surfs[0]);
            if (s < 0) { k->dead = 1; break; }
            surfs[s].client = c; surfs[s].id = (int)sid; surfs[s].pending_buf = surfs[s].cur_buf = -1; surfs[s].xs = -1;
            new_obj(c, sid, O_SURFACE, o->version, s);
        } else if (op == 1) new_obj(c, a_u32(a), O_REGION, 1, 0);
        break;
    case O_REGION: if (op == 0) delete_id(c, id); break;
    case O_SURFACE: {
        struct surface *s = &surfs[o->idx];
        switch (op) {
        case 0:                                                /* destroy */
            if (s->xs >= 0) destroy_xsurf(s->xs);
            if (s->cur_buf >= 0) release_buffer(c, s->cur_buf);
            s->used = 0;
            delete_id(c, id);
            dirty = 1;
            break;
        case 1: {                                              /* attach */
            uint32_t bid = a_u32(a);
            s->pending_buf = bid && bid < MAXOBJ && k->obj[bid].type == O_BUFFER ? k->obj[bid].idx : -1;
            s->attached = 1;
            break;
        }
        case 3: if (s->npending_cb < 16) { uint32_t cb = a_u32(a); s->pending_cb[s->npending_cb++] = cb; new_obj(c, cb, O_CALLBACK, 1, 0); } break;
        case 6: surface_commit(c, o->idx); break;
        }
        break;
    }
    case O_SHM:
        if (op == 0) {
            uint32_t pid = a_u32(a);
            struct file *f = a_fd(a);
            int32_t size = a_i32(a);
            int p = alloc_slot(pools, MAXPOOL, sizeof pools[0]);
            if (p < 0 || !f || !f->node) { if (f) file_put(f); k->dead = 1; break; }
            pools[p].client = c; pools[p].f = f; pools[p].size = (uint64_t)size;
            new_obj(c, pid, O_POOL, 1, p);
        }
        break;
    case O_POOL: {
        struct pool *p = &pools[o->idx];
        if (op == 0) {
            uint32_t bid = a_u32(a);
            int b = alloc_slot(bufs, MAXBUF, sizeof bufs[0]);
            if (b < 0) { k->dead = 1; break; }
            bufs[b].client = c; bufs[b].id = (int)bid; bufs[b].pool = o->idx;
            bufs[b].off = a_i32(a); bufs[b].w = a_i32(a); bufs[b].h = a_i32(a); bufs[b].stride = a_i32(a); bufs[b].fmt = (int)a_u32(a);
            new_obj(c, bid, O_BUFFER, 1, b);
        } else if (op == 1) {                                 /* destroy: buffers keep the memory alive */
            int inuse = 0;
            for (int b = 0; b < MAXBUF; b++) if (bufs[b].used && bufs[b].pool == o->idx) inuse = 1;
            if (!inuse) { file_put(p->f); p->used = 0; } else p->client = -1 - c;   /* orphaned: freed with its last buffer */
            delete_id(c, id);
        } else if (op == 2) p->size = (uint64_t)a_i32(a);
        break;
    }
    case O_BUFFER:
        if (op == 0) {
            int b = o->idx, p = bufs[b].pool;
            for (int i = 0; i < MAXSURF; i++) if (surfs[i].used && surfs[i].cur_buf == b) surfs[i].cur_buf = -1;
            bufs[b].used = 0;
            if (pools[p].used && pools[p].client < 0) {
                int inuse = 0;
                for (int q = 0; q < MAXBUF; q++) if (bufs[q].used && bufs[q].pool == p) inuse = 1;
                if (!inuse) { file_put(pools[p].f); pools[p].used = 0; }
            }
            delete_id(c, id);
            dirty = 1;
        }
        break;
    case O_SEAT:
        if (op == 0) new_obj(c, a_u32(a), O_POINTER, o->version, 0);
        else if (op == 1) { uint32_t kid = a_u32(a); new_obj(c, kid, O_KEYBOARD, o->version, 0); flush_to(c, 0, 0); send_keymap(c, kid); }
        else if (op == 2) new_obj(c, a_u32(a), O_INERT, 1, 0);
        break;
    case O_POINTER: case O_KEYBOARD: case O_OUTPUT: case O_DATA_DEVICE: case O_DATA_SOURCE: case O_INERT:
        if ((o->type == O_POINTER && op == 1) || (o->type == O_KEYBOARD && op == 0) || (o->type == O_OUTPUT && op == 0) ||
            (o->type == O_DATA_DEVICE && op == 2) || (o->type == O_DATA_SOURCE && op == 1))
            delete_id(c, id);
        break;
    case O_DDM:
        if (op == 0) new_obj(c, a_u32(a), O_DATA_SOURCE, 3, 0);
        else if (op == 1) new_obj(c, a_u32(a), O_DATA_DEVICE, 3, 0);
        break;
    case O_WM_BASE:
        if (op == 0) delete_id(c, id);
        else if (op == 1) {
            uint32_t pid = a_u32(a);
            int p = nposn++ % 64;
            memset(&posns[p], 0, sizeof posns[p]);
            new_obj(c, pid, O_POSITIONER, 1, p);
        } else if (op == 2) {
            uint32_t xid = a_u32(a), sid = a_u32(a);
            if (sid >= MAXOBJ || k->obj[sid].type != O_SURFACE) { k->dead = 1; break; }
            int x = alloc_slot(xss, MAXXS, sizeof xss[0]);
            if (x < 0) { k->dead = 1; break; }
            xss[x].client = c; xss[x].id = (int)xid; xss[x].surf = k->obj[sid].idx; xss[x].win = -1; xss[x].parent = -1;
            surfs[xss[x].surf].xs = x;
            new_obj(c, xid, O_XDG_SURFACE, o->version, x);
        }
        break;
    case O_POSITIONER: {
        struct positioner *p = &posns[o->idx];
        switch (op) {
        case 0: delete_id(c, id); break;
        case 1: p->w = a_i32(a); p->h = a_i32(a); break;
        case 2: p->ax = a_i32(a); p->ay = a_i32(a); p->aw = a_i32(a); p->ah = a_i32(a); break;
        case 3: p->anchor = (int)a_u32(a); break;
        case 4: p->gravity = (int)a_u32(a); break;
        case 6: p->ox = a_i32(a); p->oy = a_i32(a); break;
        }
        break;
    }
    case O_XDG_SURFACE: {
        struct xsurf *x = &xss[o->idx];
        switch (op) {
        case 0: destroy_xsurf(o->idx); delete_id(c, id); break;
        case 1: { uint32_t tid = a_u32(a); x->role = 1; x->role_id = (int)tid; new_obj(c, tid, O_TOPLEVEL, o->version, o->idx); break; }
        case 2: {
            uint32_t pid = a_u32(a), parent = a_u32(a), posid = a_u32(a);
            x->role = 2; x->role_id = (int)pid;
            x->parent = parent < MAXOBJ && k->obj[parent].type == O_XDG_SURFACE ? k->obj[parent].idx : -1;
            if (posid < MAXOBJ && k->obj[posid].type == O_POSITIONER) x->pos = posns[k->obj[posid].idx];
            if (x->parent < 0) { k->dead = 1; break; }
            new_obj(c, pid, O_POPUP, o->version, o->idx);
            break;
        }
        case 3: x->gx = a_i32(a); x->gy = a_i32(a); x->gw = a_i32(a); x->gh = a_i32(a); x->has_geom = 1; break;
        case 4: break;                                         /* ack_configure */
        }
        break;
    }
    case O_TOPLEVEL: {
        struct xsurf *x = &xss[o->idx];
        switch (op) {
        case 0: x->role = 0; if (x->win >= 0) chrono_wl_close(x->win); x->win = -1; x->mapped = 0; delete_id(c, id); dirty = 1; break;
        case 2: strlcpy(x->title, a_str(a), sizeof x->title); if (x->win >= 0) chrono_wl_title(x->win, x->title); dirty = 1; break;
        case 3: strlcpy(x->app_id, a_str(a), sizeof x->app_id); break;
        case 5: if (x->win >= 0) chrono_wl_move(x->win); break;
        case 6: { a_u32(a); a_u32(a); uint32_t edges = a_u32(a); if (x->win >= 0) chrono_wl_resize(x->win, (int)edges); break; }
        case 9: x->maximized = 1; if (x->win >= 0) chrono_wl_maximize(x->win, 1); send_configure(o->idx); break;
        case 10: x->maximized = 0; if (x->win >= 0) chrono_wl_maximize(x->win, 0); send_configure(o->idx); break;
        case 13: if (x->win >= 0) chrono_wl_minimize(x->win); break;
        }
        break;
    }
    case O_POPUP:
        if (op == 0) { destroy_xsurf(o->idx); delete_id(c, id); }
        else if (op == 2) {                                   /* reposition(positioner, token) */
            uint32_t posid = a_u32(a), token = a_u32(a);
            struct xsurf *x = &xss[o->idx];
            if (posid < MAXOBJ && k->obj[posid].type == O_POSITIONER) x->pos = posns[k->obj[posid].idx];
            place_popup(o->idx);
            ev((uint32_t)x->role_id, 2); u32(token); end();
            send_configure(o->idx);
        }
        break;
    case O_CALLBACK: case O_NONE: default: break;
    }
}

/* ── per-client I/O ─────────────────────────────────────────────────── */
static void drop_client(int c)
{
    struct client *k = &cl[c];
    for (int i = 0; i < MAXXS; i++) if (xss[i].used && xss[i].client == c) destroy_xsurf(i);
    for (int i = 0; i < MAXSURF; i++) if (surfs[i].used && surfs[i].client == c) surfs[i].used = 0;
    for (int i = 0; i < MAXBUF; i++) if (bufs[i].used && bufs[i].client == c) bufs[i].used = 0;
    for (int i = 0; i < MAXPOOL; i++) if (pools[i].used && (pools[i].client == c || pools[i].client == -1 - c)) { file_put(pools[i].f); pools[i].used = 0; }
    for (int i = 0; i < k->nfds; i++) file_put(k->fds[i]);
    unix_close(k->s);
    pmm_free_page((uint64_t)(uintptr_t)k->in);
    for (int i = 0; i < 16; i++) pmm_free_page((uint64_t)(uintptr_t)k->in + 4096 + (uint64_t)i * 4096);
    k->used = 0;
    dirty = 1;
}

static void client_io(int c)
{
    struct client *k = &cl[c];
    /* a bounded amount per frame: a chatty client mustn't starve the desktop */
    for (int rounds = 0; rounds < 16; rounds++) {
        struct file *files[16];
        int nf = 0;
        long r = unix_recv(k->s, k->in + k->inlen, 4096 - (size_t)k->inlen, files, &nf, 16);
        for (int i = 0; i < nf; i++) { if (k->nfds < 32) k->fds[k->nfds++] = files[i]; else file_put(files[i]); }
        if (r == 0) { k->dead = 1; break; }
        if (r < 0) break;
        k->inlen += (int)r;
        int off = 0;
        while (k->inlen - off >= 8) {
            uint32_t id, w;
            memcpy(&id, k->in + off, 4); memcpy(&w, k->in + off + 4, 4);
            uint16_t size = (uint16_t)(w >> 16), op = (uint16_t)w;
            if (size < 8) { k->dead = 1; break; }
            if (off + size > k->inlen) break;
            struct args a = { k->in + off + 8, k->in + off + size, c };
            request(c, id, op, &a);
            flush_to(c, 0, 0);
            off += size;
        }
        memmove(k->in, k->in + off, (size_t)(k->inlen - off));
        k->inlen -= off;
        if (k->dead) break;
    }
}

/* the socket goes in the session's runtime directory (/run/user/UID), once we know who that is */
static int wl_listen(void)
{
    char path[64];
    ksnprintf(path, sizeof path, "%s/wayland-0", session_runtime_dir());
    struct node *old;
    if (fs_lookup(0, path, 0, &old) == 0) vfs_remove(path);
    listener = unix_new();
    if (!listener) return -1;
    listener->kernel = 1;
    if (unix_bind(listener, path, 0) < 0) { listener = 0; return -1; }
    unix_listen(listener);
    return 0;
}

int wl_server_init(int w, int h)
{
    screen_w = w; screen_h = h;
    return 0;
}

int wl_server_poll(void)
{
    if (!listener && session_ready) { static int tried; if (!tried++) wl_listen(); }
    if (!listener) return 0;
    struct usock *n;
    while ((n = unix_accept(listener))) {
        int c = -1;
        for (int i = 0; i < MAXCLIENT; i++) if (!cl[i].used) { c = i; break; }
        if (c < 0) { unix_close(n); continue; }
        memset(&cl[c], 0, sizeof cl[c]);
        uint64_t mem = pmm_alloc_contig(17);
        uint64_t objs = pmm_alloc_contig((MAXOBJ * sizeof(struct obj) + 4095) / 4096);
        if (!mem || !objs) { unix_close(n); continue; }
        cl[c].used = 1; cl[c].s = n; cl[c].in = (uint8_t *)(uintptr_t)mem;
        cl[c].obj = (struct obj *)(uintptr_t)objs;
        memset(cl[c].obj, 0, MAXOBJ * sizeof(struct obj));
        cl[c].obj[1].type = O_DISPLAY;
    }
    for (int c = 0; c < MAXCLIENT; c++) {
        if (!cl[c].used) continue;
        client_io(c);
        if (cl[c].dead) drop_client(c);
    }
    int d = dirty;
    dirty = 0;
    return d;
}

void wl_frame_done(void)
{
    uint32_t t = now_ms();
    for (int i = 0; i < MAXSURF; i++) {
        struct surface *s = &surfs[i];
        if (!s->used || !s->nframe_cb) continue;
        for (int k = 0; k < s->nframe_cb; k++) { ev(s->frame_cb[k], 0); u32(t); end(); delete_id(s->client, s->frame_cb[k]); }
        s->nframe_cb = 0;
        flush_to(s->client, 0, 0);
    }
}

/* ── drawing ────────────────────────────────────────────────────────── */
static void blit(struct canvas *c, int sidx, int x, int y)
{
    struct surface *s = &surfs[sidx];
    if (s->cur_buf < 0) return;
    struct buffer *b = &bufs[s->cur_buf];
    struct pool *p = &pools[b->pool];
    struct node *n = p->f->node;
    if (!n || !n->data) return;
    uint64_t need = (uint64_t)b->off + (uint64_t)b->stride * (uint64_t)(b->h - 1) + (uint64_t)b->w * 4;
    if (need > n->size) return;
    const uint8_t *base = n->data + b->off;
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + b->w > c->cx1 ? c->cx1 : x + b->w, y1 = y + b->h > c->cy1 ? c->cy1 : y + b->h;
    int opaque = b->fmt == 1;
    for (int yy = y0; yy < y1; yy++) {
        const uint32_t *src = (const uint32_t *)(base + (size_t)(yy - y) * (size_t)b->stride) + (x0 - x);
        uint32_t *dst = c->px + (size_t)yy * (size_t)c->stride + x0;
        for (int xx = x0; xx < x1; xx++, src++, dst++) {
            uint32_t sp = *src;
            if (opaque) { *dst = sp | 0xFF000000; continue; }
            uint32_t a = sp >> 24;
            if (a == 255) { *dst = sp; continue; }
            if (!a) continue;
            uint32_t d = *dst, ia = 255 - a;
            uint32_t rb = ((d & 0xFF00FF) * ia >> 8) & 0xFF00FF, g = ((d & 0x00FF00) * ia >> 8) & 0x00FF00;
            *dst = 0xFF000000 | ((sp & 0xFF00FF) + rb) | ((sp & 0x00FF00) + g);
        }
    }
}

void wl_paint_window(struct canvas *c, int x, int sx, int sy)
{
    if (x < 0 || x >= MAXXS || !xss[x].used) return;
    blit(c, xss[x].surf, sx - xss[x].gx, sy - xss[x].gy);
}

void wl_paint_popups(struct canvas *c)
{
    for (int i = 0; i < MAXXS; i++)
        if (xss[i].used && xss[i].role == 2 && xss[i].mapped) blit(c, xss[i].surf, xss[i].ax - xss[i].gx, xss[i].ay - xss[i].gy);
}

const char *wl_app_id(int x) { return x >= 0 && x < MAXXS && xss[x].used ? xss[x].app_id : ""; }
const char *wl_title(int x) { return x >= 0 && x < MAXXS && xss[x].used ? xss[x].title : ""; }

/* ── input ──────────────────────────────────────────────────────────── */
static void each_bound(int c, int type, void (*fn)(int c, uint32_t id, void *ctx), void *ctx)
{
    for (uint32_t id = 2; id < MAXOBJ; id++) if (cl[c].obj[id].type == type) fn(c, id, ctx);
}

struct pev { int kind, x, y, button, state, axis; uint32_t surf; };
static void ptr_send(int c, uint32_t pid, void *ctx)
{
    struct pev *e = ctx;
    uint32_t t = now_ms();
    switch (e->kind) {
    case 0: ev(pid, 0); u32(serial++); u32(e->surf); fixed(e->x); fixed(e->y); end(); break;     /* enter */
    case 1: ev(pid, 1); u32(serial++); u32(e->surf); end(); break;                              /* leave */
    case 2: ev(pid, 2); u32(t); fixed(e->x); fixed(e->y); end(); break;                          /* motion */
    case 3: ev(pid, 3); u32(serial++); u32(t); u32((uint32_t)e->button); u32((uint32_t)e->state); end(); break;
    case 4:
        if (cl[c].obj[pid].version >= 5) { ev(pid, 6); u32(0); end(); ev(pid, 8); u32(0); i32(e->axis); end(); }
        ev(pid, 4); u32(t); u32(0); i32(e->axis * 15 * 256); end();
        break;
    }
    if (cl[c].obj[pid].version >= 5) { ev(pid, 5); end(); }                                     /* frame */
}

static void pointer_event(int x, struct pev *e)
{
    if (x < 0 || !xss[x].used) return;
    int c = xss[x].client;
    e->surf = (uint32_t)surfs[xss[x].surf].id;
    each_bound(c, O_POINTER, ptr_send, e);
    flush_to(c, 0, 0);
}

int wl_popup_at(int sx, int sy, int *lx, int *ly)
{
    for (int i = MAXXS - 1; i >= 0; i--) {
        struct xsurf *s = &xss[i];
        if (!s->used || s->role != 2 || !s->mapped) continue;
        int bx = s->ax - s->gx, by = s->ay - s->gy;
        struct surface *su = &surfs[s->surf];
        if (su->cur_buf < 0) continue;
        if (sx >= s->ax && sy >= s->ay && sx < s->ax + s->gw && sy < s->ay + s->gh) { *lx = sx - bx; *ly = sy - by; return i; }
    }
    return -1;
}

int wl_any_popup(void)
{
    for (int i = 0; i < MAXXS; i++) if (xss[i].used && xss[i].role == 2) return 1;
    return 0;
}

void wl_dismiss_popups(void)
{
    for (int i = MAXXS - 1; i >= 0; i--)
        if (xss[i].used && xss[i].role == 2) { ev((uint32_t)xss[i].role_id, 1); end(); flush_to(xss[i].client, 0, 0); }
}

/* the pointer is over toplevel/popup x at buffer coordinates (bx, by); x = -1: over nothing of ours */
void wl_pointer_at(int x, int bx, int by)
{
    struct pev e = { 0 };
    if (x != ptr_focus) {
        if (ptr_focus >= 0) { e.kind = 1; pointer_event(ptr_focus, &e); }
        ptr_focus = x;
        if (x >= 0) { e.kind = 0; e.x = bx; e.y = by; pointer_event(x, &e); }
        return;
    }
    if (x >= 0) { e.kind = 2; e.x = bx; e.y = by; pointer_event(x, &e); }
}

void wl_button(int button, int pressed)
{
    struct pev e = { 3, 0, 0, button, pressed, 0, 0 };
    pointer_event(ptr_focus, &e);
}

void wl_axis(int dz)
{
    struct pev e = { 4, 0, 0, 0, 0, dz, 0 };
    pointer_event(ptr_focus, &e);
}

struct kev { int kind, key, state; uint32_t surf; };
static void kb_send(int c, uint32_t kid, void *ctx)
{
    (void)c;
    struct kev *e = ctx;
    if (e->kind == 0) {                                       /* enter + modifiers */
        ev(kid, 1); u32(serial++); u32(e->surf); arr(0, 0); end();
        ev(kid, 4); u32(serial++); u32(mods_dep); u32(0); u32(mods_lock); u32(0); end();
    } else if (e->kind == 1) { ev(kid, 2); u32(serial++); u32(e->surf); end(); }
    else if (e->kind == 2) { ev(kid, 3); u32(serial++); u32(now_ms()); u32((uint32_t)e->key); u32((uint32_t)e->state); end(); }
    else { ev(kid, 4); u32(serial++); u32(mods_dep); u32(0); u32(mods_lock); u32(0); end(); }
}

static void kb_event(int x, struct kev *e)
{
    if (x < 0 || !xss[x].used) return;
    e->surf = (uint32_t)surfs[xss[x].surf].id;
    each_bound(xss[x].client, O_KEYBOARD, kb_send, e);
    flush_to(xss[x].client, 0, 0);
}

void wl_focus(int x)
{
    if (x == kb_focus) return;
    struct kev e = { 0 };
    int old = kb_focus;
    if (old >= 0) { e.kind = 1; kb_event(old, &e); }
    kb_focus = x;
    if (x >= 0) { e.kind = 0; kb_event(x, &e); }
    if (old >= 0 && xss[old].used && xss[old].role == 1) send_configure(old);       /* "activated" changes */
    if (x >= 0 && xss[x].role == 1) send_configure(x);
}

void wl_key(int code, int pressed)
{
    /* modifier masks as the standard keymap defines them */
    uint32_t bit = code == 42 || code == 54 ? 1 : code == 29 || code == 97 ? 4 : code == 56 || code == 100 ? 8 :
                   code == 125 || code == 126 ? 64 : 0;
    int changed = 0;
    if (bit) { uint32_t o = mods_dep; mods_dep = pressed ? mods_dep | bit : mods_dep & ~bit; changed = o != mods_dep; }
    if (code == 58 && pressed) { mods_lock ^= 2; changed = 1; }
    int target = kb_focus;
    if (wl_any_popup()) for (int i = MAXXS - 1; i >= 0; i--) if (xss[i].used && xss[i].role == 2 && xss[i].mapped) { target = i; break; }
    struct kev e = { 2, code, pressed, 0 };
    kb_event(target, &e);
    if (changed) { struct kev m = { 3, 0, 0, 0 }; kb_event(target, &m); }
}

void wl_configure_size(int x, int w, int h)
{
    if (x < 0 || !xss[x].used || xss[x].role != 1) return;
    struct xsurf *s = &xss[x];
    s->serial = serial++;
    uint32_t states[3]; int n = 0;
    if (s->maximized) states[n++] = 1;
    states[n++] = 3;                                          /* resizing */
    if (kb_focus == x) states[n++] = 4;
    ev((uint32_t)s->role_id, 0); i32(w); i32(h); arr(states, (uint32_t)n * 4); end();
    ev((uint32_t)s->id, 0); u32(s->serial); end();
    flush_to(s->client, 0, 0);
}

void wl_close(int x)
{
    if (x < 0 || !xss[x].used || xss[x].role != 1) return;
    ev((uint32_t)xss[x].role_id, 1); end();
    flush_to(xss[x].client, 0, 0);
}

void wl_geometry(int x, int *gx, int *gy, int *gw, int *gh)
{
    *gx = xss[x].gx; *gy = xss[x].gy; *gw = xss[x].gw; *gh = xss[x].gh;
}
