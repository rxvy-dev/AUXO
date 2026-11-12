/* The Chrono Browser: an HTTP/HTTPS web browser for AuxoBSD.
 *
 * Pages are fetched by the "www" loader thread (net/www.c, TLS by BearSSL) and laid out here:
 * an HTML tokenizer feeding a flow layout of words, headings, lists, quotes, preformatted
 * text, rules, tables (one row per line), links and forms (GET and POST). No JavaScript, no
 * CSS, no images (their alt text is shown). about:home, file:// (the in-memory filesystem, for
 * Aether's preview) and http(s):// URLs.
 *
 * Tabs, and what the browser remembers in /home/root/.config/browser (kept on the disk of an
 * installed system): the open tabs (session), cookies (net/cookies.c: you stay signed in) and
 * saved logins (offered after you sign in, filled in when you come back). */
#include "kern.h"
#include "gfx.h"
#include "fonts.h"
#include "apps.h"
#include "www.h"
#include "vfs.h"
#include "cookies.h"

#define TABBAR 32
#define TOOLBAR 44
#define TOP (TABBAR + TOOLBAR)
#define MAXTABS 8
#define LOGINBAR 38
#define STATUSBAR 22
#define MARGIN 16
#define MAXITEMS 60000
#define ARENA (2u << 20)
#define MAXLINKS 6000
#define MAXFIELDS 64
#define MAXFORMS 16
#define HIST 32

enum { F_UI, F_BOLD, F_MONO, F_HEAD };
static const struct font *const FONTS[] = { &font_ui, &font_bold, &font_mono, &font_head };
enum { IT_UNDER = 1, IT_RULE = 2, IT_FIELD = 4, IT_DIM = 8, IT_ACCENT = 16, IT_BULLET = 32 };

struct item { int32_t x, y; uint32_t off; int32_t link; int16_t w; uint8_t font, flags; };
enum { FT_TEXT, FT_PASSWORD, FT_SUBMIT, FT_HIDDEN, FT_CHECK };
struct field { int item, form, type, checked; char name[48]; char value[256]; int x, y, w, h; };
struct form { char action[512]; int post; };

/* ── page state ─────────────────────────────────────────────────────── */
static int inited;
static char *src;                 /* the page's HTML (copied from the loader's buffer) */
static size_t srclen;
static int src_latin1, src_plain;
static char *arena;
static size_t alen;
static struct item *items;
static int nitems, truncated;
static uint32_t links[MAXLINKS];
static int nlinks;
static struct field fields[MAXFIELDS];
static int nfields;
static struct form forms[MAXFORMS];
static int nforms;
static int doc_h, layout_w = -1, scroll;
static char title[128], url[512], base[512], status[160];
static char refresh_url[512];
static int refresh_pending;

static char back_hist[HIST][512], fwd_hist[HIST][512];
static int nback, nfwd;

static char addr[512];
static int addr_focus, addr_caret, addr_all;
static int field_focus = -1, field_caret;
static int view_h = 400, view_w = 600;
static int hover_link = -1;
static uint32_t loaded_gen;
static int loading, https_guess;   /* https_guess: we added https:// to a typed address */
static uint32_t page_gen, fields_gen; /* a new page vs. the same page laid out again (resize) */
static int restore_scroll = -1;

/* tabs: the page state above belongs to tabs[active]; the others keep theirs here */
struct tab {
    char url[512], title[128];
    char back[HIST][512], fwd[HIST][512];
    int nback, nfwd, scroll;
    char *cache; size_t cache_len, cache_pages; int cache_plain, cache_latin1;
};
static struct tab tabs[MAXTABS];
static int ntabs, active, loading_tab = -1;
static char deferred[512];
static int deferred_tab = -1;

/* saved logins: offered after a form with a password is sent */
static struct { int active, survive; char site[128], user[256], pass[256]; } offer;

const char *browser_title(void) { return title[0] ? title : "Browser"; }

/* ── small string helpers ───────────────────────────────────────────── */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_space(int c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f'; }
static int ci_eq(const char *a, const char *b) { while (*a && lower(*a) == lower(*b)) { a++; b++; } return !*a && !*b; }
static int ci_starts(const char *s, const char *p) { while (*p) { if (lower(*s++) != lower(*p++)) return 0; } return 1; }

static const char *ci_find(const char *s, const char *end, const char *needle)
{
    size_t n = strlen(needle);
    for (; s + n <= end; s++) {
        size_t i = 0;
        while (i < n && lower(s[i]) == needle[i]) i++;
        if (i == n) return s;
    }
    return 0;
}

static void url_resolve(const char *b, const char *href, char *out, size_t n)
{
    while (is_space(*href)) href++;
    if (ci_starts(href, "http://") || ci_starts(href, "https://") || ci_starts(href, "about:") || ci_starts(href, "file://")) {
        strlcpy(out, href, n); return;
    }
    const char *colon = strchr(b, ':');
    size_t sl = colon ? (size_t)(colon - b) : 0;
    if (href[0] == '/' && href[1] == '/') {               /* scheme-relative */
        char sch[16];
        strlcpy(sch, b, sl + 1 < sizeof sch ? sl + 1 : sizeof sch);
        ksnprintf(out, n, "%s:%s", sch, href); return;
    }
    /* origin = scheme://host[:port] */
    char origin[256];
    const char *p = colon && colon[1] == '/' && colon[2] == '/' ? colon + 3 : b;
    if (ci_starts(b, "file://")) p = b + 7;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    size_t ol = (size_t)(p - b);
    strlcpy(origin, b, ol + 1 < sizeof origin ? ol + 1 : sizeof origin);
    if (href[0] == '/') { ksnprintf(out, n, "%s%s", origin, href); return; }
    if (href[0] == '#' || !href[0]) {                      /* same page */
        strlcpy(out, b, n);
        char *h = strchr(out, '#'); if (h) *h = 0;
        return;
    }
    char dir[512];
    strlcpy(dir, *p ? p : "/", sizeof dir);
    char *q = dir; while (*q && *q != '?' && *q != '#') q++;
    *q = 0;
    char *last = strrchr(dir, '/');
    if (last) last[1] = 0; else strlcpy(dir, "/", sizeof dir);
    if (href[0] == '?') {
        char path[512]; strlcpy(path, *p ? p : "/", sizeof path);
        char *qq = path; while (*qq && *qq != '?' && *qq != '#') qq++;
        *qq = 0;
        ksnprintf(out, n, "%s%s%s", origin, path, href); return;
    }
    /* collapse ./ and ../ */
    char path[1024];
    ksnprintf(path, sizeof path, "%s%s", dir, href);
    char res[1024]; size_t r = 0;
    for (const char *s = path; *s; ) {
        if (s[0] == '/' && s[1] == '.' && s[2] == '.' && (s[3] == '/' || !s[3])) {
            while (r > 0 && res[r - 1] != '/') r--;
            if (r > 0) r--;
            s += 3;
            if (!*s) { res[r++] = '/'; }
            continue;
        }
        if (s[0] == '/' && s[1] == '.' && (s[2] == '/' || !s[2])) { s += 2; if (!*s) res[r++] = '/'; continue; }
        if (r + 1 < sizeof res) res[r++] = *s;
        s++;
    }
    res[r] = 0;
    ksnprintf(out, n, "%s%s", origin, res);
}

static void url_encode(const char *s, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

/* ── layout ─────────────────────────────────────────────────────────── */
static struct {
    int x, y, left, right, line_first, line_h, line_base, space;
    int bold, mono, pre, head, link, under, dim, accent, skip, center;
    int list_depth, list_num[8], list_ol[8], quote;
    int in_title, in_button, button_form, in_a;
    int form;
} L;

static int add_text(const char *s, size_t n)
{
    if (alen + n + 1 > ARENA) { truncated = 1; return -1; }
    memcpy(arena + alen, s, n);
    arena[alen + n] = 0;
    int off = (int)alen;
    alen += n + 1;
    return off;
}

static void end_line(void)
{
    /* align the line's items on a common baseline */
    int base = 0, h = 0;
    for (int i = L.line_first; i < nitems; i++) {
        const struct font *f = FONTS[items[i].font];
        if (f->baseline > base) base = f->baseline;
        if (f->height > h) h = f->height;
    }
    if (!h) h = 16, base = 12;
    for (int i = L.line_first; i < nitems; i++) {
        if (items[i].flags & IT_RULE) continue;
        items[i].y += base - FONTS[items[i].font]->baseline;
    }
    L.y += h + 4;
    L.x = L.left;
    L.line_first = nitems;
    L.space = 0;
}

static void newline(void) { if (L.x > L.left || nitems > L.line_first) end_line(); }
static void block(int gap)
{
    newline();
    if (gap && nitems) L.y += gap;
}

static struct item *add_item(int font, int w, uint32_t off, int flags)
{
    if (nitems >= MAXITEMS) { truncated = 1; return 0; }
    struct item *it = &items[nitems++];
    it->x = L.x; it->y = L.y; it->off = off; it->w = (int16_t)(w > 32767 ? 32767 : w);
    it->font = (uint8_t)font; it->flags = (uint8_t)flags;
    it->link = L.link;
    return it;
}

static int cur_font(void) { return L.head && L.head <= 2 ? F_HEAD : L.mono ? F_MONO : (L.bold || L.head) ? F_BOLD : F_UI; }
static int cur_flags(void)
{
    return (L.link >= 0 || L.under ? IT_UNDER : 0) | (L.dim ? IT_DIM : 0) | (L.head ? IT_ACCENT : 0);
}

static void word(const char *s, size_t n)
{
    if (L.skip || !n || truncated) return;
    if (L.in_title) {
        size_t tl = strlen(title);
        if (tl && tl + 1 < sizeof title) title[tl++] = ' ';
        for (size_t i = 0; i < n && tl + 1 < sizeof title; i++) title[tl++] = s[i];
        title[tl] = 0;
        return;
    }
    int font = cur_font();
    int off = add_text(s, n);
    if (off < 0) return;
    const struct font *f = FONTS[font];
    int w = gfx_text_width(f, arena + off);
    int sp = L.space && L.x > L.left ? gfx_glyph(f, ' ')->advance : 0;
    if (L.x + sp + w > L.right && L.x > L.left) { end_line(); sp = 0; }
    L.x += sp;
    struct item *it = add_item(font, w, (uint32_t)off, cur_flags());
    if (it && L.in_button) { it->flags |= IT_FIELD; it->link = -2 - L.button_form; }
    L.x += w;
    L.space = 0;
}

/* text with entities already decoded, and whitespace handling by mode */
static void text(const char *s, size_t n)
{
    if (L.pre && !L.in_title) {
        size_t a = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i == n || s[i] == '\n') {
                if (i > a) {
                    /* expand tabs, emit as one unwrapped run */
                    char buf[1024]; size_t o = 0;
                    for (size_t k = a; k < i && o + 8 < sizeof buf; k++) {
                        if (s[k] == '\t') { do buf[o++] = ' '; while (o % 4); }
                        else if (s[k] != '\r') buf[o++] = s[k];
                    }
                    int off = add_text(buf, o);
                    if (off >= 0) {
                        int w = gfx_text_width(FONTS[F_MONO], arena + off);
                        add_item(F_MONO, w, (uint32_t)off, cur_flags());
                        L.x += w;
                    }
                }
                if (i < n) end_line();
                a = i + 1;
            }
        }
        return;
    }
    size_t i = 0;
    while (i < n) {
        if (is_space((unsigned char)s[i])) { L.space = 1; i++; continue; }
        size_t j = i;
        while (j < n && !is_space((unsigned char)s[j])) j++;
        word(s + i, j - i);
        i = j;
        if (i < n) L.space = 1;
    }
}

