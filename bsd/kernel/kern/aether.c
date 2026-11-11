/* Aether on AuxoBSD: a native port of Aether (github.com/rxvy-dev/aether), the code editor.
 *
 * Aether itself is Python + GTK 4; AuxoBSD has neither, so this is a re-implementation for the
 * Chrono desktop that keeps Aether's look and data: its colour schemes (Aether Crimson by
 * default), its starter templates for 34 languages, the explorer / tabs / editor / status bar
 * layout, auto-indent, bracket closing, and the HTML live preview (here: the Chrono Browser).
 * Files live in the AuxoBSD filesystem (vfs), under /home/root. */
#include "kern.h"
#include "gfx.h"
#include "fonts.h"
#include "apps.h"
#include "vfs.h"
#include "rs.h"
#include "aether_data.h"

#define SIDEBAR 210
#define HEADER 40
#define TABS 30
#define STATUS 24
#define LINE_H 18
#define CHAR_W 8
#define MAXBUF 8
#define BUFCAP (1u << 20)
#define MAXLINES 65536
#define UNDO 256

struct op { int ins; int pos; int len; char *text; };   /* one edit, for undo */
struct buffer {
    int used, dirty, lang, hl;
    char path[VFS_PATH];
    char *text; int len;
    int caret, anchor;                 /* selection = anchor..caret when anchor >= 0 */
    int top, left;                     /* first visible line, first visible column */
    struct op undo[UNDO]; int nundo, nredo;
};

static struct buffer bufs[MAXBUF];
static int cur = -1;
static int theme = AETHER_THEME_DEFAULT;
static int lines[MAXLINES], nlines;
static char clip[65536]; static int cliplen;
static int overlay;                    /* 0, OV_NEW, OV_THEME, OV_FIND */
enum { OV_NONE, OV_NEW, OV_THEME, OV_FIND };
static char findbuf[128]; static int findlen;
static char message[160];
static int view_rows = 30, view_cols = 80, win_w, win_h;
static char title_buf[160];
static int explorer_scroll;

static const struct aether_theme *TH(void) { return &aether_themes[theme]; }

const char *aether_title(void)
{
    if (cur < 0) return "Aether";
    const char *p = strrchr(bufs[cur].path, '/');
    ksnprintf(title_buf, sizeof title_buf, "%s%s — Aether", bufs[cur].dirty ? "● " : "", p ? p + 1 : bufs[cur].path);
    return title_buf;
}

/* ── buffers ────────────────────────────────────────────────────────── */
static int lang_for(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    static const struct { const char *ext, *id; } map[] = {
        { ".c", "c" }, { ".h", "c" }, { ".cpp", "cpp" }, { ".cc", "cpp" }, { ".hpp", "cpp" }, { ".py", "python" },
        { ".js", "javascript" }, { ".mjs", "javascript" }, { ".ts", "typescript" }, { ".jsx", "javascriptreact" },
        { ".tsx", "typescriptreact" }, { ".rs", "rust" }, { ".go", "go" }, { ".java", "java" }, { ".cs", "csharp" },
        { ".kt", "kotlin" }, { ".swift", "swift" }, { ".dart", "dart" }, { ".scala", "scala" }, { ".sh", "shellscript" },
        { ".lua", "lua" }, { ".pl", "perl" }, { ".rb", "ruby" }, { ".php", "php" }, { ".hs", "haskell" }, { ".zig", "zig" },
        { ".ex", "elixir" }, { ".exs", "elixir" }, { ".R", "r" }, { ".r", "r" }, { ".jl", "julia" }, { ".html", "html" },
        { ".htm", "html" }, { ".css", "css" }, { ".sql", "sql" }, { ".toml", "toml" }, { ".yaml", "yaml" }, { ".yml", "yaml" },
        { ".md", "markdown" }, { ".json", "json" }, { 0, 0 }
    };
    const char *id = 0;
    if (!strcmp(base, "Makefile") || !strcmp(base, "makefile")) id = "makefile";
    else if (!strcmp(base, "Dockerfile")) id = "dockerfile";
    else if (dot) for (int i = 0; map[i].ext; i++) if (!strcmp(dot, map[i].ext)) { id = map[i].id; break; }
    if (id) for (int i = 0; i < AETHER_LANG_COUNT; i++) if (!strcmp(aether_langs[i].id, id)) return i;
    return -1;
}

static void reindex(struct buffer *b)
{
    nlines = 0;
    lines[nlines++] = 0;
    for (int i = 0; i < b->len && nlines < MAXLINES; i++) if (b->text[i] == '\n') lines[nlines++] = i + 1;
}

static int line_of(int pos)
{
    int lo = 0, hi = nlines - 1;
    while (lo < hi) { int m = (lo + hi + 1) / 2; if (lines[m] <= pos) lo = m; else hi = m - 1; }
    return lo;
}
static int line_end(const struct buffer *b, int ln) { return ln + 1 < nlines ? lines[ln + 1] - 1 : b->len; }

static int display_col(const struct buffer *b, int ln, int pos)
{
    int col = 0;
    for (int i = lines[ln]; i < pos && i < b->len; i++) {
        if (b->text[i] == '\t') col = (col / 4 + 1) * 4;
        else if (((unsigned char)b->text[i] & 0xC0) != 0x80) col++;
    }
    return col;
}

static int pos_at_col(const struct buffer *b, int ln, int want)
{
    int col = 0, i = lines[ln], e = line_end(b, ln);
    while (i < e) {
        int nc = b->text[i] == '\t' ? (col / 4 + 1) * 4 : col + 1;
        if (nc > want) { if (want - col > nc - want) i++; break; }
        col = nc;
        i++;
        while (i < e && ((unsigned char)b->text[i] & 0xC0) == 0x80) i++;
    }
    return i;
}

static struct buffer *open_buffer(const char *path, const char *text, size_t len)
{
    for (int i = 0; i < MAXBUF; i++) if (bufs[i].used && !strcmp(bufs[i].path, path)) { cur = i; return &bufs[i]; }
    int slot = -1;
    for (int i = 0; i < MAXBUF; i++) if (!bufs[i].used) { slot = i; break; }
    if (slot < 0) { ksnprintf(message, sizeof message, "Close a tab first (up to %d files open)", MAXBUF); return 0; }
    struct buffer *b = &bufs[slot];
    if (!b->text) {
        uint64_t pg = pmm_alloc_contig(BUFCAP / PAGE_SIZE);
        if (!pg) { ksnprintf(message, sizeof message, "Out of memory"); return 0; }
        b->text = (char *)(uintptr_t)pg;
    }
    if (len > BUFCAP - 1) len = BUFCAP - 1;
    memcpy(b->text, text, len);
    b->len = (int)len;
    b->used = 1; b->dirty = 0; b->caret = 0; b->anchor = -1; b->top = b->left = 0;
    for (int i = 0; i < b->nundo + b->nredo && i < UNDO; i++) if (b->undo[i].text) rs_kfree(b->undo[i].text);
    b->nundo = b->nredo = 0;
    memset(b->undo, 0, sizeof b->undo);
    strlcpy(b->path, path, sizeof b->path);
    b->lang = lang_for(path);
    b->hl = b->lang >= 0 ? aether_langs[b->lang].hl : HL_PLAIN;
    cur = slot;
    return b;
}

