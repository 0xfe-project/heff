// The client's side of src/heff/rpc.hf, for the C backend: JSON-RPC over
// stdio, as the Language Server Protocol frames it -- each message a
// `Content-Length: n` header, a blank line, then n bytes of JSON; stdout
// carries only the messages, logs go to stderr -- and what of the host a
// language server asks for (the shape of a provider is said in
// lib/std/io.provider.c).

#include <dirent.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#define lstat stat
#endif

// A message's JSON, as src/heff/json.hf's `Json`
// ---------------------------------------------------------------------

typedef struct { const char *s, *end; } Json;

static void json_ws(Json *j) {
  while (j->s < j->end && (*j->s == ' ' || *j->s == '\t' || *j->s == '\n' || *j->s == '\r')) j->s++;
}

static int json_lit(Json *j, const char *w) {
  size_t n = strlen(w);
  if ((size_t)(j->end - j->s) < n || memcmp(j->s, w, n) != 0) return 0;
  j->s += n;
  return 1;
}

static void json_utf8(Buf *b, uint32_t u) {
  char c[4];
  size_t n;
  if (u < 0x80) { c[0] = (char)u; n = 1; }
  else if (u < 0x800) { c[0] = (char)(0xC0 | (u >> 6)); c[1] = (char)(0x80 | (u & 0x3F)); n = 2; }
  else if (u < 0x10000) { c[0] = (char)(0xE0 | (u >> 12)); c[1] = (char)(0x80 | ((u >> 6) & 0x3F)); c[2] = (char)(0x80 | (u & 0x3F)); n = 3; }
  else { c[0] = (char)(0xF0 | (u >> 18)); c[1] = (char)(0x80 | ((u >> 12) & 0x3F)); c[2] = (char)(0x80 | ((u >> 6) & 0x3F)); c[3] = (char)(0x80 | (u & 0x3F)); n = 4; }
  buf_put(b, c, n);
}

static int json_hex4(Json *j, uint32_t *u) {
  if (j->end - j->s < 4) return 0;
  *u = 0;
  for (int i = 0; i < 4; i++) {
    char c = *j->s++;
    *u <<= 4;
    if (c >= '0' && c <= '9') *u |= (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') *u |= (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') *u |= (uint32_t)(c - 'A' + 10);
    else return 0;
  }
  return 1;
}

// (a string's text, its escapes undone: 0 if it is not one)
static V json_string(Json *j) {
  if (j->s >= j->end || *j->s != '"') return 0;
  j->s++;
  Buf b = {0};
  for (;;) {
    if (j->s >= j->end) { free(b.s); return 0; }
    char c = *j->s++;
    if (c == '"') break;
    if (c != '\\') { buf_put(&b, &c, 1); continue; }
    if (j->s >= j->end) { free(b.s); return 0; }
    char e = *j->s++;
    uint32_t u;
    switch (e) {
    case '"': case '\\': case '/': buf_put(&b, &e, 1); break;
    case 'b': buf_put(&b, "\b", 1); break;
    case 'f': buf_put(&b, "\f", 1); break;
    case 'n': buf_put(&b, "\n", 1); break;
    case 'r': buf_put(&b, "\r", 1); break;
    case 't': buf_put(&b, "\t", 1); break;
    case 'u':
      if (!json_hex4(j, &u)) { free(b.s); return 0; }
      if (u >= 0xD800 && u < 0xDC00 && j->end - j->s >= 6 && j->s[0] == '\\' && j->s[1] == 'u') {
        Json save = *j;
        uint32_t lo;
        j->s += 2;
        if (json_hex4(j, &lo) && lo >= 0xDC00 && lo < 0xE000) u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        else *j = save;
      }
      json_utf8(&b, u);
      break;
    default: free(b.s); return 0;
    }
  }
  V s = heff_str_utf8((const unsigned char *)(b.s ? b.s : ""), (int64_t)b.n);
  free(b.s);
  return s;
}

static V json_value(Json *j);