static size_t put_utf8(char *o, uint32_t cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 63)); o[2] = (char)(0x80 | (cp >> 6 & 63)); o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

static const struct { const char *n; uint16_t cp; } ENT[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
    { "hellip", 0x2026 }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
    { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bull", 0x2022 }, { "middot", 0xB7 }, { "times", 0xD7 },
    { "euro", 0x20AC }, { "pound", 0xA3 }, { "yen", 0xA5 }, { "cent", 0xA2 }, { "deg", 0xB0 }, { "sect", 0xA7 },
    { "para", 0xB6 }, { "plusmn", 0xB1 }, { "frac12", 0xBD }, { "rarr", 0x2192 }, { "larr", 0x2190 },
    { "uarr", 0x2191 }, { "darr", 0x2193 }, { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "aacute", 0xE1 },
    { "agrave", 0xE0 }, { "ouml", 0xF6 }, { "uuml", 0xFC }, { "auml", 0xE4 }, { "szlig", 0xDF }, { "ccedil", 0xE7 },
    { "ntilde", 0xF1 }, { "iexcl", 0xA1 }, { "iquest", 0xBF }, { "shy", 0 }, { "zwj", 0 }, { "zwnj", 0 },
    { "ensp", ' ' }, { "emsp", ' ' }, { "thinsp", ' ' }, { "minus", 0x2212 }, { "check", 0x2713 }, { "star", 0x2606 },
};

/* decode entities (and Latin-1 bytes when the page is Latin-1) from s..e into out */
static size_t decode(const char *s, const char *e, char *out, size_t cap)
{
    size_t o = 0;
    while (s < e && o + 8 < cap) {
        unsigned char c = (unsigned char)*s;
        if (c == '&') {
            const char *semi = s + 1;
            while (semi < e && semi - s < 12 && *semi != ';' && *semi != '&' && !is_space(*semi) && *semi != '<') semi++;
            if (semi < e && *semi == ';') {
                uint32_t cp = 0xFFFFFFFF;
                if (s[1] == '#') {
                    cp = 0;
                    if (s[2] == 'x' || s[2] == 'X') for (const char *k = s + 3; k < semi; k++) cp = cp * 16 + (uint32_t)(*k <= '9' ? *k - '0' : (lower(*k) - 'a' + 10));
                    else for (const char *k = s + 2; k < semi; k++) cp = cp * 10 + (uint32_t)(*k - '0');
                    if (cp == 0x2019 || cp == 0x2018) { }
                } else {
                    char nm[12]; size_t l = (size_t)(semi - s - 1);
                    if (l < sizeof nm) {
                        memcpy(nm, s + 1, l); nm[l] = 0;
                        for (size_t k = 0; k < sizeof ENT / sizeof *ENT; k++) if (!strcmp(ENT[k].n, nm)) { cp = ENT[k].cp; break; }
                    }
                }
                if (cp != 0xFFFFFFFF) {
                    if (cp == 0xA0) out[o++] = ' ';              /* nbsp: an ordinary space here */
                    else if (cp) o += put_utf8(out + o, cp);
                    s = semi + 1;
                    continue;
                }
            }
        }
        if (c >= 0x80 && src_latin1) { o += put_utf8(out + o, c); s++; continue; }
        out[o++] = (char)c;
        s++;
    }
    return o;
}

/* attributes of the tag being handled */
static struct { char name[16]; char val[512]; } attr[12];
static int nattr;

static const char *get_attr(const char *n)
{
    for (int i = 0; i < nattr; i++) if (!strcmp(attr[i].name, n)) return attr[i].val;
    return 0;
}

static int add_link(const char *href)
{
    if (nlinks >= MAXLINKS || !href) return -1;
    while (is_space(*href)) href++;
    if (ci_starts(href, "javascript:") || ci_starts(href, "mailto:") || ci_starts(href, "data:") || ci_starts(href, "tel:")) return -1;
    char abs[1024];
    url_resolve(base, href, abs, sizeof abs);
    int off = add_text(abs, strlen(abs));
    if (off < 0) return -1;
    links[nlinks] = (uint32_t)off;
    return nlinks++;
}

static void add_field(int type, const char *name, const char *value, int width)
{
    if (nfields >= MAXFIELDS) return;
    struct field *f = &fields[nfields];
    memset(f, 0, sizeof *f);
    f->type = type; f->form = L.form;
    strlcpy(f->name, name ? name : "", sizeof f->name);
    if (value) {
        char tmp[256];
        size_t n = decode(value, value + strlen(value), tmp, sizeof tmp - 1);
        tmp[n] = 0;
        strlcpy(f->value, tmp, sizeof f->value);
    }
    if (type == FT_HIDDEN) { f->item = -1; nfields++; return; }
    int w = width;
    if (type == FT_SUBMIT) w = gfx_text_width(&font_ui, f->value[0] ? f->value : "Submit") + 24;
    if (L.x + w + 6 > L.right && L.x > L.left) end_line();
    if (L.x > L.left) L.x += 4;
    struct item *it = add_item(F_UI, w, 0, IT_FIELD);
    if (!it) return;
    it->link = -100 - nfields;                    /* items that are fields carry their index */
    f->item = nitems - 1;
    L.x += w + 4;
    nfields++;
}

static int heading_level(const char *t) { return t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2] ? t[1] - '0' : 0; }

static int is_block(const char *t)
{
    static const char *const B[] = { "p", "div", "section", "article", "header", "footer", "nav", "main", "aside",
        "figure", "figcaption", "center", "address", "details", "summary", "fieldset", "table", "tr", "form", "dl",
        "dt", "dd", "caption", "legend", "noscript", "body", "html", "menu", "hgroup", "search", 0 };
    for (int i = 0; B[i]; i++) if (!strcmp(t, B[i])) return 1;
    return 0;
}