void aether_open(const char *path)
{
    const char *d; size_t n;
    if (vfs_read(path, &d, &n) < 0) { d = ""; n = 0; }
    open_buffer(path, d, n);
}

static void save(void)
{
    if (cur < 0) return;
    struct buffer *b = &bufs[cur];
    if (vfs_write(b->path, b->text, (size_t)b->len) < 0) { ksnprintf(message, sizeof message, "Couldn't save %s", b->path); return; }
    b->dirty = 0;
    ksnprintf(message, sizeof message, "Saved %s (%d bytes)", b->path, b->len);
}

static void close_tab(int i)
{
    if (i < 0 || !bufs[i].used) return;
    if (bufs[i].dirty) save();
    bufs[i].used = 0;
    if (cur == i) { cur = -1; for (int k = 0; k < MAXBUF; k++) if (bufs[k].used) { cur = k; break; } }
}

static void new_from_template(int lang)
{
    const struct aether_lang *l = &aether_langs[lang];
    char path[VFS_PATH];
    const char *dot = strrchr(l->file, '.');
    size_t stem = dot ? (size_t)(dot - l->file) : strlen(l->file);
    ksnprintf(path, sizeof path, "/home/root/%s", l->file);
    for (int n = 2; vfs_exists(path) && n < 100; n++) {
        char s[64]; memcpy(s, l->file, stem); s[stem] = 0;
        ksnprintf(path, sizeof path, "/home/root/%s%d%s", s, n, dot ? dot : "");
    }
    vfs_write(path, l->main, strlen(l->main));
    aether_open(path);
    if (cur >= 0) {
        /* caret where the template's body goes: after the first "{\n\t" or at the end */
        struct buffer *b = &bufs[cur];
        const char *brace = strchr(b->text, '{');
        b->caret = brace ? (int)(brace - b->text) + 1 : b->len;
        while (b->caret < b->len && (b->text[b->caret] == '\n' || b->text[b->caret] == '\t')) b->caret++;
        ksnprintf(message, sizeof message, "New %s file from Aether's template: %s", l->name, path);
    }
}

/* ── editing with undo ──────────────────────────────────────────────── */
static void push_op(struct buffer *b, int ins, int pos, const char *t, int len)
{
    for (int i = b->nundo; i < b->nundo + b->nredo && i < UNDO; i++) if (b->undo[i].text) { rs_kfree(b->undo[i].text); b->undo[i].text = 0; }
    b->nredo = 0;
    /* merge consecutive single-character typing into one step */
    if (b->nundo && ins && len == 1 && t[0] != '\n') {
        struct op *o = &b->undo[b->nundo - 1];
        if (o->ins && o->pos + o->len == pos && o->len < 64) {
            char *nt = rs_kmalloc((size_t)o->len + 1);
            if (nt) { memcpy(nt, o->text, (size_t)o->len); nt[o->len] = t[0]; rs_kfree(o->text); o->text = nt; o->len++; return; }
        }
    }
    if (b->nundo == UNDO) { rs_kfree(b->undo[0].text); memmove(b->undo, b->undo + 1, sizeof b->undo[0] * (UNDO - 1)); b->nundo--; }
    char *copy = rs_kmalloc((size_t)len + 1);
    if (!copy) return;
    memcpy(copy, t, (size_t)len);
    b->undo[b->nundo++] = (struct op){ ins, pos, len, copy };
}

static void raw_insert(struct buffer *b, int pos, const char *t, int len)
{
    if (b->len + len >= (int)BUFCAP) return;
    memmove(b->text + pos + len, b->text + pos, (size_t)(b->len - pos));
    memcpy(b->text + pos, t, (size_t)len);
    b->len += len;
}
static void raw_delete(struct buffer *b, int pos, int len)
{
    memmove(b->text + pos, b->text + pos + len, (size_t)(b->len - pos - len));
    b->len -= len;
}

static void insert(struct buffer *b, const char *t, int len)
{
    if (b->len + len >= (int)BUFCAP) { ksnprintf(message, sizeof message, "File too big"); return; }
    push_op(b, 1, b->caret, t, len);
    raw_insert(b, b->caret, t, len);
    b->caret += len;
    b->dirty = 1;
}

static void delete_range(struct buffer *b, int a, int e)
{
    if (a > e) { int t = a; a = e; e = t; }
    if (a < 0) a = 0;
    if (e > b->len) e = b->len;
    if (a == e) return;
    push_op(b, 0, a, b->text + a, e - a);
    raw_delete(b, a, e - a);
    b->caret = a;
    b->dirty = 1;
}

static int has_sel(const struct buffer *b) { return b->anchor >= 0 && b->anchor != b->caret; }
static void delete_sel(struct buffer *b) { if (has_sel(b)) delete_range(b, b->anchor, b->caret); b->anchor = -1; }

static void undo(struct buffer *b, int redo)
{
    if (redo) {
        if (!b->nredo) return;
        struct op *o = &b->undo[b->nundo++]; b->nredo--;
        if (o->ins) { raw_insert(b, o->pos, o->text, o->len); b->caret = o->pos + o->len; }
        else { raw_delete(b, o->pos, o->len); b->caret = o->pos; }
    } else {
        if (!b->nundo) return;
        struct op *o = &b->undo[--b->nundo]; b->nredo++;
        if (o->ins) { raw_delete(b, o->pos, o->len); b->caret = o->pos; }
        else { raw_insert(b, o->pos, o->text, o->len); b->caret = o->pos + o->len; }
    }
    b->anchor = -1;
    b->dirty = 1;
}

/* ── syntax highlighting ────────────────────────────────────────────── */
enum { T_TEXT, T_KEYWORD, T_TYPE, T_STRING, T_COMMENT, T_NUMBER, T_FUNCTION, T_PREPROC, T_OPERATOR, T_HEADING };