// (a list of what `item` reads, between `open` and `close`)
static V json_items(Json *j, char close, V (*item)(Json *)) {
  j->s++;
  int64_t n = 0, cap = 8;
  V *xs = heff_alloc(cap * sizeof(V));
  json_ws(j);
  if (j->s < j->end && *j->s == close) j->s++;
  else
    for (;;) {
      V x = item(j);
      if (!x) goto bad;
      if (n == cap) {
        V *x2 = heff_alloc(cap * 2 * sizeof(V));
        memcpy(x2, xs, n * sizeof(V));
        free(xs);
        xs = x2;
        cap *= 2;
      }
      xs[n++] = x;
      json_ws(j);
      if (j->s < j->end && *j->s == ',') { j->s++; continue; }
      if (j->s < j->end && *j->s == close) { j->s++; break; }
      goto bad;
    }
  V l = NTAG(TAG_Nil);
  for (int64_t i = n - 1; i >= 0; i--) l = heff_cons(xs[i], l);
  free(xs);
  return l;
bad:
  for (int64_t i = 0; i < n; i++) heff_drop(xs[i]);
  free(xs);
  return 0;
}

static V json_member(Json *j) {
  json_ws(j);
  V k = json_string(j);
  if (!k) return 0;
  json_ws(j);
  if (j->s >= j->end || *j->s != ':') { heff_drop(k); return 0; }
  j->s++;
  V v = json_value(j);
  if (!v) { heff_drop(k); return 0; }
  V f[2] = { k, v };
  return heff_tag(TAG_MkPair, 2, f);
}

static V json_one(int tag, V x) { return x ? heff_tag(tag, 1, &x) : 0; }

// (a number: an integer an Int, as JavaScript's Number.isInteger says)
static V json_number(Json *j) {
  char *e;
  double d = strtod(j->s, &e);
  if (e == j->s || e > j->end) return 0;
  j->s = e;
  if (d == floor(d) && fabs(d) < 2305843009213693952.0) return json_one(TAG_JInt, MK_INT((int64_t)d));
  return json_one(TAG_JFloat, heff_flt(d));
}

static V json_value(Json *j) {
  json_ws(j);
  if (j->s >= j->end) return 0;
  switch (*j->s) {
  case 'n': return json_lit(j, "null") ? NTAG(TAG_JNull) : 0;
  case 't': return json_lit(j, "true") ? json_one(TAG_JBool, NTAG(TAG_True)) : 0;
  case 'f': return json_lit(j, "false") ? json_one(TAG_JBool, NTAG(TAG_False)) : 0;
  case '"': return json_one(TAG_JString, json_string(j));
  case '[': return json_one(TAG_JArray, json_items(j, ']', json_value));
  case '{': return json_one(TAG_JObject, json_items(j, '}', json_member));
  default: return json_number(j);
  }
}

// (a body as JSON: 0 if it is not)
static V json_parse(const char *s, size_t n) {
  Json j = { s, s + n };
  V v = json_value(&j);
  json_ws(&j);
  if (v && j.s != j.end) { heff_drop(v); return 0; }
  return v;
}

// The messages, read from stdin
// ---------------------------------------------------------------------

static Buf rpc_in;
static int rpc_closed;

// (the next message's body, if a whole one is read: malloc'd)
static char *rpc_take(size_t *len) {
  for (;;) {
    char *sep = NULL;
    for (size_t i = 0; i + 4 <= rpc_in.n; i++)
      if (memcmp(rpc_in.s + i, "\r\n\r\n", 4) == 0) { sep = rpc_in.s + i; break; }
    if (!sep) return NULL;
    size_t head = (size_t)(sep - rpc_in.s), start = head + 4;
    long n = -1;
    for (size_t i = 0; i + 15 <= head; i++)
      if (strncasecmp(rpc_in.s + i, "Content-Length:", 15) == 0) {
        size_t k = i + 15;
        while (k < head && (rpc_in.s[k] == ' ' || rpc_in.s[k] == '\t')) k++;
        if (k < head && rpc_in.s[k] >= '0' && rpc_in.s[k] <= '9') {
          n = 0;
          while (k < head && rpc_in.s[k] >= '0' && rpc_in.s[k] <= '9') n = n * 10 + (rpc_in.s[k++] - '0');
        }
        break;
      }
    if (n < 0) {
      memmove(rpc_in.s, rpc_in.s + start, rpc_in.n - start);
      rpc_in.n -= start;
      continue;
    }
    if (rpc_in.n < start + (size_t)n) return NULL;
    char *body = heff_alloc((size_t)n + 1);
    memcpy(body, rpc_in.s + start, (size_t)n);
    body[n] = 0;
    memmove(rpc_in.s, rpc_in.s + start + n, rpc_in.n - start - n);
    rpc_in.n -= start + n;
    *len = (size_t)n;
    return body;
  }
}