static void tag(const char *name, int closing, int selfclose)
{
    int h = heading_level(name);
    if (!strcmp(name, "title")) { L.in_title = !closing; return; }
    if (!strcmp(name, "br")) { end_line(); return; }
    if (!strcmp(name, "hr")) {
        block(6);
        struct item *it = add_item(F_UI, L.right - L.left, 0, IT_RULE);
        if (it) it->link = -1;
        L.y += 10;
        L.line_first = nitems;
        return;
    }
    if (!strcmp(name, "base") && !closing) { const char *b = get_attr("href"); if (b) url_resolve(url, b, base, sizeof base); return; }
    if (!strcmp(name, "meta") && !closing) {
        const char *he = get_attr("http-equiv"), *ct = get_attr("content"), *cs = get_attr("charset");
        if (cs && (ci_starts(cs, "iso-8859") || ci_starts(cs, "windows-125") || ci_starts(cs, "latin1"))) src_latin1 = 1;
        if (he && ct && ci_eq(he, "content-type") && (ci_find(ct, ct + strlen(ct), "iso-8859") || ci_find(ct, ct + strlen(ct), "windows-125"))) src_latin1 = 1;
        if (he && ct && ci_eq(he, "refresh")) {
            const char *u = ci_find(ct, ct + strlen(ct), "url=");
            if (u && katoi(ct) <= 5) { url_resolve(url, u + 4, refresh_url, sizeof refresh_url); refresh_pending = 1; }
        }
        return;
    }
    if (h) {
        if (!closing) { block(nitems ? (h <= 2 ? 14 : 10) : 0); L.head = h; }
        else { L.head = 0; block(6); }
        return;
    }
    if (!strcmp(name, "p")) { block(closing ? 4 : 8); return; }
    if (!strcmp(name, "ul") || !strcmp(name, "ol") || !strcmp(name, "dl") || !strcmp(name, "menu")) {
        if (!closing) {
            block(L.list_depth ? 0 : 4);
            if (L.list_depth < 8) { L.list_ol[L.list_depth] = name[0] == 'o'; L.list_num[L.list_depth] = 0; }
            L.list_depth++;
            L.left += 26; L.x = L.left;
        } else if (L.list_depth) {
            block(0);
            L.list_depth--;
            L.left -= 26; L.x = L.left;
            if (!L.list_depth) L.y += 4;
        }
        return;
    }
    if (!strcmp(name, "li")) {
        if (closing) { newline(); return; }
        newline();
        int d = L.list_depth ? L.list_depth - 1 : 0;
        char b[16];
        if (d < 8 && L.list_ol[d]) ksnprintf(b, sizeof b, "%d.", ++L.list_num[d]);
        else strlcpy(b, d % 2 ? "–" : "•", sizeof b);
        int off = add_text(b, strlen(b));
        if (off >= 0) {
            int w = gfx_text_width(&font_ui, arena + off);
            int sx = L.x;
            L.x = L.left - w - 8;
            struct item *it = add_item(F_UI, w, (uint32_t)off, IT_BULLET);
            if (it) it->link = -1;
            L.x = sx;
        }
        return;
    }
    if (!strcmp(name, "dd")) { if (!closing) { block(0); L.x += 26; } else block(0); return; }
    if (!strcmp(name, "dt")) { block(0); L.bold += closing ? -1 : 1; if (L.bold < 0) L.bold = 0; return; }
    if (!strcmp(name, "blockquote")) {
        block(4);
        if (!closing) { L.left += 24; L.right -= 16; L.quote++; L.dim++; }
        else if (L.quote) { L.left -= 24; L.right += 16; L.quote--; L.dim--; }
        L.x = L.left;
        return;
    }
    if (!strcmp(name, "pre") || !strcmp(name, "listing") || !strcmp(name, "xmp")) {
        block(6);
        L.pre += closing ? -1 : 1;
        if (L.pre < 0) L.pre = 0;
        return;
    }
    if (!strcmp(name, "td") || !strcmp(name, "th")) {
        if (!closing && L.x > L.left) L.x += 18;
        if (name[1] == 'h') { L.bold += closing ? -1 : 1; if (L.bold < 0) L.bold = 0; }
        return;
    }
    if (!strcmp(name, "a")) {
        if (closing) { L.link = -1; L.in_a = 0; return; }
        const char *href = get_attr("href");
        L.link = href ? add_link(href) : -1;
        L.in_a = 1;
        return;
    }
    if (!strcmp(name, "b") || !strcmp(name, "strong")) { L.bold += closing ? -1 : 1; if (L.bold < 0) L.bold = 0; return; }
    if (!strcmp(name, "code") || !strcmp(name, "tt") || !strcmp(name, "kbd") || !strcmp(name, "samp")) {
        L.mono += closing ? -1 : 1; if (L.mono < 0) L.mono = 0; return;
    }
    if (!strcmp(name, "u") || !strcmp(name, "ins")) { L.under += closing ? -1 : 1; if (L.under < 0) L.under = 0; return; }
    if (!strcmp(name, "small") || !strcmp(name, "figcaption") || !strcmp(name, "cite")) { L.dim += closing ? -1 : 1; if (L.dim < 0) L.dim = 0; return; }
    if (!strcmp(name, "img") && !closing) {
        const char *alt = get_attr("alt");
        if (alt && *alt) {
            char t[200], b[220];
            size_t n = decode(alt, alt + strlen(alt), t, sizeof t - 1);
            t[n] = 0;
            ksnprintf(b, sizeof b, "[%s]", t);
            L.dim++; text(b, strlen(b)); L.dim--;
            L.space = 1;
        }
        return;
    }
    if (!strcmp(name, "form")) {
        block(4);
        if (!closing && nforms < MAXFORMS) {
            const char *a = get_attr("action");
            url_resolve(base, a && *a ? a : url, forms[nforms].action, sizeof forms[nforms].action);
            const char *m = get_attr("method");
            forms[nforms].post = m && ci_eq(m, "post");
            L.form = nforms++;
        } else if (closing) L.form = -1;
        return;
    }
    if (!strcmp(name, "input") && !closing) {
        const char *ty = get_attr("type"), *nm = get_attr("name"), *val = get_attr("value");
        if (!ty || ci_eq(ty, "text") || ci_eq(ty, "search") || ci_eq(ty, "email") || ci_eq(ty, "url") || ci_eq(ty, "tel") || ci_eq(ty, "number"))
            add_field(FT_TEXT, nm, val, 240);
        else if (ci_eq(ty, "password")) add_field(FT_PASSWORD, nm, val, 200);
        else if (ci_eq(ty, "submit") || ci_eq(ty, "button") || ci_eq(ty, "image")) add_field(FT_SUBMIT, nm, val ? val : "Submit", 0);
        else if (ci_eq(ty, "hidden")) add_field(FT_HIDDEN, nm, val, 0);
        else if (ci_eq(ty, "checkbox")) {
            add_field(FT_CHECK, nm, val ? val : "on", 18);
            if (nfields && fields[nfields - 1].type == FT_CHECK) fields[nfields - 1].checked = get_attr("checked") != 0;
        }
        return;
    }
    if (!strcmp(name, "button")) {
        if (!closing) { L.in_button = 1; L.button_form = L.form < 0 ? 0 : L.form; L.x += 4; }
        else { L.in_button = 0; L.x += 4; }
        return;
    }
    if (is_block(name)) { block(!strcmp(name, "table") || !strcmp(name, "form") ? 4 : 0); return; }
    (void)selfclose;
}

static void autofill(void);
static struct { char value[256]; int checked; char name[48]; } kept[MAXFIELDS];