static const char *const KW_C[] = { "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue",
    "return", "goto", "struct", "union", "enum", "typedef", "sizeof", "static", "const", "extern", "inline", "volatile",
    "register", "class", "public", "private", "protected", "new", "delete", "this", "namespace", "using", "template",
    "typename", "virtual", "override", "final", "try", "catch", "throw", "import", "package", "interface", "extends",
    "implements", "fun", "val", "var", "let", "func", "null", "nullptr", "true", "false", "self", "super", "in", "is",
    "as", "echo", "function", "constexpr", "auto", "operator", "friend", "throws", "abstract", "object", "when",
    "guard", "struct", "async", "await", "foreach", "readonly", "base", "out", "ref", "def", "where", 0 };
static const char *const TY_C[] = { "int", "char", "void", "float", "double", "long", "short", "unsigned", "signed",
    "bool", "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t",
    "string", "String", "boolean", "byte", "Int", "Double", "Float", "Bool", "Unit", "Any", "std", "vector", "FILE", 0 };
static const char *const KW_PY[] = { "def", "class", "if", "elif", "else", "for", "while", "return", "import", "from",
    "as", "with", "try", "except", "finally", "raise", "pass", "break", "continue", "lambda", "yield", "None", "True",
    "False", "and", "or", "not", "in", "is", "global", "nonlocal", "async", "await", "end", "do", "module", "require",
    "nil", "self", "puts", "function", "begin", "unless", "then", "defmodule", "defp", "fn", "using", "struct",
    "TRUE", "FALSE", "NULL", "library", "print", 0 };
static const char *const KW_JS[] = { "var", "let", "const", "function", "return", "if", "else", "for", "while", "do",
    "switch", "case", "break", "continue", "new", "this", "class", "extends", "import", "export", "from", "default",
    "async", "await", "try", "catch", "finally", "throw", "typeof", "instanceof", "null", "undefined", "true", "false",
    "of", "in", "yield", "interface", "type", "enum", "implements", "readonly", "as", "static", "get", "set", 0 };
static const char *const TY_JS[] = { "string", "number", "boolean", "any", "void", "never", "unknown", "object",
    "Array", "Promise", "Map", "Set", "React", "console", "document", "window", "JSON", "Math", 0 };
static const char *const KW_RS[] = { "fn", "let", "mut", "pub", "struct", "enum", "impl", "trait", "use", "mod",
    "crate", "self", "Self", "match", "if", "else", "loop", "while", "for", "in", "return", "break", "continue", "as",
    "ref", "move", "unsafe", "where", "async", "await", "dyn", "const", "static", "type", "true", "false", "Some",
    "None", "Ok", "Err", "var", "try", "defer", "errdefer", "comptime", "test", "orelse", "catch", "union", 0 };
static const char *const TY_RS[] = { "i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64", "u128",
    "usize", "f32", "f64", "bool", "char", "str", "String", "Vec", "Option", "Result", "Box", "void", "anyerror", 0 };
static const char *const KW_GO[] = { "func", "package", "import", "var", "const", "type", "struct", "interface", "map",
    "chan", "go", "defer", "select", "switch", "case", "default", "if", "else", "for", "range", "return", "break",
    "continue", "fallthrough", "nil", "true", "false", "make", "new", "len", "append", 0 };
static const char *const TY_GO[] = { "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32",
    "uint64", "float32", "float64", "string", "bool", "byte", "rune", "error", "any", 0 };
static const char *const KW_SH[] = { "if", "then", "else", "elif", "fi", "for", "while", "do", "done", "case", "esac",
    "function", "return", "in", "export", "local", "echo", "set", "unset", "source", "exit", "FROM", "RUN", "CMD",
    "COPY", "ADD", "WORKDIR", "ENV", "EXPOSE", "ENTRYPOINT", "ARG", "USER", "VOLUME", "my", "sub", "use", "print",
    "true", "false", "PHONY", "all", "clean", "ifeq", "endif", 0 };
static const char *const KW_LUA[] = { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if",
    "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while", "module", "where", "let",
    "data", "import", "instance", "deriving", "otherwise", "case", "of", 0 };
static const char *const KW_SQL[] = { "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE",
    "CREATE", "TABLE", "DROP", "ALTER", "JOIN", "LEFT", "RIGHT", "INNER", "OUTER", "ON", "AS", "AND", "OR", "NOT",
    "NULL", "PRIMARY", "KEY", "ORDER", "BY", "GROUP", "HAVING", "LIMIT", "INTEGER", "TEXT", "VARCHAR", "REFERENCES",
    "DEFAULT", "UNIQUE", "IF", "EXISTS", "INDEX", "COUNT", "DISTINCT", 0 };

static int word_in(const char *const *list, const char *w, int n, int ci)
{
    if (!list) return 0;
    for (int i = 0; list[i]; i++) {
        int k = 0;
        while (k < n && list[i][k] && (ci ? ((w[k] | 32) == (list[i][k] | 32)) : w[k] == list[i][k])) k++;
        if (k == n && !list[i][k]) return 1;
    }
    return 0;
}

static int is_id(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80; }

/* the comment state at the start of line `ln` (block comments span lines) */
static int block_state_at(const struct buffer *b, int ln)
{
    int hl = b->hl, in = 0;
    if (hl != HL_C && hl != HL_JS && hl != HL_RUST && hl != HL_GO && hl != HL_CSS && hl != HL_HTML) return 0;
    const char *open = hl == HL_HTML ? "<!--" : "/*", *close = hl == HL_HTML ? "-->" : "*/";
    int ol = hl == HL_HTML ? 4 : 2, cl = hl == HL_HTML ? 3 : 2;
    char q = 0;
    for (int i = 0; i < lines[ln]; i++) {
        char c = b->text[i];
        if (in) { if (!strncmp(b->text + i, close, (size_t)cl)) { in = 0; i += cl - 1; } continue; }
        if (q) { if (c == '\\') i++; else if (c == q || c == '\n') q = 0; continue; }
        if (hl != HL_HTML && (c == '"' || c == '\'')) { q = c; continue; }
        if (hl != HL_HTML && hl != HL_CSS && c == '/' && b->text[i + 1] == '/') { while (i < b->len && b->text[i] != '\n') i++; continue; }
        if (!strncmp(b->text + i, open, (size_t)ol)) { in = 1; i += ol - 1; }
    }
    return in;
}

struct tok { int start, len, kind; };