static int64_t rpc_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// (whether stdin has something to read within `wait` ms (none, below
// 0): 1 if so, 0 if not, below 0 if waiting failed)
static int rpc_ready(int wait) {
#ifdef _WIN32
  // (a pipe, as an editor gives it, is looked into until it has bytes;
  // what is not a pipe is read, waiting there)
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  int64_t until = rpc_now() + wait;
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) || avail > 0) return 1;
    if (wait >= 0 && rpc_now() >= until) return 0;
    Sleep(5);
  }
#else
  struct pollfd fd = { 0, POLLIN, 0 };
  return poll(&fd, 1, wait);
#endif
}

// (the next event: a message, the time waited for, `ms`, passing first
// (none, below 0), or the client gone)
static V rpc_await(int64_t ms) {
  int64_t until = rpc_now() + ms;
  for (;;) {
    size_t n;
    char *body = rpc_take(&n);
    if (body) {
      V v = json_parse(body, n);
      free(body);
      if (v) return heff_tag(TAG_Message, 1, &v);
      fputs("heff lsp: not JSON\n", stderr);
      continue;
    }
    if (rpc_closed) return NTAG(TAG_Closed);
    int wait = -1;
    if (ms >= 0) {
      int64_t left = until - rpc_now();
      if (left <= 0) return NTAG(TAG_Timeout);
      wait = left > 0x7fffffff ? 0x7fffffff : (int)left;
    }
    int r = rpc_ready(wait);
    if (r == 0) return NTAG(TAG_Timeout);
    if (r < 0) continue;
    char chunk[65536];
    ssize_t got = read(0, chunk, sizeof chunk);
    if (got <= 0) rpc_closed = 1;
    else buf_put(&rpc_in, chunk, (size_t)got);
  }
}

// Paths and URIs
// ---------------------------------------------------------------------

// (a path made absolute against the working directory, its `.`, `..`
// and repeated separators resolved, as Node.js's path.resolve: malloc'd;
// on Windows a path starts with its drive, `C:`, and its separator is
// `\\`, though `/` is read as one too)
#ifdef _WIN32
#define RPC_SEP '\\'
#define rpc_is_sep(c) ((c) == '/' || (c) == '\\')
#else
#define RPC_SEP '/'
#define rpc_is_sep(c) ((c) == '/')
#endif

static char *rpc_resolve(const char *p) {
  Buf b = {0};
  char cwd[4096];
  if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
#ifdef _WIN32
  int drive = p[0] && p[1] == ':';
  if (drive && rpc_is_sep(p[2])) buf_str(&b, p);
  else if (rpc_is_sep(p[0])) {
    if (cwd[0] && cwd[1] == ':') buf_put(&b, cwd, 2);
    buf_str(&b, p);
  } else {
    buf_str(&b, cwd);
    buf_put(&b, "\\", 1);
    buf_str(&b, drive ? p + 2 : p);
  }
#else
  if (p[0] != '/') {
    buf_str(&b, cwd);
    buf_put(&b, "/", 1);
  }
  buf_str(&b, p);
#endif
  buf_put(&b, "", 1);
  char *out = heff_alloc(b.n + 2);
  size_t o = 0, top = 0;
  const char *s = b.s;
#ifdef _WIN32
  if (s[0] && s[1] == ':') {
    out[o++] = s[0];
    out[o++] = ':';
    s += 2;
    top = 2;
  }
#endif
  while (*s) {
    while (rpc_is_sep(*s)) s++;
    const char *e = s;
    while (*e && !rpc_is_sep(*e)) e++;
    size_t n = (size_t)(e - s);
    if (n == 0 || (n == 1 && s[0] == '.')) {}
    else if (n == 2 && s[0] == '.' && s[1] == '.') {
      while (o > top && out[o - 1] != RPC_SEP) o--;
      if (o > top) o--;
    } else {
      out[o++] = RPC_SEP;
      memcpy(out + o, s, n);
      o += n;
    }
    s = e;
  }
  if (o == top) out[o++] = RPC_SEP;
  out[o] = 0;
  free(b.s);
  return out;
}