static void layout(int width)
{
    int nkept = 0;
    if (fields_gen == page_gen)                /* same page, new width: keep what was typed */
        for (; nkept < nfields; nkept++) { strlcpy(kept[nkept].value, fields[nkept].value, sizeof kept[0].value); kept[nkept].checked = fields[nkept].checked; strlcpy(kept[nkept].name, fields[nkept].name, sizeof kept[0].name); }
    fields_gen = page_gen;
    nitems = 0; alen = 0; nlinks = 0; nfields = 0; nforms = 0; truncated = 0;
    title[0] = 0;
    refresh_pending = 0;
    strlcpy(base, url, sizeof base);
    memset(&L, 0, sizeof L);
    L.left = MARGIN; L.right = width - MARGIN; L.x = L.left; L.y = 12; L.link = -1; L.form = -1;
    layout_w = width;
    if (L.right < L.left + 100) L.right = L.left + 100;
    const char *s = src, *e = src + srclen;
    if (src_plain) { L.pre = 1; text(s, srclen); newline(); doc_h = L.y + 20; nfields = 0; return; }

    static char buf[16384];
    while (s < e && !truncated) {
        if (*s != '<') {
            const char *t = s;
            while (t < e && *t != '<') t++;
            /* decode in pieces */
            while (s < t) {
                const char *piece = s + 8000 < t ? s + 8000 : t;
                while (piece < t && piece > s && !is_space(*piece) && piece - s > 7000) piece--;
                size_t n = decode(s, piece, buf, sizeof buf);
                text(buf, n);
                s = piece;
            }
            continue;
        }
        /* comment, doctype, CDATA */
        if (s + 4 <= e && !strncmp(s, "<!--", 4)) {
            const char *end = ci_find(s + 4, e, "-->");
            s = end ? end + 3 : e;
            continue;
        }
        if (s + 1 < e && (s[1] == '!' || s[1] == '?')) { while (s < e && *s != '>') s++; s++; continue; }
        const char *p = s + 1;
        int closing = 0;
        if (p < e && *p == '/') { closing = 1; p++; }
        char name[16]; size_t nl = 0;
        while (p < e && (((*p | 32) >= 'a' && (*p | 32) <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == ':')) {
            if (nl + 1 < sizeof name) name[nl++] = (char)lower(*p);
            p++;
        }
        name[nl] = 0;
        if (!nl) { text("<", 1); s++; continue; }
        /* attributes */
        nattr = 0;
        int selfclose = 0;
        while (p < e && *p != '>') {
            while (p < e && is_space(*p)) p++;
            if (p < e && *p == '/') { selfclose = 1; p++; continue; }
            if (p >= e || *p == '>') break;
            char an[16]; size_t al = 0;
            while (p < e && !is_space(*p) && *p != '=' && *p != '>' && *p != '/') { if (al + 1 < sizeof an) an[al++] = (char)lower(*p); p++; }
            an[al] = 0;
            while (p < e && is_space(*p)) p++;
            char *val = 0;
            if (nattr < 12) { strlcpy(attr[nattr].name, an, sizeof attr[nattr].name); val = attr[nattr].val; val[0] = 0; }
            if (p < e && *p == '=') {
                p++;
                while (p < e && is_space(*p)) p++;
                char q = (p < e && (*p == '"' || *p == '\'')) ? *p++ : 0;
                size_t vl = 0;
                while (p < e && (q ? *p != q : (!is_space(*p) && *p != '>'))) { if (val && vl + 1 < sizeof attr[0].val) val[vl++] = *p; p++; }
                if (val) val[vl] = 0;
                if (q && p < e) p++;
            }
            if (al && nattr < 12) nattr++;
            if (!al && p < e && *p != '>') p++;
        }
        s = p < e ? p + 1 : e;
        /* raw-text elements: skip their content (title is read as text) */
        if (!closing && (!strcmp(name, "script") || !strcmp(name, "style") || !strcmp(name, "svg") || !strcmp(name, "template") ||
                         !strcmp(name, "textarea") || !strcmp(name, "select") || !strcmp(name, "iframe") || !strcmp(name, "object") ||
                         !strcmp(name, "canvas") || !strcmp(name, "math"))) {
            char endtag[20];
            ksnprintf(endtag, sizeof endtag, "</%s", name);
            const char *end = ci_find(s, e, endtag);
            if (!strcmp(name, "textarea")) {
                const char *nm = get_attr("name");
                char v[256]; size_t n = end ? (size_t)(end - s) : 0;
                if (n > sizeof v - 1) n = sizeof v - 1;
                memcpy(v, s, n); v[n] = 0;
                add_field(FT_TEXT, nm, v, 320);
            }
            s = end ? end : e;
            if (end) { while (s < e && *s != '>') s++; s++; }
            continue;
        }
        if (!strcmp(name, "head") && closing) { title[sizeof title - 1] = 0; }
        tag(name, closing, selfclose);
    }
    newline();
    if (truncated) {
        L.dim++;
        block(10);
        const char *m = "(the rest of this page is too long to show)";
        text(m, strlen(m));
        newline();
    }
    doc_h = L.y + 24;
    /* trim the title */
    char *t = title; while (*t == ' ') t++;
    if (t != title) memmove(title, t, strlen(t) + 1);
    for (int i = 0; i < nkept && i < nfields; i++)
        if (!strcmp(kept[i].name, fields[i].name)) { strlcpy(fields[i].value, kept[i].value, sizeof fields[i].value); fields[i].checked = kept[i].checked; }
    if (!nkept) autofill();
}

/* ── loading ────────────────────────────────────────────────────────── */
static const char HOME[] =
    "<html><head><title>Start</title></head><body>"
    "<h1>Chrono Browser</h1>"
    "<p>Type an address above, or words to search. AuxoBSD's browser speaks HTTP and HTTPS (TLS 1.2); "
    "it shows text, links and simple forms, and it doesn't run JavaScript.</p>"
    "<form action=\"https://lite.duckduckgo.com/lite/\"><input name=\"q\" type=\"text\"> <input type=\"submit\" value=\"Search\"></form>"
    "<h3>Sites that work well</h3><ul>"
    "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite</a> - web search, no JavaScript needed</li>"
    "<li><a href=\"http://frogfind.com/\">FrogFind</a> - search, and it simplifies any page for old browsers</li>"
    "<li><a href=\"http://68k.news/\">68k.news</a> - news headlines</li>"
    "<li><a href=\"https://text.npr.org/\">text.npr.org</a> - NPR news, text only</li>"
    "<li><a href=\"https://lite.cnn.com/\">lite.cnn.com</a> - CNN, text only</li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Special:Random\">Wikipedia</a> - a random article</li>"
    "<li><a href=\"https://auxolinux.com/\">auxolinux.com</a> - Auxo</li>"
    "<li><a href=\"https://github.com/rxvy-dev/chrono\">github.com/rxvy-dev/chrono</a> - the Chrono desktop</li>"
    "<li><a href=\"https://github.com/rxvy-dev/aether\">github.com/rxvy-dev/aether</a> - Aether</li>"
    "</ul><h3>On this computer</h3><ul>"
    "<li><a href=\"file:///home/root/index.html\">file:///home/root/index.html</a> - edit it in Aether</li>"
    "<li><a href=\"file:///home/root/README.txt\">README.txt</a></li>"
    "</ul><p><small>Ctrl+L: address bar · Backspace: back · Space / PgDn: scroll</small></p></body></html>";

static void set_page(const char *data, size_t len, int plain)
{
    if (len > WWW_BUF - 1) len = WWW_BUF - 1;
    memcpy(src, data, len);
    srclen = len;
    src[len] = 0;
    src_plain = plain;
    scroll = 0;
    layout_w = -1;
    page_gen++;
    field_focus = -1;
    hover_link = -1;
}

static void error_page(const char *heading, const char *msg)
{
    static char page[2048];
    ksnprintf(page, sizeof page, "<html><head><title>%s</title></head><body><h2>%s</h2><p>%s</p><p><small>%s</small></p>"
              "<p><a href=\"about:home\">Start page</a></p></body></html>", heading, heading, msg, url);
    src_latin1 = 0;
    set_page(page, strlen(page), 0);
}

static int ensure_init(void)
{
    if (inited) return 0;
    uint64_t a = pmm_alloc_contig(WWW_BUF / PAGE_SIZE), b = pmm_alloc_contig(ARENA / PAGE_SIZE);
    uint64_t c = pmm_alloc_contig((MAXITEMS * sizeof(struct item) + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!a || !b || !c || www_init() < 0) return -1;
    src = (char *)(uintptr_t)a; arena = (char *)(uintptr_t)b; items = (struct item *)(uintptr_t)c;
    inited = 1;
    return 0;
}

/* ── what the browser keeps: /home/root/.config/browser ─────────────── */
#define SESSION "/home/root/.config/browser/session"
#define LOGINS  "/home/root/.config/browser/logins"

static void pct_decode(const char *s, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        char c = s[i];
        if (c == '+') c = ' ';
        else if (c == '%' && i + 2 < n) {
            int h = 0;
            for (int k = 1; k <= 2; k++) { char d = s[i + k]; h = h * 16 + (d >= '0' && d <= '9' ? d - '0' : (d | 32) - 'a' + 10); }
            c = (char)h; i += 2;
        }
        out[o++] = c;
    }
    out[o] = 0;
}

/* the part of a URL a login belongs to: host, plus the port when it isn't the default */
static void site_of(const char *u, char *out, size_t n)
{
    out[0] = 0;
    if (ci_starts(u, "https://")) u += 8; else if (ci_starts(u, "http://")) u += 7; else return;
    size_t i = 0;
    while (*u && *u != '/' && *u != '?' && *u != '#' && i + 1 < n) out[i++] = (char)lower(*u++);
    out[i] = 0;
}

/* 1: found (user/pass filled; if user was given, only that user), -1: "never" for this site, 0: none */
static int login_find(const char *site, char *user, size_t un, char *pass, size_t pn, int want_user)
{
    const char *d; size_t n;
    if (vfs_read(LOGINS, &d, &n) < 0) return 0;
    int found = 0;
    for (size_t i = 0; i < n; ) {
        size_t e = i; while (e < n && d[e] != '\n') e++;
        const char *f[4]; size_t fl[4]; int k = 0; size_t st = i;
        for (size_t j = i; j <= e && k < 4; j++) if (j == e || d[j] == '\t') { f[k] = d + st; fl[k] = j - st; k++; st = j + 1; }
        char s2[128];
        if (k >= 2) {
            size_t l = fl[1] < sizeof s2 - 1 ? fl[1] : sizeof s2 - 1;
            memcpy(s2, f[1], l); s2[l] = 0;
            if (!strcmp(s2, site)) {
                if (fl[0] == 5 && !memcmp(f[0], "never", 5)) found = -1;
                else if (k == 4 && fl[0] == 5 && !memcmp(f[0], "login", 5)) {
                    char u[256]; pct_decode(f[2], fl[2], u, sizeof u);
                    if (!want_user || !strcmp(u, user)) {
                        strlcpy(user, u, un);
                        pct_decode(f[3], fl[3], pass, pn);
                        found = 1;
                    }
                }
            }
        }
        i = e + 1;
    }
    return found;
}

static void login_store(const char *site, const char *user, const char *pass, int never)
{
    const char *d; size_t n;
    static char out[32768];
    size_t o = 0;
    if (vfs_read(LOGINS, &d, &n) == 0)                 /* keep every other line */
        for (size_t i = 0; i < n; ) {
            size_t e = i; while (e < n && d[e] != '\n') e++;
            char site2[128] = "", u2[256] = "";
            const char *t1 = memchr(d + i, '\t', e - i);
            if (t1) {
                const char *t2 = memchr(t1 + 1, '\t', (size_t)(d + e - t1 - 1));
                size_t l = (size_t)((t2 ? t2 : d + e) - t1 - 1);
                if (l < sizeof site2) { memcpy(site2, t1 + 1, l); site2[l] = 0; }
                if (t2) { const char *t3 = memchr(t2 + 1, '\t', (size_t)(d + e - t2 - 1)); if (t3) pct_decode(t2 + 1, (size_t)(t3 - t2 - 1), u2, sizeof u2); }
            }
            int same = !strcmp(site2, site) && (never || !strcmp(u2, user) || !strncmp(d + i, "never", 5));
            if (!same && e > i && o + (e - i) + 1 < sizeof out) { memcpy(out + o, d + i, e - i); o += e - i; out[o++] = '\n'; }
            i = e + 1;
        }
    char eu[800], ep[800];
    if (never) o += (size_t)ksnprintf(out + o, sizeof out - o, "never\t%s\n", site);
    else {
        url_encode(user, eu, sizeof eu); url_encode(pass, ep, sizeof ep);
        o += (size_t)ksnprintf(out + o, sizeof out - o, "login\t%s\t%s\t%s\n", site, eu, ep);
    }
    vfs_write(LOGINS, out, o < sizeof out ? o : sizeof out);
}

/* the text field a password field goes with: the last one before it in the same form */
static int user_field_for(int pw)
{
    for (int i = pw - 1; i >= 0; i--)
        if (fields[i].form == fields[pw].form && fields[i].type == FT_TEXT) return i;
    return -1;
}

static void autofill(void)
{
    char site[128];
    site_of(url, site, sizeof site);
    if (!site[0]) return;
    for (int i = 0; i < nfields; i++) {
        if (fields[i].type != FT_PASSWORD || fields[i].value[0]) continue;
        int u = user_field_for(i);
        char user[256] = "", pass[256];
        int want = u >= 0 && fields[u].value[0];
        if (want) strlcpy(user, fields[u].value, sizeof user);
        if (login_find(site, user, sizeof user, pass, sizeof pass, want) != 1) continue;
        strlcpy(fields[i].value, pass, sizeof fields[i].value);
        if (u >= 0 && !fields[u].value[0]) strlcpy(fields[u].value, user, sizeof fields[u].value);
        ksnprintf(status, sizeof status, "Filled in your saved login for %s", site);
    }
}

static void offer_login(int form)
{
    char site[128];
    site_of(url, site, sizeof site);
    if (!site[0]) return;
    for (int i = 0; i < nfields; i++) {
        if (fields[i].form != form || fields[i].type != FT_PASSWORD || !fields[i].value[0]) continue;
        int u = user_field_for(i);
        char user[256] = "", pass[256] = "";
        if (u >= 0) strlcpy(user, fields[u].value, sizeof user);
        int r = login_find(site, user, sizeof user, pass, sizeof pass, 1);
        if (r == -1 || (r == 1 && !strcmp(pass, fields[i].value))) return;   /* "never", or saved as is */
        strlcpy(offer.site, site, sizeof offer.site);
        strlcpy(offer.user, u >= 0 ? fields[u].value : "", sizeof offer.user);
        strlcpy(offer.pass, fields[i].value, sizeof offer.pass);
        offer.active = 1; offer.survive = 1;
        return;
    }
}

/* the session: which tabs were open, so they come back next time */
static void tab_save_state(int i);
static void save_session(void)
{
    if (ntabs <= 0) return;
    tab_save_state(active);
    static char out[8192];
    size_t o = (size_t)ksnprintf(out, sizeof out, "active\t%d\n", active);
    for (int i = 0; i < ntabs && o < sizeof out - 700; i++) {
        char t[128];
        strlcpy(t, tabs[i].title, sizeof t);
        for (char *p = t; *p; p++) if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
        o += (size_t)ksnprintf(out + o, sizeof out - o, "tab\t%s\t%s\n", tabs[i].url[0] ? tabs[i].url : "about:home", t);
    }
    const char *d; size_t n;
    if (vfs_read(SESSION, &d, &n) == 0 && n == o && !memcmp(d, out, o)) return;   /* unchanged */
    vfs_write(SESSION, out, o);
}

/* ── loading ────────────────────────────────────────────────────────── */
static void push_history(void)
{
    if (!url[0]) return;
    if (nback == HIST) { memmove(back_hist[0], back_hist[1], sizeof back_hist[0] * (HIST - 1)); nback--; }
    strlcpy(back_hist[nback++], url, sizeof back_hist[0]);
    nfwd = 0;
}

static int www_busy(void) { return www.state == WWW_REQUESTED || www.state == WWW_LOADING; }

static void load(const char *u, int push)
{
    if (ensure_init() < 0) return;
    https_guess = 0;
    char from[512];
    strlcpy(from, url, sizeof from);
    if (push && url[0] && strcmp(url, u)) push_history();
    if (offer.survive) offer.survive = 0; else offer.active = 0;
    strlcpy(url, u, sizeof url);
    strlcpy(addr, u, sizeof addr);
    addr_focus = 0;
    status[0] = 0;
    src_latin1 = 0;
    restore_scroll = -1;
    if (ci_starts(u, "about:")) {
        if (ci_eq(u, "about:home") || ci_eq(u, "about:blank")) set_page(HOME, sizeof HOME - 1, 0);
        else error_page("Not found", "There's no such about: page.");
        loading = 0;
        save_session();
        return;
    }
    if (ci_starts(u, "file://")) {
        const char *d; size_t n;
        if (vfs_read(u + 7, &d, &n) < 0) { error_page("File not found", "There's no such file on this computer."); loading = 0; return; }
        const char *dot = strrchr(u, '.');
        set_page(d, n, !(dot && (ci_eq(dot, ".html") || ci_eq(dot, ".htm"))));
        loading = 0;
        save_session();
        return;
    }
    loading = 1;
    if (www_busy() && loading_tab != active) {           /* another tab is still loading: queue it */
        strlcpy(deferred, u, sizeof deferred);
        deferred_tab = active;
        ksnprintf(status, sizeof status, "Waiting for the other tab…");
        return;
    }
    ksnprintf(status, sizeof status, "Connecting…");
    if (deferred_tab == active) { deferred_tab = -1; deferred[0] = 0; }
    loading_tab = active;
    www_set_referer(push && (ci_starts(from, "http://") || ci_starts(from, "https://")) ? from : "");
    www_request(u);
}

static void load_post(const char *action, const char *body, size_t len)
{
    if (www_busy()) { ksnprintf(status, sizeof status, "Still loading: try again in a moment"); return; }
    char from[512];
    strlcpy(from, url, sizeof from);
    push_history();
    if (offer.survive) offer.survive = 0; else offer.active = 0;   /* the bar outlives the reply page */
    strlcpy(url, action, sizeof url);
    strlcpy(addr, action, sizeof addr);
    addr_focus = 0;
    https_guess = 0;
    loading = 1;
    loading_tab = active;
    ksnprintf(status, sizeof status, "Sending…");
    www_post(action, body, len, from);
}

void browser_navigate(const char *u)
{
    char full[512];
    while (is_space(*u)) u++;
    int has_scheme = ci_starts(u, "http://") || ci_starts(u, "https://") || ci_starts(u, "about:") || ci_starts(u, "file://");
    if (has_scheme) strlcpy(full, u, sizeof full);
    else if (u[0] == '/') ksnprintf(full, sizeof full, "file://%s", u);
    else if ((strchr(u, '.') || ci_starts(u, "localhost")) && !strchr(u, ' ')) {
        /* an explicit port other than 443 is almost always plain HTTP (a dev server) */
        const char *colon = strchr(u, ':');
        int port = colon ? katoi(colon + 1) : 443;
        ksnprintf(full, sizeof full, "%s://%s", port == 443 ? "https" : "http", u);
        load(full, 1);
        https_guess = port == 443;
        return;
    }
    else { char q[400]; url_encode(u, q, sizeof q); ksnprintf(full, sizeof full, "https://lite.duckduckgo.com/lite/?q=%s", q); }
    if (!full[0]) strlcpy(full, "about:home", sizeof full);
    load(full, 1);
}

static void go_back(void)
{
    if (!nback) return;
    if (nfwd < HIST) strlcpy(fwd_hist[nfwd++], url, sizeof fwd_hist[0]);
    load(back_hist[--nback], 0);
}

static void go_forward(void)
{
    if (!nfwd) return;
    if (nback < HIST) strlcpy(back_hist[nback++], url, sizeof back_hist[0]);
    load(fwd_hist[--nfwd], 0);
}

/* ── tabs ───────────────────────────────────────────────────────────── */
static void cache_free(struct tab *t)
{
    if (t->cache) for (size_t i = 0; i < t->cache_pages; i++) pmm_free_page((uint64_t)(uintptr_t)t->cache + i * PAGE_SIZE);
    t->cache = 0; t->cache_len = t->cache_pages = 0;
}

static void cache_put(struct tab *t, const char *data, size_t len, int plain, int latin1)
{
    cache_free(t);
    if (!len || len > (2u << 20)) return;               /* big pages are fetched again */
    size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t pg = pmm_alloc_contig(pages);
    if (!pg) return;
    t->cache = (char *)(uintptr_t)pg;
    memcpy(t->cache, data, len);
    t->cache_len = len; t->cache_pages = pages; t->cache_plain = plain; t->cache_latin1 = latin1;
}

static void tab_save_state(int i)
{
    if (i < 0 || i >= ntabs) return;
    struct tab *t = &tabs[i];
    strlcpy(t->url, url, sizeof t->url);
    if (title[0] || !loading) strlcpy(t->title, title, sizeof t->title);
    memcpy(t->back, back_hist, sizeof back_hist); memcpy(t->fwd, fwd_hist, sizeof fwd_hist);
    t->nback = nback; t->nfwd = nfwd; t->scroll = scroll;
}

static void activate(int i)
{
    if (i < 0 || i >= ntabs) return;
    if (i != active && active < ntabs) {
        tab_save_state(active);
        if (!loading || loading_tab != active) cache_put(&tabs[active], src, srclen, src_plain, src_latin1);
    }
    active = i;
    struct tab *t = &tabs[i];
    strlcpy(url, t->url, sizeof url); strlcpy(addr, url, sizeof addr); strlcpy(title, t->title, sizeof title);
    memcpy(back_hist, t->back, sizeof back_hist); memcpy(fwd_hist, t->fwd, sizeof fwd_hist);
    nback = t->nback; nfwd = t->nfwd;
    addr_focus = 0; field_focus = -1; status[0] = 0;
    offer.active = 0;
    if (loading_tab == i) { loading = 1; ksnprintf(status, sizeof status, "Loading…"); return; }
    loading = 0;
    if (t->cache) {
        src_latin1 = t->cache_latin1;
        set_page(t->cache, t->cache_len, t->cache_plain);
        restore_scroll = t->scroll;
        save_session();
    } else load(url[0] ? url : "about:home", 0);
}

static int tab_new(const char *u, int focus)
{
    if (ntabs >= MAXTABS) { ksnprintf(status, sizeof status, "That's the most tabs there can be (%d)", MAXTABS); return -1; }
    struct tab *t = &tabs[ntabs];
    memset(t, 0, sizeof *t);
    strlcpy(t->url, u, sizeof t->url);
    strlcpy(t->title, u, sizeof t->title);
    int n = ntabs++;
    if (focus) activate(n); else save_session();
    return n;
}

static void tab_close(int i)
{
    if (i < 0 || i >= ntabs) return;
    if (ntabs == 1) { tab_new("about:home", 0); }       /* always one tab */
    cache_free(&tabs[i]);
    if (loading_tab == i) loading_tab = -2;              /* its reply is dropped when it comes */
    else if (loading_tab > i) loading_tab--;
    if (deferred_tab == i) { deferred_tab = -1; deferred[0] = 0; } else if (deferred_tab > i) deferred_tab--;
    memmove(&tabs[i], &tabs[i + 1], sizeof tabs[0] * (size_t)(ntabs - i - 1));
    ntabs--;
    if (i == active) {
        int next = i < ntabs ? i : ntabs - 1;
        active = ntabs;                                  /* nothing to save from the closed tab */
        activate(next);
    } else {
        if (active > i) active--;
        save_session();
    }
}

static void extract_title(const char *s, size_t n, char *out, size_t cap)
{
    out[0] = 0;
    const char *t = ci_find(s, s + n, "<title");
    if (!t) return;
    while (t < s + n && *t != '>') t++;
    const char *e = ci_find(t, s + n, "</title");
    if (!e) return;
    size_t k = decode(t + 1, e, out, cap - 1);
    out[k] = 0;
    char *p = out; while (*p == ' ' || *p == '\n') p++;
    memmove(out, p, strlen(p) + 1);
}

static void restore_session(void)
{
    const char *d; size_t n;
    int want = 0;
    if (vfs_read(SESSION, &d, &n) == 0)
        for (size_t i = 0; i < n && ntabs < MAXTABS; ) {
            size_t e = i; while (e < n && d[e] != '\n') e++;
            if (e - i > 7 && !memcmp(d + i, "active\t", 7)) want = katoi(d + i + 7);
            else if (e - i > 4 && !memcmp(d + i, "tab\t", 4)) {
                const char *u = d + i + 4, *tab2 = memchr(u, '\t', (size_t)(d + e - u));
                struct tab *t = &tabs[ntabs];
                memset(t, 0, sizeof *t);
                size_t ul = (size_t)((tab2 ? tab2 : d + e) - u);
                if (ul && ul < sizeof t->url) {
                    memcpy(t->url, u, ul); t->url[ul] = 0;
                    if (tab2) { size_t tl = (size_t)(d + e - tab2 - 1); if (tl >= sizeof t->title) tl = sizeof t->title - 1; memcpy(t->title, tab2 + 1, tl); t->title[tl] = 0; }
                    ntabs++;
                }
            }
            i = e + 1;
        }
    if (!ntabs) { memset(&tabs[0], 0, sizeof tabs[0]); strlcpy(tabs[0].url, "about:home", sizeof tabs[0].url); ntabs = 1; }
    if (want < 0 || want >= ntabs) want = 0;
    active = ntabs;                                      /* nothing to save yet */
    activate(want);
}

/* a page finished loading for a tab that isn't showing: keep it for when it is */
static void stash(int ti, struct http_result *r)
{
    struct tab *t = &tabs[ti];
    if (r->status < 0 || !r->body) { strlcpy(t->title, "Can't open this page", sizeof t->title); cache_free(t); return; }
    strlcpy(t->url, r->url, sizeof t->url);
    const char *ct = r->content_type;
    int plain = !(!ct[0] || ci_find(ct, ct + strlen(ct), "html") || ci_find(ct, ct + strlen(ct), "xml"));
    cache_put(t, r->body, r->len, plain, ci_find(ct, ct + strlen(ct), "iso-8859") != 0);
    extract_title(r->body, r->len, t->title, sizeof t->title);
    if (!t->title[0]) strlcpy(t->title, t->url, sizeof t->title);
}

int browser_tick(void)
{
    if (!inited) { if (ensure_init() == 0) restore_session(); return 1; }
    if (www.state == WWW_DONE && www.gen != (int)loaded_gen) {
        loaded_gen = (uint32_t)www.gen;
        int ti = loading_tab;
        loading_tab = -1;
        struct http_result *r = &www.res;
        int redraw = 0;
        if (ti != active) {                              /* for a tab in the background (or closed) */
            if (ti >= 0 && ti < ntabs) stash(ti, r);
            save_session();
            redraw = 1;
        } else if (loading) {
            loading = 0;
            if ((r->status < 0 || !r->body) && https_guess && ci_starts(url, "https://")) {
                char plain[512];
                ksnprintf(plain, sizeof plain, "http://%s", url + 8);   /* HTTPS didn't work: try HTTP, like other browsers */
                https_guess = 0;
                load(plain, 0);
                return 1;
            }
            https_guess = 0;
            status[0] = 0;
            if (r->status < 0 || !r->body) error_page("Can't open this page", r->error[0] ? r->error : "unknown error");
            else {
                strlcpy(url, r->url, sizeof url);              /* after redirects */
                strlcpy(addr, url, sizeof addr);
                const char *ct = r->content_type;
                src_latin1 = ci_find(ct, ct + strlen(ct), "iso-8859") || ci_find(ct, ct + strlen(ct), "windows-125");
                if (!ct[0] || ci_find(ct, ct + strlen(ct), "html") || ci_find(ct, ct + strlen(ct), "xml")) set_page(r->body, r->len, 0);
                else if (ci_starts(ct, "text/") || ci_find(ct, ct + strlen(ct), "json") || ci_find(ct, ct + strlen(ct), "javascript")) set_page(r->body, r->len, 1);
                else {
                    char m[160];
                    ksnprintf(m, sizeof m, "This is a %s file (%lu bytes). The browser can only show web pages and text.", ct, (unsigned long)r->len);
                    error_page("Can't show this file", m);
                }
                if (r->status >= 400) ksnprintf(status, sizeof status, "HTTP %d", r->status);
                extract_title(src, srclen, title, sizeof title);
            }
            save_session();
            redraw = 1;
        }
        if (deferred[0] && deferred_tab == active && !www_busy()) {   /* this tab was waiting its turn */
            char u[512];
            strlcpy(u, deferred, sizeof u);
            deferred[0] = 0; deferred_tab = -1;
            load(u, 0);
        }
        return redraw;
    }
    if (!loading) {
        if (refresh_pending && layout_w > 0) { refresh_pending = 0; load(refresh_url, 0); return 1; }
        return 0;
    }
    if (loading_tab != active) return 0;                 /* waiting for another tab's load */
    char s[160];
    if (www.state == WWW_LOADING && www.bytes) ksnprintf(s, sizeof s, "Loading… %lu KB", (unsigned long)(www.bytes >> 10));
    else ksnprintf(s, sizeof s, "%s", www.post ? "Sending…" : "Connecting…");
    if (strcmp(s, status)) { strlcpy(status, s, sizeof status); return 1; }
    return 0;
}

/* ── painting ───────────────────────────────────────────────────────── */
static void icon_back(struct canvas *c, int x, int y, uint32_t col, int fwd)
{
    float cx = x + 14, cy = y + 14, d = fwd ? 1 : -1;     /* the tip points where you'd go */
    gfx_line(c, cx - 6 * d, cy, cx + 6 * d, cy, 2, col);
    gfx_line(c, cx + 6 * d, cy, cx + 1 * d, cy - 5, 2, col);
    gfx_line(c, cx + 6 * d, cy, cx + 1 * d, cy + 5, 2, col);
}

static void icon_reload(struct canvas *c, int x, int y, uint32_t col)
{
    float cx = x + 14, cy = y + 14, r = 6;
    float px = cx + r, py = cy;
    for (int i = 1; i <= 20; i++) {
        float th = i / 20.0f * 1.6f * GFX_PI;
        float nx = cx + gfx_cos(th) * r, ny = cy - gfx_sin(th) * r;
        gfx_line(c, px, py, nx, ny, 1.8f, col);
        px = nx; py = ny;
    }
    gfx_line(c, cx + r, cy, cx + r - 3, cy - 4, 1.8f, col);
    gfx_line(c, cx + r, cy, cx + r + 3, cy - 4, 1.8f, col);
}

static void icon_home(struct canvas *c, int x, int y, uint32_t col)
{
    float cx = x + 14, cy = y + 14;
    gfx_line(c, cx - 7, cy, cx, cy - 7, 1.8f, col);
    gfx_line(c, cx, cy - 7, cx + 7, cy, 1.8f, col);
    gfx_frame(c, (int)cx - 5, (int)cy - 1, 11, 8, 1, col);
}

void browser_paint(const struct app_ctx *a)
{
    struct canvas *c = a->c;
    const struct app_colors *k = a->col;
    int bar = offer.active ? LOGINBAR : 0;                     /* "Save password?" pushes the page down */
    view_w = a->w; view_h = a->h - TOP - STATUSBAR - bar;
    if (!inited) { gfx_fill(c, a->x, a->y, a->w, a->h, k->bg); return; }
    if (layout_w != a->w - 12) layout(a->w - 12);
    if (restore_scroll >= 0) { scroll = restore_scroll; restore_scroll = -1; }
    if (scroll > doc_h - view_h) scroll = doc_h - view_h;
    if (scroll < 0) scroll = 0;

    /* page */
    int px = a->x, py = a->y + TOP + bar;
    gfx_fill(c, px, py, a->w, view_h, k->bg);
    gfx_clip(c, px, py, a->w, view_h);
    /* first visible item: binary search on y */
    int lo = 0, hi = nitems;
    while (lo < hi) { int mid = (lo + hi) / 2; if (items[mid].y + 40 < scroll) lo = mid + 1; else hi = mid; }
    for (int i = lo > 64 ? lo - 64 : 0; i < nitems; i++) {
        const struct item *it = &items[i];
        int x = px + it->x, y = py + it->y - scroll;
        if (y > py + view_h) break;
        if (y + 30 < py) continue;
        if (it->flags & IT_RULE) { gfx_fill(c, x, y + 4, it->w, 1, with_alpha(k->fg, 60)); continue; }
        if ((it->flags & IT_FIELD) && it->link <= -100) {
            struct field *f = &fields[-100 - it->link];
            int fh = 24, fy = y - 4;
            f->x = it->x; f->y = it->y - 4; f->w = it->w; f->h = fh;
            if (f->type == FT_CHECK) {
                gfx_round(c, x, fy + 3, 18, 18, 4, f->checked ? k->accent : k->field, 0);
                gfx_round_frame(c, x, fy + 3, 18, 18, 4, f->checked ? k->accent : with_alpha(k->fg, 90));
                if (f->checked) { gfx_line(c, x + 4, fy + 12, x + 8, fy + 16, 2, k->accent_fg); gfx_line(c, x + 8, fy + 16, x + 14, fy + 7, 2, k->accent_fg); }
            } else if (f->type == FT_SUBMIT) {
                gfx_round(c, x, fy, it->w, fh, 6, k->button, 0);
                gfx_text(c, &font_ui, x + 12, fy + 4, f->value[0] ? f->value : "Submit", k->button_fg);
            } else {
                int foc = field_focus == -100 - it->link;
                gfx_round(c, x, fy, it->w, fh, 5, k->field, 0);
                gfx_round_frame(c, x, fy, it->w, fh, 5, foc ? k->accent : with_alpha(k->fg, 70));
                char shown[256];
                if (f->type == FT_PASSWORD) { size_t n = strlen(f->value); for (size_t q = 0; q < n && q < 255; q++) shown[q] = '*'; shown[n < 255 ? n : 255] = 0; }
                else strlcpy(shown, f->value, sizeof shown);
                gfx_clip(c, x + 4, fy, it->w - 8, fh);
                int tw = gfx_text(c, &font_ui, x + 6, fy + 4, shown, k->fg);
                if (foc && a->focused && !(a->blink & 1)) gfx_fill(c, x + 6 + tw, fy + 5, 1, 14, k->fg);
                gfx_noclip(c);
                gfx_clip(c, px, py, a->w, view_h);
            }
            continue;
        }
        const char *s = arena + it->off;
        const struct font *f = FONTS[it->font];
        uint32_t col = k->fg;
        if (it->flags & IT_DIM) col = k->dim;
        if (it->flags & IT_ACCENT) col = it->font == F_HEAD ? k->fg : k->accent;
        if (it->flags & IT_BULLET) col = k->accent;
        if (it->link >= 0) col = it->link == hover_link ? k->accent : (k->light ? 0xff1a5fd0 : gfx_mix(k->accent, 0xff9ecbff, 0.5f));
        if ((it->flags & IT_FIELD) && it->link <= -2) {           /* <button> text */
            gfx_round(c, x - 6, y - 3, it->w + 12, f->height + 4, 5, k->button, 0);
            col = k->button_fg;
        }
        gfx_text(c, f, x, y, s, col);
        if ((it->flags & IT_UNDER) && it->link >= 0) gfx_fill(c, x, y + f->baseline + 2, it->w, 1, with_alpha(col, it->link == hover_link ? 255 : 110));
    }
    gfx_noclip(c);
    if (offer.active) {                                         /* "Save password?" */
        int by = a->y + TOP;
        gfx_fill(c, px, by, a->w, LOGINBAR, k->field);
        gfx_fill(c, px, by + LOGINBAR - 1, a->w, 1, with_alpha(k->fg, 40));
        char m[200];
        ksnprintf(m, sizeof m, "Save the password%s%s for %s?", offer.user[0] ? " for " : "", offer.user, offer.site);
        gfx_text_fit(c, &font_ui, px + 14, by + 10, a->w - 300, m, k->fg);
        gfx_round(c, px + a->w - 250, by + 6, 90, 26, 6, k->accent, 0);
        gfx_text(c, &font_bold, px + a->w - 250 + (90 - gfx_text_width(&font_bold, "Save")) / 2, by + 10, "Save", k->accent_fg);
        gfx_round(c, px + a->w - 150, by + 6, 90, 26, 6, k->button, 0);
        gfx_text(c, &font_ui, px + a->w - 150 + (90 - gfx_text_width(&font_ui, "Never")) / 2, by + 10, "Never", k->button_fg);
        gfx_line(c, px + a->w - 38, by + 13, px + a->w - 26, by + 25, 1.6f, k->dim);
        gfx_line(c, px + a->w - 26, by + 13, px + a->w - 38, by + 25, 1.6f, k->dim);
    }
    /* scrollbar */
    if (doc_h > view_h) {
        int th = view_h * view_h / doc_h; if (th < 24) th = 24;
        int ty = py + (view_h - th) * scroll / (doc_h - view_h);
        gfx_round(c, px + a->w - 8, ty, 5, th, 2, with_alpha(k->fg, 70), 0);
    }

    /* tabs */
    gfx_fill(c, a->x, a->y, a->w, TABBAR, gfx_mix(k->field, k->bg, 0.5f));
    {
        int avail = a->w - 48, tw = ntabs ? avail / ntabs : avail;
        if (tw > 220) tw = 220;
        for (int i = 0; i < ntabs; i++) {
            int x0 = a->x + 6 + i * tw, act = i == active;
            const char *tt = act ? (title[0] ? title : (url[0] ? url : "New tab")) : (tabs[i].title[0] ? tabs[i].title : tabs[i].url);
            if (ci_starts(tt, "about:home")) tt = "Start";
            if (act) gfx_round(c, x0, a->y + 4, tw - 4, TABBAR - 4, 7, k->field, 0);
            else if (i + 1 < ntabs && i + 1 != active) gfx_fill(c, x0 + tw - 4, a->y + 10, 1, TABBAR - 18, with_alpha(k->fg, 50));
            gfx_text_fit(c, act ? &font_bold : &font_ui, x0 + 12, a->y + 10, tw - 44, tt, act ? k->fg : k->dim);
            if (i == loading_tab) gfx_fill(c, x0 + 10, a->y + TABBAR - 4, tw - 24, 2, k->accent);
            int cx = x0 + tw - 22, cy = a->y + 17;                 /* close × */
            gfx_line(c, cx - 4, cy - 4, cx + 4, cy + 4, 1.4f, k->dim);
            gfx_line(c, cx + 4, cy - 4, cx - 4, cy + 4, 1.4f, k->dim);
        }
        int plx = a->x + 6 + ntabs * tw + 8, ply = a->y + 17;      /* new tab + */
        if (ntabs < MAXTABS) { gfx_line(c, plx + 3, ply, plx + 15, ply, 1.8f, k->fg); gfx_line(c, plx + 9, ply - 6, plx + 9, ply + 6, 1.8f, k->fg); }
    }

    /* toolbar */
    int tx = a->x, ty = a->y + TABBAR;
    gfx_fill(c, tx, ty, a->w, TOOLBAR, k->field);
    gfx_fill(c, tx, ty + TOOLBAR - 1, a->w, 1, with_alpha(k->fg, 40));
    icon_back(c, tx + 6, ty + 8, nback ? k->fg : with_alpha(k->fg, 70), 0);
    icon_back(c, tx + 36, ty + 8, nfwd ? k->fg : with_alpha(k->fg, 70), 1);
    icon_reload(c, tx + 66, ty + 8, k->fg);
    icon_home(c, tx + 96, ty + 8, k->fg);
    int ax = tx + 132, aw = a->w - 144;
    gfx_round(c, ax, ty + 7, aw, 30, 15, k->bg, 0);
    gfx_round_frame(c, ax, ty + 7, aw, 30, 15, addr_focus ? k->accent : with_alpha(k->fg, 50));
    gfx_clip(c, ax + 12, ty + 7, aw - 24, 30);
    if (addr_focus) {
        int w = gfx_text_width(&font_ui, addr);
        if (addr_all && addr[0]) gfx_fill(c, ax + 13, ty + 13, w, 18, k->sel);
        gfx_text(c, &font_ui, ax + 14, ty + 14, addr, k->fg);
        char tmp[512]; strlcpy(tmp, addr, sizeof tmp); tmp[addr_caret] = 0;
        if (a->focused && !(a->blink & 1)) gfx_fill(c, ax + 14 + gfx_text_width(&font_ui, tmp), ty + 14, 1, 16, k->fg);
    } else {
        int secure = ci_starts(url, "https://");
        int xx = ax + 14;
        if (secure) {                                          /* a little padlock */
            gfx_ring(c, xx + 5, ty + 17, 3.5f, 1.6f, k->accent);
            gfx_round(c, xx, ty + 18, 11, 9, 2, k->accent, 0);
            xx += 18;
        }
        const char *show = url;
        if (ci_starts(show, "https://")) show += 8; else if (ci_starts(show, "http://")) show += 7;
        gfx_text_fit(c, &font_ui, xx, ty + 14, aw - 28, show, k->fg);
    }
    gfx_noclip(c);
    if (loading) {                                             /* a sliding bar under the toolbar */
        int bw = a->w / 4, off = (int)((a->blink * 37) % (uint32_t)(a->w + bw)) - bw;
        gfx_fill(c, tx + (off < 0 ? 0 : off), ty + TOOLBAR - 2, bw + (off < 0 ? off : 0), 2, k->accent);
    }

    /* status bar */
    int sy = a->y + a->h - STATUSBAR;
    gfx_fill(c, a->x, sy, a->w, STATUSBAR, k->field);
    const char *st = status;
    if (hover_link >= 0 && hover_link < nlinks) st = arena + links[hover_link];
    else if (!st[0]) st = title[0] ? title : "";
    gfx_text_fit(c, &font_ui, a->x + 10, sy + 3, a->w - 20, st, k->dim);
}

/* ── input ──────────────────────────────────────────────────────────── */
static void submit(int form, int submitter)
{
    if (!nforms) return;
    if (form < 0 || form >= nforms) form = 0;
    static char q[12288];
    char enc[800], encn[160];
    size_t ql = 0;
    for (int i = 0; i < nfields; i++) {
        struct field *f = &fields[i];
        if (f->form != form || !f->name[0]) continue;
        if (f->type == FT_SUBMIT && i != submitter) continue;      /* only the button that was pressed */
        if (f->type == FT_CHECK && !f->checked) continue;
        url_encode(f->name, encn, sizeof encn);
        url_encode(f->value, enc, sizeof enc);
        int w = ksnprintf(q + ql, sizeof q - ql, "%s%s=%s", ql ? "&" : "", encn, enc);
        if (w < 0 || ql + (size_t)w >= sizeof q) break;
        ql += (size_t)w;
    }
    offer_login(form);
    if (forms[form].post) { load_post(forms[form].action, q, ql); return; }
    char target[1024];
    strlcpy(target, forms[form].action, sizeof target);
    char *qm = strchr(target, '?');
    if (qm) *qm = 0;
    char full[1600];
    ksnprintf(full, sizeof full, "%s?%s", target, q);
    load(full, 1);
}

static int item_at(int x, int y)
{
    int dy = y + scroll;
    for (int i = 0; i < nitems; i++) {
        const struct item *it = &items[i];
        if (it->y > dy + 4) break;
        int h = FONTS[it->font]->height;
        if ((it->flags & IT_FIELD) && it->link <= -100) { if (x >= it->x && x < it->x + it->w && dy >= it->y - 4 && dy < it->y + 20) return i; continue; }
        if (x >= it->x - 2 && x < it->x + it->w + 2 && dy >= it->y - 2 && dy < it->y + h) return i;
    }
    return -1;
}

int browser_hover(int x, int y)
{
    int bar = offer.active ? LOGINBAR : 0;
    if (!inited || y < TOP + bar || y >= TOP + bar + view_h) { if (hover_link != -1) { hover_link = -1; return 1; } return 0; }
    int i = item_at(x, y - TOP - bar);
    int l = i >= 0 && items[i].link >= 0 ? items[i].link : -1;
    if (l != hover_link) { hover_link = l; return 1; }
    return 0;
}

int browser_click(int x, int y, int w, int h)
{
    (void)h;
    if (!inited) return 0;
    if (y < TABBAR) {                                   /* tabs */
        field_focus = -1; addr_focus = 0;
        int avail = w - 48, tw = ntabs ? avail / ntabs : avail;
        if (tw > 220) tw = 220;
        int i = (x - 6) / (tw > 0 ? tw : 1);
        if (x >= 6 && i < ntabs) {
            int x0 = 6 + i * tw;
            if (x >= x0 + tw - 32) tab_close(i);
            else if (i != active) activate(i);
            return 1;
        }
        int plx = 6 + ntabs * tw + 8;
        if (x >= plx - 4 && x < plx + 24 && tab_new("about:home", 1) >= 0) { addr_focus = 1; addr_all = 1; addr[0] = 0; addr_caret = 0; }
        return 1;
    }
    y -= TABBAR;
    if (y < TOOLBAR) {
        field_focus = -1;
        if (x < 34) { go_back(); return 1; }
        if (x < 64) { go_forward(); return 1; }
        if (x < 94) { load(url, 0); return 1; }
        if (x < 124) { load("about:home", 1); return 1; }
        if (x >= 132 && x < w - 12) {
            if (!addr_focus) { addr_focus = 1; addr_all = 1; strlcpy(addr, url, sizeof addr); }
            else addr_all = 0;
            addr_caret = (int)strlen(addr);
            return 1;
        }
        return 0;
    }
    addr_focus = 0;
    int bar = offer.active ? LOGINBAR : 0;
    if (y >= TOOLBAR + bar + view_h) return 0;
    if (offer.active && y < TOOLBAR + LOGINBAR) {          /* the "Save password?" bar */
        if (x >= w - 250 && x < w - 160) login_store(offer.site, offer.user, offer.pass, 0);
        else if (x >= w - 150 && x < w - 60) login_store(offer.site, "", "", 1);
        else if (x < w - 44) return 1;
        offer.active = 0;
        if (x < w - 44) ksnprintf(status, sizeof status, "%s", x < w - 160 ? "Password saved" : "Passwords won't be saved for this site");
        return 1;
    }
    int i = item_at(x, y - TOOLBAR - bar);
    field_focus = -1;
    if (i < 0) return 1;
    const struct item *it = &items[i];
    if (it->link >= 0) {
        const char *target = arena + links[it->link];
        if (input_ctrl()) { char t2[512]; strlcpy(t2, target, sizeof t2); tab_new(t2, 0); ksnprintf(status, sizeof status, "Opened in a new tab"); return 1; }
        if (!strcmp(target, url)) { scroll = 0; return 1; }
        load(target, 1);
        return 1;
    }
    if (it->link <= -100) {
        int fi = -100 - it->link;
        if (fields[fi].type == FT_SUBMIT) submit(fields[fi].form, fi);
        else if (fields[fi].type == FT_CHECK) fields[fi].checked = !fields[fi].checked;
        else { field_focus = fi; field_caret = (int)strlen(fields[fi].value); }
        return 1;
    }
    if (it->link <= -2) { submit(-2 - it->link, -1); return 1; }
    return 1;
}

static int edit(char *buf, size_t cap, int *caret, int k)
{
    int n = (int)strlen(buf);
    if (*caret > n) *caret = n;
    if (k == '\b') { if (*caret > 0) { int p = *caret - 1; while (p > 0 && ((unsigned char)buf[p] & 0xC0) == 0x80) p--; memmove(buf + p, buf + *caret, (size_t)(n - *caret + 1)); *caret = p; } return 1; }
    if (k == KEY_DEL) { if (*caret < n) { int q = *caret + 1; while (q < n && ((unsigned char)buf[q] & 0xC0) == 0x80) q++; memmove(buf + *caret, buf + q, (size_t)(n - q + 1)); } return 1; }
    if (k == KEY_LEFT) { if (*caret > 0) (*caret)--; while (*caret > 0 && ((unsigned char)buf[*caret] & 0xC0) == 0x80) (*caret)--; return 1; }
    if (k == KEY_RIGHT) { if (*caret < n) (*caret)++; while (*caret < n && ((unsigned char)buf[*caret] & 0xC0) == 0x80) (*caret)++; return 1; }
    if (k == KEY_HOME) { *caret = 0; return 1; }
    if (k == KEY_END) { *caret = n; return 1; }
    if (k == 0x15) { buf[0] = 0; *caret = 0; return 1; }      /* Ctrl+U */
    if (k >= 32 && k < 127 && (size_t)n + 1 < cap) {
        memmove(buf + *caret + 1, buf + *caret, (size_t)(n - *caret + 1));
        buf[*caret] = (char)k;
        (*caret)++;
        return 1;
    }
    return 0;
}

int browser_key(int k)
{
    if (!inited) return 0;
    if (k == 0x0C) { addr_focus = 1; addr_all = 1; strlcpy(addr, url, sizeof addr); addr_caret = (int)strlen(addr); field_focus = -1; return 1; }
    if (k == 0x12) { load(url, 0); return 1; }                 /* Ctrl+R */
    if (k == 0x14) {                                           /* Ctrl+T: new tab */
        if (tab_new("about:home", 1) >= 0) { addr_focus = 1; addr_all = 1; addr[0] = 0; addr_caret = 0; field_focus = -1; }
        return 1;
    }
    if (k == 0x17) { tab_close(active); return 1; }            /* Ctrl+W */
    if (k == '\t' && input_ctrl()) { if (ntabs > 1) activate((active + (input_shift() ? ntabs - 1 : 1)) % ntabs); return 1; }
    if (addr_focus) {
        if (k == '\n') { addr_focus = 0; browser_navigate(addr); return 1; }
        if (k == 27) { addr_focus = 0; strlcpy(addr, url, sizeof addr); return 1; }
        if (addr_all && ((k >= 32 && k < 127) || k == '\b' || k == KEY_DEL)) { addr[0] = 0; addr_caret = 0; addr_all = 0; if (k == '\b' || k == KEY_DEL) return 1; }
        addr_all = 0;
        return edit(addr, sizeof addr, &addr_caret, k);
    }
    if (field_focus >= 0) {
        struct field *f = &fields[field_focus];
        if (k == '\n') { submit(f->form, -1); return 1; }
        if (k == 27) { field_focus = -1; return 1; }
        if (k == '\t') {
            for (int i = field_focus + 1; i < nfields; i++) if (fields[i].type == FT_TEXT || fields[i].type == FT_PASSWORD) { field_focus = i; field_caret = (int)strlen(fields[i].value); return 1; }
            field_focus = -1; return 1;
        }
        return edit(f->value, sizeof f->value, &field_caret, k);
    }
    int page = view_h - 40;
    switch (k) {
    case KEY_DOWN: scroll += 40; return 1;
    case KEY_UP: scroll -= 40; return 1;
    case KEY_PGDN: case ' ': scroll += page; return 1;
    case KEY_PGUP: scroll -= page; return 1;
    case KEY_HOME: scroll = 0; return 1;
    case KEY_END: scroll = doc_h; return 1;
    case '\b': go_back(); return 1;
    case '\t':
        for (int i = 0; i < nfields; i++) if (fields[i].type == FT_TEXT || fields[i].type == FT_PASSWORD) { field_focus = i; field_caret = (int)strlen(fields[i].value); return 1; }
        return 0;
    }
    return 0;
}

int browser_wheel(int dz)
{
    scroll += dz * 60;
    return 1;
}