static int tokenize(const struct buffer *b, int ln, int *in_block, struct tok *out, int max)
{
    int s = lines[ln], e = line_end(b, ln), n = 0, hl = b->hl;
    const char *t = b->text;
    const char *const *kw = 0, *const *ty = 0;
    int ci = 0;
    const char *lc = 0;
    switch (hl) {
    case HL_C: kw = KW_C; ty = TY_C; lc = "//"; break;
    case HL_JS: kw = KW_JS; ty = TY_JS; lc = "//"; break;
    case HL_RUST: kw = KW_RS; ty = TY_RS; lc = "//"; break;
    case HL_GO: kw = KW_GO; ty = TY_GO; lc = "//"; break;
    case HL_PY: kw = KW_PY; lc = "#"; break;
    case HL_SH: kw = KW_SH; lc = "#"; break;
    case HL_LUA: case HL_HS: kw = KW_LUA; lc = "--"; break;
    case HL_SQL: kw = KW_SQL; lc = "--"; ci = 1; break;
    }
#define EMIT(st, ln_, k) do { if (n < max) out[n++] = (struct tok){ (st), (ln_), (k) }; } while (0)
    if (hl == HL_MD) {
        int k = t[s] == '#' ? T_HEADING : (t[s] == '>' ? T_COMMENT : ((t[s] == '-' || t[s] == '*') && t[s + 1] == ' ') ? T_KEYWORD : T_TEXT);
        if (k == T_KEYWORD) { EMIT(s, 1, k); s++; k = T_TEXT; }
        for (int i = s; i < e; ) {
            if (t[i] == '`') { int j = i + 1; while (j < e && t[j] != '`') j++; if (j < e) j++; EMIT(i, j - i, T_STRING); i = j; continue; }
            int j = i; while (j < e && t[j] != '`') j++;
            EMIT(i, j - i, k); i = j;
        }
        return n;
    }
    if (hl == HL_C && t[s] == '#') { EMIT(s, e - s, T_PREPROC); return n; }
    int i = s;
    while (i < e) {
        if (*in_block) {
            const char *close = hl == HL_HTML ? "-->" : "*/";
            int cl = (int)strlen(close), j = i;
            while (j < e && strncmp(t + j, close, (size_t)cl)) j++;
            if (j < e) { j += cl; *in_block = 0; }
            EMIT(i, j - i, T_COMMENT); i = j; continue;
        }
        char c = t[i];
        if (hl == HL_HTML) {
            if (!strncmp(t + i, "<!--", 4)) { *in_block = 1; continue; }
            if (c == '<') {
                int j = i + 1; if (j < e && t[j] == '/') j++;
                while (j < e && (is_id((unsigned char)t[j]) || t[j] == '-' || t[j] == '!')) j++;
                EMIT(i, j - i, T_KEYWORD); i = j;
                while (i < e && t[i] != '>') {
                    if (t[i] == '"' || t[i] == '\'') { char q = t[i]; int k = i + 1; while (k < e && t[k] != q) k++; if (k < e) k++; EMIT(i, k - i, T_STRING); i = k; continue; }
                    int k = i; while (k < e && t[k] != '>' && t[k] != '"' && t[k] != '\'' && t[k] != '=') k++;
                    if (k > i) EMIT(i, k - i, T_TYPE);
                    if (k < e && t[k] == '=') { EMIT(k, 1, T_OPERATOR); k++; }
                    i = k;
                }
                if (i < e) { EMIT(i, 1, T_KEYWORD); i++; }
                continue;
            }
            int j = i; while (j < e && t[j] != '<') j++;
            EMIT(i, j - i, T_TEXT); i = j; continue;
        }
        if ((hl == HL_C || hl == HL_JS || hl == HL_RUST || hl == HL_GO || hl == HL_CSS) && c == '/' && i + 1 < e && t[i + 1] == '*') { *in_block = 1; continue; }
        if (lc && !strncmp(t + i, lc, strlen(lc))) { EMIT(i, e - i, T_COMMENT); break; }
        if (c == '"' || c == '\'' || (c == '`' && (hl == HL_JS || hl == HL_GO))) {
            int j = i + 1;
            while (j < e && t[j] != c) { if (t[j] == '\\') j++; j++; }
            if (j < e) j++;
            if (j > e) j = e;
            EMIT(i, j - i, T_STRING); i = j; continue;
        }
        if (c >= '0' && c <= '9') { int j = i; while (j < e && (is_id((unsigned char)t[j]) || t[j] == '.')) j++; EMIT(i, j - i, T_NUMBER); i = j; continue; }
        if (is_id((unsigned char)c)) {
            int j = i; while (j < e && is_id((unsigned char)t[j])) j++;
            int k = T_TEXT;
            if (hl == HL_CSS) { int q = j; while (q < e && t[q] == ' ') q++; k = (q < e && t[q] == ':') ? T_TYPE : T_TEXT; }
            else if (word_in(kw, t + i, j - i, ci)) k = T_KEYWORD;
            else if (word_in(ty, t + i, j - i, 0) || (hl == HL_C && j - i > 2 && t[j - 2] == '_' && t[j - 1] == 't')) k = T_TYPE;
            else { int q = j; while (q < e && t[q] == ' ') q++; if (q < e && t[q] == '(') k = T_FUNCTION; }
            EMIT(i, j - i, k); i = j; continue;
        }
        int j = i + 1;
        while (j < e && !is_id((unsigned char)t[j]) && t[j] != '"' && t[j] != '\'' && t[j] != '/' && t[j] != '#' && t[j] != '-' && t[j] != '`') j++;
        EMIT(i, j - i, strchr("{}()[];,.", c) ? T_TEXT : T_OPERATOR); i = j;
    }
    return n;
#undef EMIT
}

static uint32_t tok_color(int k)
{
    const struct aether_theme *t = TH();
    switch (k) {
    case T_KEYWORD: return t->keyword; case T_TYPE: return t->type; case T_STRING: return t->string;
    case T_COMMENT: return t->comment; case T_NUMBER: return t->number; case T_FUNCTION: return t->function;
    case T_PREPROC: return t->preproc; case T_OPERATOR: return t->operator; case T_HEADING: return t->heading;
    default: return t->fg;
    }
}

/* ── painting ───────────────────────────────────────────────────────── */
static void logo(struct canvas *c, int x, int y, uint32_t col)
{
    /* Aether's mark: an open triangle with a spark */
    gfx_line(c, x, y + 18, x + 9, y + 2, 2.2f, col);
    gfx_line(c, x + 9, y + 2, x + 18, y + 18, 2.2f, col);
    gfx_line(c, x + 5, y + 12, x + 13, y + 12, 2.0f, col);
    gfx_circle(c, x + 18, y + 3, 2.2f, col);
}

static void button(struct canvas *c, int x, int y, int w, const char *label, uint32_t bg, uint32_t fg)
{
    gfx_round(c, x, y, w, 26, 6, bg, 0);
    gfx_text(c, &font_ui, x + (w - gfx_text_width(&font_ui, label)) / 2, y + 5, label, fg);
}

static const char *const TOOLS[] = { "New", "Save", "Preview", "Find", "Theme" };
static int tool_x(int i) { return 150 + i * 78; }