static int rpc_hex(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// (a file: URI's path, as Node.js's fileURLToPath: none if it has a host)
static V rpc_uri_path(const char *u) {
  if (strncmp(u, "file:", 5) != 0) return heff_nothing;
  u += 5;
  if (strncmp(u, "//", 2) == 0) {
    u += 2;
    const char *h = u;
    while (*u && *u != '/') u++;
    size_t hn = (size_t)(u - h);
    if (hn != 0 && !(hn == 9 && strncasecmp(h, "localhost", 9) == 0)) return heff_nothing;
  }
  Buf b = {0};
  for (; *u && *u != '?' && *u != '#'; u++) {
    int a, c;
    if (*u == '%' && (a = rpc_hex(u[1])) >= 0 && (c = rpc_hex(u[2])) >= 0) {
      char x = (char)(a * 16 + c);
      if (x == '/' || (RPC_SEP == '\\' && x == '\\')) { free(b.s); return heff_nothing; }
      buf_put(&b, &x, 1);
      u += 2;
    } else buf_put(&b, u, 1);
  }
  if (b.n == 0) buf_put(&b, "/", 1);
#ifdef _WIN32
  // (`/C:/a/b` is `C:\\a\\b`; a path without its drive is no path here)
  buf_put(&b, "", 1);
  if (!(b.n >= 4 && b.s[0] == '/' && b.s[2] == ':' && (b.s[3] == '/' || b.s[3] == 0))) { free(b.s); return heff_nothing; }
  b.n -= 2;
  memmove(b.s, b.s + 1, b.n);
  for (size_t i = 0; i < b.n; i++)
    if (b.s[i] == '/') b.s[i] = '\\';
  if (b.n == 2) buf_put(&b, "\\", 1);
#endif
  V s = heff_str_utf8((const unsigned char *)b.s, (int64_t)b.n);
  free(b.s);
  return heff_just(s);
}

// (a path as a file: URI, as Node.js's pathToFileURL(...).href)
static V rpc_path_uri(const char *p) {
  char *abs = rpc_resolve(p);
  Buf b = {0};
#ifdef _WIN32
  // (`C:\\a\\b` is `file:///C:/a/b`)
  for (char *c = abs; *c; c++)
    if (*c == '\\') *c = '/';
  buf_str(&b, "file:///");
#else
  buf_str(&b, "file://");
#endif
  for (const unsigned char *c = (const unsigned char *)abs; *c; c++) {
    if (*c <= 0x20 || *c >= 0x7F || strchr("\"#<>?`{}%\\", *c)) {
      char e[4];
      snprintf(e, sizeof e, "%%%02X", *c);
      buf_put(&b, e, 3);
    } else buf_put(&b, (const char *)c, 1);
  }
  free(abs);
  V s = heff_str_utf8((const unsigned char *)b.s, (int64_t)b.n);
  free(b.s);
  return s;
}

// The project's files
// ---------------------------------------------------------------------

static int rpc_is_root(const char *d) {
  char *p = heff_join(d, "package.hf");
  struct stat st;
  int ok = stat(p, &st) == 0 && S_ISREG(st.st_mode) && access(p, R_OK) == 0;
  free(p);
  return ok;
}

typedef struct { char **xs; int64_t n, cap; } Names;

static void names_add(Names *ns, char *x) {
  if (ns->n == ns->cap) {
    int64_t cap = ns->cap ? ns->cap * 2 : 16;
    char **x2 = heff_alloc(cap * sizeof(char *));
    if (ns->n) memcpy(x2, ns->xs, ns->n * sizeof(char *));
    free(ns->xs);
    ns->xs = x2;
    ns->cap = cap;
  }
  ns->xs[ns->n++] = x;
}

static int names_has(Names *ns, const char *x) {
  for (int64_t i = 0; i < ns->n; i++)
    if (strcmp(ns->xs[i], x) == 0) return 1;
  return 0;
}

static void names_free(Names *ns) {
  for (int64_t i = 0; i < ns->n; i++) free(ns->xs[i]);
  free(ns->xs);
}

static void rpc_walk(const char *d, Names *ignored, Names *files) {
  DIR *dir = opendir(d);
  if (!dir) return;
  Names here = {0};
  struct dirent *e;
  while ((e = readdir(dir))) names_add(&here, strdup(e->d_name));
  closedir(dir);
  qsort(here.xs, here.n, sizeof(char *), heff_cmp_cstr);
  for (int64_t i = 0; i < here.n; i++) {
    const char *name = here.xs[i];
    if (name[0] == '.' || names_has(ignored, name)) continue;
    char *p = heff_join(d, name);
    struct stat st;
    if (lstat(p, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        if (!rpc_is_root(p)) rpc_walk(p, ignored, files);
      } else if (S_ISREG(st.st_mode)) {
        size_t n = strlen(name);
        if (n >= 3 && strcmp(name + n - 3, ".hf") == 0) { names_add(files, p); continue; }
      }
    }
    free(p);
  }
  names_free(&here);
}

// (the `.hf` files of the project at `root` (a directory with a
// package.hf): every directory but another project's, a hidden one, or
// one the root's .gitignore names)
static V rpc_project_files(const char *root) {
  Names files = {0}, ignored = {0};
  if (rpc_is_root(root)) {
    names_add(&ignored, strdup("node_modules"));
    char *gi = heff_join(root, ".gitignore");
    FILE *f = fopen(gi, "r");
    free(gi);
    if (f) {
      char line[4096];
      while (fgets(line, sizeof line, f)) {
        char *s = line, *e = line + strlen(line);
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
        if (s < e && *s == '/') s++;
        if (e > s && e[-1] == '/') e--;
        *e = 0;
        if (!*s || *s == '#' || strpbrk(s, "*?[/!")) continue;
        names_add(&ignored, strdup(s));
      }
      fclose(f);
    }
    rpc_walk(root, &ignored, &files);
  }
  V l = heff_strs(files.xs, files.n);
  names_free(&files);
  names_free(&ignored);
  return l;
}

// The provider
// ---------------------------------------------------------------------

static V heff_provider_rpc(V p, V k) {
  V x;
  switch (heff_tag_of(p)) {
  case TAG_Await: {
    int64_t ms = INT_OF(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    x = heff_answer(f, rpc_await(ms));
    break;
  }
  case TAG_Send: {
    char *body = heff_cstr(heff_field(p, 0));
    x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    size_t n = strlen(body);
    fprintf(stdout, "Content-Length: %zu\r\n\r\n", n);
    fwrite(body, 1, n, stdout);
    fflush(stdout);
    free(body);
    break;
  }
  case TAG_Log: {
    fputs("heff lsp: ", stderr);
    heff_put(stderr, heff_field(p, 0));
    fputc('\n', stderr);
    x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    break;
  }
  case TAG_Now: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    x = heff_answer(f, MK_INT(rpc_now()));
    break;
  }
  case TAG_ProjectFiles: case TAG_UriPath: case TAG_PathUri: case TAG_AbsPath: {
    int tag = heff_tag_of(p);
    char *s = heff_cstr(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V r;
    if (tag == TAG_ProjectFiles) r = rpc_project_files(s);
    else if (tag == TAG_UriPath) r = rpc_uri_path(s);
    else if (tag == TAG_PathUri) r = rpc_path_uri(s);
    else {
      char *a = rpc_resolve(s);
      r = heff_str_c(a);
      free(a);
    }
    free(s);
    x = heff_answer(f, r);
    break;
  }
  case TAG_Quit: {
    int code = (int)INT_OF(heff_field(p, 0));
    fflush(stdout);
    fflush(stderr);
    exit(code);
  }
  default:
    heff_fail("not an operation of rpc");
    return 0;
  }
  return heff_tail(k, 1, &x);
}