static void paint_overlay_new(struct canvas *c, int X, int Y, int W, int H, const struct aether_theme *t)
{
    int ow = W - 80 > 760 ? 760 : W - 80, cols = ow / 180, rows = (AETHER_LANG_COUNT + cols - 1) / cols;
    int oh = 70 + rows * 34;
    if (oh > H - 40) oh = H - 40;
    int ox = X + (W - ow) / 2, oy = Y + 30;
    gfx_shadow(c, ox, oy, ow, oh, 10, 18, 120);
    gfx_round(c, ox, oy, ow, oh, 10, t->line, 0);
    gfx_round_frame(c, ox, oy, ow, oh, 10, t->cursor);
    gfx_text(c, &font_bold, ox + 18, oy + 14, "New file from a template", t->fg);
    gfx_text(c, &font_ui, ox + 18, oy + 34, "Aether's starter main for each language. Esc to cancel.", t->gutter_fg);
    for (int i = 0; i < AETHER_LANG_COUNT; i++) {
        int bx = ox + 18 + (i % cols) * 180, by = oy + 60 + (i / cols) * 34;
        if (by + 28 > oy + oh) break;
        gfx_round(c, bx, by, 170, 28, 6, t->bg, 0);
        gfx_text(c, &font_bold, bx + 10, by + 6, aether_langs[i].name, t->fg);
        gfx_text_fit(c, &font_ui, bx + 92, by + 6, 74, aether_langs[i].file, t->gutter_fg);
    }
}

static void paint_overlay_theme(struct canvas *c, int X, int Y, int W, int H, const struct aether_theme *t)
{
    int ow = 520, cols = 2, rows = (AETHER_THEME_COUNT + 1) / 2, oh = 56 + rows * 26;
    if (oh > H - 20) oh = H - 20;
    int ox = X + (W - ow) / 2, oy = Y + 10;
    gfx_shadow(c, ox, oy, ow, oh, 10, 18, 120);
    gfx_round(c, ox, oy, ow, oh, 10, t->line, 0);
    gfx_round_frame(c, ox, oy, ow, oh, 10, t->cursor);
    gfx_text(c, &font_bold, ox + 18, oy + 14, "Colour theme", t->fg);
    for (int i = 0; i < AETHER_THEME_COUNT; i++) {
        const struct aether_theme *o = &aether_themes[i];
        int bx = ox + 14 + (i % cols) * 250, by = oy + 44 + (i / cols) * 26;
        if (by + 24 > oy + oh) break;
        gfx_round(c, bx, by, 240, 24, 5, o->bg, 0);
        if (i == theme) gfx_round_frame(c, bx, by, 240, 24, 5, o->cursor);
        gfx_circle(c, bx + 14, by + 12, 5, o->keyword);
        gfx_circle(c, bx + 28, by + 12, 5, o->string);
        gfx_text(c, &font_ui, bx + 42, by + 4, o->name, o->fg);
    }
}

void aether_paint(const struct app_ctx *a)
{
    struct canvas *c = a->c;
    const struct aether_theme *t = TH();
    int X = a->x, Y = a->y, W = a->w, H = a->h;
    win_w = W; win_h = H;
    uint32_t side = gfx_mix(t->bg, t->fg, t->light ? 0.04f : 0.035f) | 0xff000000;
    uint32_t brand = t->cursor;

    /* header: logo, name, tool buttons */
    gfx_fill(c, X, Y, W, HEADER, side);
    logo(c, X + 14, Y + 10, brand);
    gfx_text(c, &font_bold, X + 42, Y + 12, "Aether", t->fg);
    for (int i = 0; i < 5; i++) {
        int on = (i == 2 && cur >= 0 && bufs[cur].hl == HL_HTML) || i != 2;
        button(c, X + tool_x(i), Y + 7, 70, TOOLS[i], i == 1 && cur >= 0 && bufs[cur].dirty ? brand : t->line,
               i == 1 && cur >= 0 && bufs[cur].dirty ? 0xffffffff : (on ? t->fg : t->gutter_fg));
    }
    gfx_text_fit(c, &font_ui, X + tool_x(5) + 6, Y + 12, W - tool_x(5) - 16, t->name, t->gutter_fg);
    gfx_fill(c, X, Y + HEADER - 1, W, 1, with_alpha(t->fg, 30));

    /* explorer */
    int sy = Y + HEADER, sh = H - HEADER - STATUS;
    gfx_fill(c, X, sy, SIDEBAR, sh, side);
    gfx_text(c, &font_bold, X + 14, sy + 10, "EXPLORER", t->gutter_fg);
    gfx_text(c, &font_ui, X + 14, sy + 30, "/home/root", t->gutter_fg);
    gfx_clip(c, X, sy + 50, SIDEBAR, sh - 50);
    int row = 0;
    for (int i = 0; i < vfs_count(); i++) {
        const char *p = vfs_path(i);
        int fy = sy + 52 + (row - explorer_scroll) * 24;
        row++;
        if (fy < sy + 40 || fy > sy + sh) continue;
        int open = cur >= 0 && !strcmp(bufs[cur].path, p);
        if (open) gfx_fill(c, X, fy - 2, SIDEBAR, 24, t->sel);
        const char *shown = !strncmp(p, "/home/root/", 11) ? p + 11 : p;
        int l = lang_for(p);
        gfx_circle(c, X + 20, fy + 9, 3.5f, l >= 0 ? tok_color(l % 2 ? T_KEYWORD : T_STRING) : t->gutter_fg);
        gfx_text_fit(c, &font_ui, X + 32, fy + 2, SIDEBAR - 42, shown, open ? t->fg : gfx_mix(t->fg, t->bg, 0.15f));
    }
    gfx_noclip(c);
    gfx_fill(c, X + SIDEBAR - 1, sy, 1, sh, with_alpha(t->fg, 25));

    int ex = X + SIDEBAR, ew = W - SIDEBAR;
    /* tabs */
    gfx_fill(c, ex, sy, ew, TABS, side);
    int tx = ex;
    for (int i = 0; i < MAXBUF; i++) {
        if (!bufs[i].used) continue;
        const char *nm = strrchr(bufs[i].path, '/');
        nm = nm ? nm + 1 : bufs[i].path;
        int tw = gfx_text_width(&font_ui, nm) + 44;
        if (tx + tw > ex + ew) break;
        if (i == cur) { gfx_fill(c, tx, sy, tw, TABS, t->bg); gfx_fill(c, tx, sy, tw, 2, brand); }
        gfx_text(c, &font_ui, tx + 12, sy + 7, nm, i == cur ? t->fg : t->gutter_fg);
        if (bufs[i].dirty) gfx_circle(c, tx + tw - 16, sy + 15, 3.5f, i == cur ? t->fg : t->gutter_fg);
        else gfx_text(c, &font_ui, tx + tw - 20, sy + 7, "×", t->gutter_fg);
        tx += tw;
    }

    /* editor */
    int ey = sy + TABS, eh = sh - TABS;
    gfx_fill(c, ex, ey, ew, eh, t->bg);
    if (cur < 0) {
        logo(c, ex + ew / 2 - 30, ey + eh / 2 - 110, brand);
        gfx_text(c, &font_head, ex + ew / 2 - 2, ey + eh / 2 - 112, "Aether", t->fg);
        const char *l1 = "A fast IDE, now on AuxoBSD.";
        gfx_text(c, &font_ui, ex + (ew - gfx_text_width(&font_ui, l1)) / 2, ey + eh / 2 - 70, l1, t->gutter_fg);
        button(c, ex + ew / 2 - 160, ey + eh / 2 - 36, 150, "New file…", brand, 0xffffffff);
        button(c, ex + ew / 2 + 10, ey + eh / 2 - 36, 150, "Open hello.c", t->line, t->fg);
        const char *l2 = "Ctrl+N new · Ctrl+S save · Ctrl+F find · Ctrl+Z undo · Ctrl+W close";
        gfx_text(c, &font_ui, ex + (ew - gfx_text_width(&font_ui, l2)) / 2, ey + eh / 2 + 10, l2, t->gutter_fg);
    } else {
        struct buffer *b = &bufs[cur];
        reindex(b);
        int gutter = 56;
        view_rows = eh / LINE_H - 1;
        view_cols = (ew - gutter - 16) / CHAR_W;
        int cl = line_of(b->caret), cc = display_col(b, cl, b->caret);
        if (cl < b->top) b->top = cl;
        if (cl >= b->top + view_rows) b->top = cl - view_rows + 1;
        if (b->top > nlines - 1) b->top = nlines - 1;
        if (b->top < 0) b->top = 0;
        if (cc < b->left) b->left = cc;
        if (cc >= b->left + view_cols) b->left = cc - view_cols + 1;
        int sa = -1, se = -1;
        if (has_sel(b)) { sa = b->anchor < b->caret ? b->anchor : b->caret; se = b->anchor < b->caret ? b->caret : b->anchor; }
        int inb = block_state_at(b, b->top);
        struct tok toks[256];
        gfx_fill(c, ex, ey, gutter, eh, t->gutter_bg);
        for (int r = 0; r <= view_rows && b->top + r < nlines; r++) {
            int ln = b->top + r, y = ey + 6 + r * LINE_H;
            if (ln == cl) gfx_fill(c, ex + gutter, y - 1, ew - gutter, LINE_H, t->line);
            char num[12];
            ksnprintf(num, sizeof num, "%d", ln + 1);
            gfx_text(c, &font_mono, ex + gutter - 10 - (int)strlen(num) * CHAR_W, y, num, ln == cl ? t->fg : t->gutter_fg);
            gfx_clip(c, ex + gutter + 6, ey, ew - gutter - 10, eh);
            int bx = ex + gutter + 8 - b->left * CHAR_W;
            /* selection */
            if (sa >= 0) {
                int ls = lines[ln], le = line_end(b, ln);
                int a1 = sa > ls ? sa : ls, a2 = se < le ? se : le;
                if (a1 < a2 || (se > le && sa <= le)) {
                    int c1 = display_col(b, ln, a1), c2 = display_col(b, ln, a2) + (se > le ? 1 : 0);
                    gfx_fill(c, bx + c1 * CHAR_W, y - 1, (c2 - c1) * CHAR_W, LINE_H, t->sel);
                }
            }
            int nt = tokenize(b, ln, &inb, toks, 256);
            for (int k = 0; k < nt; k++) {
                char tmp[512];
                int len = toks[k].len < 511 ? toks[k].len : 511;
                int col = display_col(b, ln, toks[k].start);
                /* expand tabs inside the token */
                int o = 0, cc2 = col;
                for (int q = 0; q < len && o < 500; q++) {
                    char ch = b->text[toks[k].start + q];
                    if (ch == '\t') { int nc = (cc2 / 4 + 1) * 4; while (cc2 < nc && o < 500) { tmp[o++] = ' '; cc2++; } }
                    else { tmp[o++] = ch; if (((unsigned char)ch & 0xC0) != 0x80) cc2++; }
                }
                tmp[o] = 0;
                if (bx + cc2 * CHAR_W < ex + gutter) continue;
                gfx_text(c, &font_mono, bx + col * CHAR_W, y, tmp, tok_color(toks[k].kind));
            }
            if (ln == cl && a->focused && !overlay && !(a->blink & 1)) gfx_fill(c, bx + cc * CHAR_W, y - 1, 2, LINE_H, t->cursor);
            gfx_noclip(c);
        }
    }

    /* status bar, Aether red */
    int st = Y + H - STATUS;
    gfx_fill(c, X, st, W, STATUS, brand);
    char s[200];
    if (message[0]) strlcpy(s, message, sizeof s);
    else if (cur >= 0) {
        struct buffer *b = &bufs[cur];
        int l = line_of(b->caret);
        ksnprintf(s, sizeof s, "%s   Ln %d, Col %d   %s   %s", b->lang >= 0 ? aether_langs[b->lang].name : "Plain text", l + 1,
                  display_col(b, l, b->caret) + 1, b->dirty ? "unsaved" : "saved", b->path);
    } else strlcpy(s, "Aether for AuxoBSD · github.com/rxvy-dev/aether", sizeof s);
    gfx_text_fit(c, &font_ui, X + 12, st + 4, W - 24, s, 0xffffffff);

    if (overlay == OV_FIND) {
        int fw = 360, fx = X + W - fw - 20, fy = Y + HEADER + TABS + 8;
        gfx_shadow(c, fx, fy, fw, 36, 8, 12, 100);
        gfx_round(c, fx, fy, fw, 36, 8, t->line, 0);
        gfx_round_frame(c, fx, fy, fw, 36, 8, brand);
        gfx_text(c, &font_ui, fx + 12, fy + 10, "Find:", t->gutter_fg);
        int tw = gfx_text(c, &font_mono, fx + 56, fy + 10, findbuf, t->fg);
        if (!(a->blink & 1)) gfx_fill(c, fx + 56 + tw, fy + 9, 2, 18, t->cursor);
    }
    if (overlay == OV_NEW) paint_overlay_new(c, X, Y, W, H, t);
    if (overlay == OV_THEME) paint_overlay_theme(c, X, Y, W, H, t);
}

/* ── input ──────────────────────────────────────────────────────────── */
static void find_next(void)
{
    if (cur < 0 || !findlen) return;
    struct buffer *b = &bufs[cur];
    for (int pass = 0; pass < 2; pass++)
        for (int i = pass ? 0 : b->caret + (has_sel(b) ? 0 : 0); i + findlen <= b->len; i++) {
            if (pass && i >= b->caret) break;
            int k = 0;
            while (k < findlen && ((b->text[i + k] | 32) == (findbuf[k] | 32))) k++;
            if (k == findlen && i != (b->anchor < b->caret ? b->anchor : b->caret)) {
                b->anchor = i; b->caret = i + findlen;
                message[0] = 0;
                return;
            }
        }
    ksnprintf(message, sizeof message, "\"%s\" not found", findbuf);
}

static void preview(void)
{
    if (cur < 0) return;
    struct buffer *b = &bufs[cur];
    if (b->hl != HL_HTML) { ksnprintf(message, sizeof message, "Preview is for HTML files (Aether's web preview)"); return; }
    save();
    char u[VFS_PATH + 8];
    ksnprintf(u, sizeof u, "file://%s", b->path);
    chrono_open_url(u);
}

int aether_click(int x, int y, int w, int h)
{
    (void)w; (void)h;
    message[0] = 0;
    if (overlay == OV_NEW) {
        int ow = win_w - 80 > 760 ? 760 : win_w - 80, cols = ow / 180, ox = (win_w - ow) / 2, oy = 30;
        int col = (x - ox - 18) / 180, row = (y - oy - 60) / 34;
        if (x >= ox + 18 && col < cols && y >= oy + 60 && (x - ox - 18) % 180 < 170) {
            int i = row * cols + col;
            if (i >= 0 && i < AETHER_LANG_COUNT) { new_from_template(i); overlay = 0; return 1; }
        }
        overlay = 0;
        return 1;
    }
    if (overlay == OV_THEME) {
        int ox = (win_w - 520) / 2, oy = 10;
        int col = (x - ox - 14) / 250, row = (y - oy - 44) / 26;
        if (x >= ox + 14 && col < 2 && y >= oy + 44) { int i = row * 2 + col; if (i >= 0 && i < AETHER_THEME_COUNT) theme = i; }
        overlay = 0;
        return 1;
    }
    if (y < HEADER) {
        for (int i = 0; i < 5; i++)
            if (x >= tool_x(i) && x < tool_x(i) + 70) {
                if (i == 0) overlay = OV_NEW;
                else if (i == 1) save();
                else if (i == 2) preview();
                else if (i == 3) { overlay = OV_FIND; }
                else overlay = OV_THEME;
                return 1;
            }
        return 0;
    }
    if (overlay == OV_FIND) overlay = 0;
    if (x < SIDEBAR) {
        int row = (y - HEADER - 52) / 24 + explorer_scroll;
        if (y >= HEADER + 50 && row >= 0 && row < vfs_count()) aether_open(vfs_path(row));
        return 1;
    }
    if (y < HEADER + TABS) {
        int tx = SIDEBAR;
        for (int i = 0; i < MAXBUF; i++) {
            if (!bufs[i].used) continue;
            const char *nm = strrchr(bufs[i].path, '/');
            nm = nm ? nm + 1 : bufs[i].path;
            int tw = gfx_text_width(&font_ui, nm) + 44;
            if (x >= tx && x < tx + tw) { if (x >= tx + tw - 24) close_tab(i); else cur = i; return 1; }
            tx += tw;
        }
        return 0;
    }
    if (cur < 0) {
        int ew = win_w - SIDEBAR, eh = win_h - HEADER - STATUS - TABS, by = HEADER + TABS + eh / 2 - 36, cx = SIDEBAR + ew / 2;
        if (y >= by && y < by + 26) {
            if (x >= cx - 160 && x < cx - 10) overlay = OV_NEW;
            else if (x >= cx + 10 && x < cx + 160) aether_open("/home/root/hello.c");
        }
        return 1;
    }
    struct buffer *b = &bufs[cur];
    reindex(b);
    int ln = b->top + (y - HEADER - TABS - 6) / LINE_H;
    if (ln < 0) ln = 0;
    if (ln >= nlines) ln = nlines - 1;
    int col = (x - SIDEBAR - 56 - 8 + CHAR_W / 2) / CHAR_W + b->left;
    if (col < 0) col = 0;
    int pos = pos_at_col(b, ln, col);
    if (input_shift()) { if (b->anchor < 0) b->anchor = b->caret; }
    else b->anchor = -1;
    b->caret = pos;
    return 1;
}

int aether_wheel(int dz)
{
    if (cur < 0) { explorer_scroll += dz; if (explorer_scroll < 0) explorer_scroll = 0; return 1; }
    struct buffer *b = &bufs[cur];
    reindex(b);
    b->top += dz * 3;
    if (b->top > nlines - 1) b->top = nlines - 1;
    if (b->top < 0) b->top = 0;
    /* keep the caret on screen so painting doesn't scroll back */
    int cl = line_of(b->caret);
    if (cl < b->top) b->caret = lines[b->top];
    if (cl >= b->top + view_rows) b->caret = lines[b->top + view_rows - 1 < nlines ? b->top + view_rows - 1 : nlines - 1];
    return 1;
}

static void move_caret(struct buffer *b, int pos, int select)
{
    if (select) { if (b->anchor < 0) b->anchor = b->caret; }
    else b->anchor = -1;
    if (pos < 0) pos = 0;
    if (pos > b->len) pos = b->len;
    b->caret = pos;
}

int aether_key(int k)
{
    if (k == 0x0E) { overlay = OV_NEW; return 1; }                 /* Ctrl+N */
    if (overlay == OV_NEW || overlay == OV_THEME) { if (k == 27) overlay = 0; return 1; }
    if (overlay == OV_FIND) {
        if (k == 27) { overlay = 0; return 1; }
        if (k == '\n' || k == 0x06) { find_next(); return 1; }
        if (k == '\b') { if (findlen) findbuf[--findlen] = 0; return 1; }
        if (k >= 32 && k < 127 && findlen + 1 < (int)sizeof findbuf) { findbuf[findlen++] = (char)k; findbuf[findlen] = 0; find_next(); }
        return 1;
    }
    if (cur < 0) { if (k == '\n') overlay = OV_NEW; return 1; }
    struct buffer *b = &bufs[cur];
    reindex(b);
    message[0] = 0;
    int sel = input_shift();
    int ln = line_of(b->caret);
    switch (k) {
    case 0x13: save(); return 1;                                   /* Ctrl+S */
    case 0x06: overlay = OV_FIND; return 1;                        /* Ctrl+F */
    case 0x17: close_tab(cur); return 1;                           /* Ctrl+W */
    case 0x1A: undo(b, 0); return 1;                               /* Ctrl+Z */
    case 0x19: undo(b, 1); return 1;                               /* Ctrl+Y */
    case 0x01: b->anchor = 0; b->caret = b->len; return 1;         /* Ctrl+A */
    case 0x10: preview(); return 1;                                /* Ctrl+P */
    case 0x03: case 0x18:                                          /* Ctrl+C, Ctrl+X */
        if (has_sel(b)) {
            int a1 = b->anchor < b->caret ? b->anchor : b->caret, a2 = b->anchor < b->caret ? b->caret : b->anchor;
            cliplen = a2 - a1 < (int)sizeof clip ? a2 - a1 : (int)sizeof clip;
            memcpy(clip, b->text + a1, (size_t)cliplen);
            if (k == 0x18) delete_sel(b);
        } else {                                                    /* whole line, like Aether */
            int a1 = lines[ln], a2 = ln + 1 < nlines ? lines[ln + 1] : b->len;
            cliplen = a2 - a1 < (int)sizeof clip ? a2 - a1 : (int)sizeof clip;
            memcpy(clip, b->text + a1, (size_t)cliplen);
            if (k == 0x18) delete_range(b, a1, a2);
        }
        return 1;
    case 0x16: delete_sel(b); insert(b, clip, cliplen); return 1; /* Ctrl+V */
    case 0x04: {                                                    /* Ctrl+D: duplicate line */
        int a1 = lines[ln], a2 = line_end(b, ln);
        char tmp[1024]; int n = a2 - a1 < 1000 ? a2 - a1 : 1000;
        tmp[0] = '\n'; memcpy(tmp + 1, b->text + a1, (size_t)n);
        int keep = b->caret; b->caret = a2; insert(b, tmp, n + 1); b->caret = keep + n + 1;
        return 1;
    }
    case KEY_LEFT: move_caret(b, has_sel(b) && !sel ? (b->anchor < b->caret ? b->anchor : b->caret) : b->caret - 1, sel); return 1;
    case KEY_RIGHT: move_caret(b, has_sel(b) && !sel ? (b->anchor > b->caret ? b->anchor : b->caret) : b->caret + 1, sel); return 1;
    case KEY_UP: case KEY_DOWN: case KEY_PGUP: case KEY_PGDN: {
        int d = k == KEY_UP ? -1 : k == KEY_DOWN ? 1 : k == KEY_PGUP ? -view_rows : view_rows;
        int nl = ln + d;
        if (nl < 0) { move_caret(b, 0, sel); return 1; }
        if (nl >= nlines) { move_caret(b, b->len, sel); return 1; }
        move_caret(b, pos_at_col(b, nl, display_col(b, ln, b->caret)), sel);
        return 1;
    }
    case KEY_HOME: {                                                 /* first non-blank, then column 0 */
        int p = lines[ln];
        while (p < b->len && (b->text[p] == ' ' || b->text[p] == '\t')) p++;
        move_caret(b, b->caret == p ? lines[ln] : p, sel);
        return 1;
    }
    case KEY_END: move_caret(b, line_end(b, ln), sel); return 1;
    case '\b':
        if (has_sel(b)) { delete_sel(b); return 1; }
        if (b->caret > 0) {
            int p = b->caret - 1;
            while (p > 0 && ((unsigned char)b->text[p] & 0xC0) == 0x80) p--;
            /* a pair the editor closed for you goes together */
            if (b->caret < b->len && strchr("([{\"'", b->text[p]) && b->text[b->caret] == ")]}\"'"[strchr("([{\"'", b->text[p]) - "([{\"'"])
                delete_range(b, p, b->caret + 1);
            else delete_range(b, p, b->caret);
        }
        return 1;
    case KEY_DEL:
        if (has_sel(b)) { delete_sel(b); return 1; }
        if (b->caret < b->len) { int q = b->caret + 1; while (q < b->len && ((unsigned char)b->text[q] & 0xC0) == 0x80) q++; delete_range(b, b->caret, q); }
        return 1;
    case '\n': {                                                     /* auto-indent */
        delete_sel(b);
        char ind[128]; int n = 0;
        ind[n++] = '\n';
        for (int p = lines[ln]; p < b->caret && n < 100 && (b->text[p] == ' ' || b->text[p] == '\t'); p++) ind[n++] = b->text[p];
        char prev = 0;
        for (int p = b->caret - 1; p >= lines[ln]; p--) if (b->text[p] != ' ' && b->text[p] != '\t') { prev = b->text[p]; break; }
        int opener = prev == '{' || prev == '[' || prev == '(' || (prev == ':' && b->hl == HL_PY);
        if (opener) ind[n++] = '\t';
        insert(b, ind, n);
        if (opener && b->caret < b->len && strchr("}])", b->text[b->caret])) {   /* {|} → {\n\t|\n} */
            int keep = b->caret;
            char close[128]; int m = 0;
            close[m++] = '\n';
            for (int q = 1; q < n - 1; q++) close[m++] = ind[q];
            insert(b, close, m);
            b->caret = keep;
        }
        return 1;
    }
    case KEY_SHIFT_TAB: {                                            /* outdent */
        int p = lines[ln];
        if (b->text[p] == '\t') { int keep = b->caret; b->caret = p; delete_range(b, p, p + 1); b->caret = keep - 1; }
        else { int q = p; while (q < p + 4 && b->text[q] == ' ') q++; if (q > p) { int keep = b->caret; delete_range(b, p, q); b->caret = keep - (q - p); } }
        return 1;
    }
    case 27: b->anchor = -1; return 1;
    }
    if (k == '\t' || (k >= 32 && k < 127)) {
        delete_sel(b);
        char ch = (char)k;
        static const char open[] = "([{\"'", close[] = ")]}\"'";
        const char *o = strchr(open, ch);
        /* typing a closer that's already there just steps over it */
        if (strchr(")]}\"'", ch) && b->caret < b->len && b->text[b->caret] == ch) { b->caret++; return 1; }
        if (o && ch) {
            int next_ok = b->caret >= b->len || strchr(" \t\n)]};,", b->text[b->caret]);
            int quote = ch == '"' || ch == '\'';
            int prev_word = b->caret > 0 && is_id((unsigned char)b->text[b->caret - 1]);
            if (next_ok && !(quote && prev_word)) {
                char pair[2] = { ch, close[o - open] };
                insert(b, pair, 2);
                b->caret--;
                return 1;
            }
        }
        insert(b, &ch, 1);
        return 1;
    }
    return 0;
}
