// The HTTP client's side of lib/std/http.hf, for the C backend: an
// effect provider (the shape of one is said in lib/std/io.provider.c).
// libcurl sends a request, in the process, its connection kept for the
// next -- or, where there is no libcurl, `curl` (on the PATH of every
// system heff runs on), spawned for each, given what libcurl is given,
// so that the two answer the same: a response, whatever its status, or
// curl's code and what it says, as its tool says it.
//
// What a request is to curl, either way: a `HEAD` no body (`-I`); a
// body sent with a `POST`, and with any other method where it is not
// empty (`--data-binary`), with no `Content-Type` of curl's own (a
// header `Content-Type:` takes it away) unless the request has one; any
// method but a `POST`, and a `GET` with no body, named (`-X`); no
// `Expect: 100-continue` (`Expect:`), unless the request has one; a
// header whose value is empty sent so (`Name;`, where `Name:` would take
// it away); redirects followed, fifty at most, where it says so (`-L`);
// its time in all bounded where it says so (`--max-time`).  The
// response's headers are those of the last response, as written before
// its body (`-D`): a status line starts the headers again.

// (an operation's strings that are not a C string: as curl calls them)
#define HTTP_BAD_ARGUMENT 43
#define HTTP_FAILED_INIT 2

// (a request, as both ways have it)
typedef struct {
  char *url;
  char *method;
  char **hs;          // (its headers, `Name: value`, and curl's taken away)
  int nh;
  const uint8_t *body;
  int64_t nbody;
  int64_t timeout;    // (milliseconds; 0, none)
  int follow;
  int head;           // (a `HEAD`)
  int send_body;      // (`--data-binary`)
  int custom;         // (`-X`)
} HttpReq;

// (what came back: a status and its headers' text and body, or curl's
// code and what it says)
typedef struct {
  int ok;
  int64_t code;
  Buf head;
  Buf body;
  Buf why;
} HttpGot;

// (whether a header of that name, its case not minded, is among `hs`)
static int http_has(char **hs, int nh, const char *name) {
  size_t n = strlen(name);
  for (int i = 0; i < nh; i++) {
    size_t k = 0;
    while (k < n && hs[i][k] && (hs[i][k] | 0x20) == (name[k] | 0x20)) k++;
    if (k == n && (hs[i][n] == ':' || hs[i][n] == ';')) return 1;
  }
  return 0;
}

// (a header as curl takes it: `Name: value`, or `Name;` where the value
// is empty; NULL where either has a NUL in it)
static char *http_header_line(V name, V value) {
  char *n = heff_cstr(name), *v = heff_cstr(value);
  char *l = NULL;
  if (n && v) {
    size_t a = strlen(n), b = strlen(v);
    l = heff_alloc(a + b + 3);
    if (b) snprintf(l, a + b + 3, "%s: %s", n, v);
    else snprintf(l, a + 2, "%s;", n);
  }
  free(n);
  free(v);
  return l;
}

// (a C string's copy, malloc'd)
static char *http_dup(const char *s) {
  size_t n = strlen(s) + 1;
  char *c = heff_alloc(n);
  memcpy(c, s, n);
  return c;
}

static void http_req_free(HttpReq *q) {
  free(q->url);
  free(q->method);
  for (int i = 0; i < q->nh; i++) free(q->hs[i]);
  free(q->hs);
}

// (the request `rq`, a `Request` (its fields in its type's order: url,
// method, headers, body, timeout, follow), as curl is to be given it,
// its body where `rq` keeps it, so `rq` held while it is used: 0 where
// a string of it has a NUL in it)
static int http_req(V rq, HttpReq *q) {
  memset(q, 0, sizeof *q);
  V url = heff_rget_v(rq, 0), method = heff_rget_v(rq, 1), hl = heff_rget_v(rq, 2);
  V body = heff_rget_v(rq, 3), timeout = heff_rget_v(rq, 4), follow = heff_rget_v(rq, 5);
  int ok = 1;
  q->url = heff_cstr_text(url);
  q->method = heff_cstr(method);
  if (!q->url || !q->method) ok = 0;
  int n = 0;
  for (V c = hl; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) n++;
  q->hs = heff_alloc(sizeof(char *) * (size_t)(n + 2));
  for (V c = hl; ok && heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) {
    V pair = heff_field(c, 0);
    char *l = http_header_line(heff_field(pair, 0), heff_field(pair, 1));
    if (!l) ok = 0;
    else q->hs[q->nh++] = l;
  }
  q->body = heff_bytes_data(body, &q->nbody);
  q->timeout = IS_INT(timeout) && INT_OF(timeout) > 0 ? INT_OF(timeout) : 0;
  q->follow = follow == NTAG(TAG_True);
  if (ok) {
    q->head = strcmp(q->method, "HEAD") == 0;
    int post = strcmp(q->method, "POST") == 0;
    q->send_body = !q->head && (post || q->nbody > 0);
    q->custom = !q->head && !post && !(strcmp(q->method, "GET") == 0 && !q->send_body);
    // (curl's own headers that the request has not, taken away)
    int user = q->nh;
    if (q->send_body && !http_has(q->hs, user, "Content-Type")) q->hs[q->nh++] = http_dup("Content-Type:");
    if (!http_has(q->hs, user, "Expect")) q->hs[q->nh++] = http_dup("Expect:");
  }
  heff_drop(url);
  heff_drop(method);
  heff_drop(hl);
  // (the body's bytes are `rq`'s, held by the caller)
  heff_drop(body);
  heff_drop(timeout);
  heff_drop(follow);
  return ok;
}

// (headers' text as written before a body: those after the last status
// line, as a list of names and values, and that line's status)
static V http_headers(const char *s, size_t n, int64_t *status) {
  size_t start = 0;
  *status = 0;
  for (size_t i = 0; i < n;) {
    size_t e = i;
    while (e < n && s[e] != '\n') e++;
    if (e - i >= 5 && memcmp(s + i, "HTTP/", 5) == 0) {
      start = e < n ? e + 1 : e;
      const char *sp = memchr(s + i, ' ', e - i);
      *status = sp ? strtol(sp + 1, NULL, 10) : 0;
    }
    i = e < n ? e + 1 : e;
  }
  // (each line's place, then the list made from the last of them)
  size_t cap = 16, m = 0;
  size_t *at = heff_alloc(sizeof(size_t) * cap * 4);
  for (size_t i = start; i < n;) {
    size_t e = i;
    while (e < n && s[e] != '\n') e++;
    size_t end = e;
    while (end > i && (s[end - 1] == '\r' || s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
    const char *colon = memchr(s + i, ':', end - i);
    if (colon) {
      size_t ne = (size_t)(colon - s), vs = ne + 1;
      while (ne > i && (s[ne - 1] == ' ' || s[ne - 1] == '\t')) ne--;
      while (vs < end && (s[vs] == ' ' || s[vs] == '\t')) vs++;
      if (m == cap) {
        size_t *more = heff_alloc(sizeof(size_t) * cap * 8);
        memcpy(more, at, sizeof(size_t) * cap * 4);
        free(at);
        at = more;
        cap *= 2;
      }
      at[m * 4] = i; at[m * 4 + 1] = ne; at[m * 4 + 2] = vs; at[m * 4 + 3] = end;
      m++;
    }
    i = e < n ? e + 1 : e;
  }
  V l = NTAG(TAG_Nil);
  for (size_t k = m; k-- > 0;) {
    V f[2] = {
      heff_str_bytes(s + at[k * 4], (int64_t)(at[k * 4 + 1] - at[k * 4])),
      heff_str_bytes(s + at[k * 4 + 2], (int64_t)(at[k * 4 + 3] - at[k * 4 + 2])),
    };
    l = heff_cons(heff_tag_n(TAG_MkPair, 2, f), l);
  }
  free(at);
  return l;
}

static uint32_t http_response_shape[3] = { LBL_status, LBL_headers, LBL_body };
static uint32_t http_error_shape[2] = { LBL_code, LBL_message };

// (an `Either HttpError Response` of what came back)
static V http_answer(HttpGot *g) {
  V r;
  if (g->ok) {
    int64_t status;
    V hs = http_headers(g->head.s ? g->head.s : "", g->head.n, &status);
    V f[3] = { heff_int(status), hs, heff_bytes_in(g->body.s ? g->body.s : "", (int64_t)g->body.n) };
    V x = heff_rec(3, http_response_shape, f);
    r = heff_tag_n(TAG_Right, 1, &x);
  } else {
    V f[2] = { heff_int(g->code), heff_str_text_n(g->why.s ? g->why.s : "", (int64_t)g->why.n) };
    V x = heff_rec(2, http_error_shape, f);
    r = heff_tag_n(TAG_Left, 1, &x);
  }
  free(g->head.s);
  free(g->body.s);
  free(g->why.s);
  return r;
}

// curl spawned
// ---------------------------------------------------------------------

// (a file of its own to write, its path in `path`: its descriptor, or -1)
static int http_temp(char *path, size_t room) {
#ifdef _WIN32
  char dir[MAX_PATH];
  if (!GetTempPathA(sizeof dir, dir) || room < MAX_PATH || !GetTempFileNameA(dir, "hfh", 0, path)) return -1;
  return _open(path, _O_WRONLY | _O_BINARY);
#else
  const char *dir = getenv("TMPDIR");
  if (!dir || !*dir) dir = "/tmp";
  snprintf(path, room, "%s/heff-http-XXXXXX", dir);
  return mkstemp(path);
#endif
}

#ifdef _WIN32
#define http_write _write
#define http_close _close
#else
#define http_write write
#define http_close close
#endif

// (the request, by a `curl` spawned)
static void http_spawned(HttpReq *q, HttpGot *g) {
  char headf[4096], bodyf[4096], secs[32];
  int hd = http_temp(headf, sizeof headf), bd = -1;
  if (hd >= 0) http_close(hd);
  int ok = hd >= 0;
  if (ok && q->send_body) {
    bd = http_temp(bodyf, sizeof bodyf);
    ok = bd >= 0;
    for (int64_t at = 0; ok && at < q->nbody;) {
      long w = (long)http_write(bd, q->body + at, (unsigned)(q->nbody - at > (1 << 30) ? 1 << 30 : q->nbody - at));
      if (w <= 0) ok = 0;
      else at += w;
    }
    if (bd >= 0) http_close(bd);
  }
  char **as = heff_alloc(sizeof(char *) * (size_t)(2 * q->nh + 24));
  int n = 0;
  char *bodyarg = NULL;
  as[n++] = "curl";
  as[n++] = "-sS";
  if (q->follow) { as[n++] = "-L"; as[n++] = "--max-redirs"; as[n++] = "50"; }
  if (q->head) as[n++] = "-I";
  if (q->custom) { as[n++] = "-X"; as[n++] = q->method; }
  for (int i = 0; i < q->nh; i++) { as[n++] = "-H"; as[n++] = q->hs[i]; }
  if (q->send_body) {
    bodyarg = heff_alloc(strlen(bodyf) + 2);
    snprintf(bodyarg, strlen(bodyf) + 2, "@%s", bodyf);
    as[n++] = "--data-binary";
    as[n++] = bodyarg;
  }
  if (q->timeout > 0) {
    snprintf(secs, sizeof secs, "%lld.%03lld", (long long)(q->timeout / 1000), (long long)(q->timeout % 1000));
    as[n++] = "--max-time";
    as[n++] = secs;
  }
  as[n++] = "-D";
  as[n++] = headf;
  as[n++] = "--";
  as[n++] = q->url;
  as[n] = NULL;
  Buf err = {0};
  int64_t status = ok ? heff_capture(as, &g->body, &err) : -1;
  if (status == 0) {
    g->ok = 1;
    if (q->head) g->body.n = 0;
    FILE *f = fopen(headf, "rb");
    if (f) {
      char chunk[65536];
      size_t got;
      while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) buf_put(&g->head, chunk, got);
      fclose(f);
    }
  } else {
    // (what curl said last: `curl: (CODE) why`)
    g->body.n = 0;
    const char *at = NULL;
    for (size_t i = 0; i + 7 <= err.n; i++)
      if ((i == 0 || err.s[i - 1] == '\n') && memcmp(err.s + i, "curl: (", 7) == 0) at = err.s + i;
    char *close_ = at ? memchr(at, ')', err.n - (size_t)(at - err.s)) : NULL;
    if (at && close_) {
      g->code = strtol(at + 7, NULL, 10);
      const char *w = close_ + 1;
      if (w < err.s + err.n && *w == ' ') w++;
      size_t len = (size_t)(err.s + err.n - w);
      while (len && (w[len - 1] == '\n' || w[len - 1] == '\r')) len--;
      buf_put(&g->why, w, len);
    } else {
      g->code = HTTP_FAILED_INIT;
      buf_str(&g->why, ok ? "curl could not be run (there is no libcurl either)" : "a file for curl could not be written");
    }
  }
  free(err.s);
  free(bodyarg);
  free(as);
  if (hd >= 0) remove(headf);
  if (bd >= 0) remove(bodyf);
}

// libcurl in the process
// ---------------------------------------------------------------------
//
// Found when the program first sends (dlopen), not linked: no header of
// it, no `-lcurl`, so a program builds as any other (`heff build` links
// `-lm` alone), and runs where there is no libcurl -- a static musl
// program, Windows -- by the spawned curl.  Whatever of it is not
// there, or fails before a request is made, the spawned curl does that
// request (and, libcurl not loaded, every one).  `-DHEFF_HTTP_SPAWN`:
// always the spawned curl (test/backends.mjs holds the two to each
// other).
//
// Each thread keeps an easy handle of its own, made at its first
// request, whose connections libcurl keeps open for the next: what makes
// a request some 40 microseconds where a spawned curl is some 6
// milliseconds.  Each request starts from the handle reset (its
// connections kept) and set up as the tool sets its own (its user
// agent, keep-alive, its buffer), and a failure is said as the tool
// says it.  Threads (`HEFF_THREADS`): libcurl made ready once
// (`pthread_once`), each thread's handle kept under a key, let go of
// when the thread ends; otherwise a static.  A handle a process was
// forked with is its parent's -- its connections the parent's sockets
// -- and is left as it is, never used nor cleaned up (which could write
// on them): the child makes its own.

#if !defined(_WIN32) && !defined(HEFF_HTTP_SPAWN)
#include <dlfcn.h>
#define HTTP_CURL 1
#endif

#ifdef HTTP_CURL
// (libcurl's ABI, as curl/curl.h has it: these numbers never change)
#define HTTP_GLOBAL_DEFAULT 3L
#define HTTP_OPT_NOBODY 44
#define HTTP_OPT_FOLLOWLOCATION 52
#define HTTP_OPT_MAXREDIRS 68
#define HTTP_OPT_BUFFERSIZE 98
#define HTTP_OPT_NOSIGNAL 99
#define HTTP_OPT_TIMEOUT_MS 155
#define HTTP_OPT_TCP_KEEPALIVE 213
#define HTTP_OPT_WRITEDATA 10001
#define HTTP_OPT_URL 10002
#define HTTP_OPT_ERRORBUFFER 10010
#define HTTP_OPT_POSTFIELDS 10015
#define HTTP_OPT_USERAGENT 10018
#define HTTP_OPT_HTTPHEADER 10023
#define HTTP_OPT_HEADERDATA 10029
#define HTTP_OPT_CUSTOMREQUEST 10036
#define HTTP_OPT_WRITEFUNCTION 20011
#define HTTP_OPT_HEADERFUNCTION 20079
#define HTTP_OPT_POSTFIELDSIZE_LARGE 30120
#define HTTP_INFO_CONTENT_LENGTH_DOWNLOAD_T (0x600000 + 15)
#define HTTP_ERROR_SIZE 256

// (a body said to be no longer than this is given room for all of it at
// once; a longer one grows as it comes, so that a length a server only
// says is not memory taken)
#define HTTP_ROOM_MAX ((int64_t)1 << 28)

static struct {
  int (*global_init)(long);
  void *(*easy_init)(void);
  void (*easy_cleanup)(void *);
  void (*easy_reset)(void *);
  int (*setopt)(void *, int, ...);
  int (*perform)(void *);
  const char *(*strerror)(int);
  void *(*slist_append)(void *, const char *);
  void (*slist_free_all)(void *);
  int (*getinfo)(void *, int, ...);   // (may be missing)
  const char *(*version)(void);       // (may be missing)
} http_lc;
static int http_lc_ok;
// (the user agent curl's tool says, `curl/VERSION`; "" where unknown)
static char http_agent[64];

// (a thread's handle, the process it was made in, and where libcurl says
// why a request failed)
typedef struct {
  void *h;
  pid_t pid;
  char err[HTTP_ERROR_SIZE];
} HttpEasy;

#ifdef HEFF_THREADS
static pthread_once_t http_once = PTHREAD_ONCE_INIT;
static pthread_key_t http_key;

static void http_let_go(void *p) {
  HttpEasy *e = p;
  if (e->h && e->pid == getpid()) http_lc.easy_cleanup(e->h);
  free(e);
}
#else
static HttpEasy http_easy;
#endif

// (the library found and its functions taken, once; http_lc_ok if all
// that is needed is there and it is ready)
static void http_load(void) {
  static const char *names[] = {
#ifdef __APPLE__
    "/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "libcurl.dylib",
#else
    "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl-nss.so.4", "libcurl.so",
#endif
    NULL
  };
  void *lib = NULL;
  for (int i = 0; names[i] && !lib; i++) lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
  if (!lib) return;
  // (a function pointer from dlsym's object pointer, as POSIX allows)
#define HTTP_SYM(f, name) (*(void **)&http_lc.f = dlsym(lib, name))
  HTTP_SYM(global_init, "curl_global_init");
  HTTP_SYM(easy_init, "curl_easy_init");
  HTTP_SYM(easy_cleanup, "curl_easy_cleanup");
  HTTP_SYM(easy_reset, "curl_easy_reset");
  HTTP_SYM(setopt, "curl_easy_setopt");
  HTTP_SYM(perform, "curl_easy_perform");
  HTTP_SYM(strerror, "curl_easy_strerror");
  HTTP_SYM(slist_append, "curl_slist_append");
  HTTP_SYM(slist_free_all, "curl_slist_free_all");
  HTTP_SYM(getinfo, "curl_easy_getinfo");
  HTTP_SYM(version, "curl_version");
#undef HTTP_SYM
  if (!http_lc.global_init || !http_lc.easy_init || !http_lc.easy_cleanup || !http_lc.easy_reset
      || !http_lc.setopt || !http_lc.perform || !http_lc.strerror || !http_lc.slist_append
      || !http_lc.slist_free_all)
    return;
#ifdef HEFF_THREADS
  if (pthread_key_create(&http_key, http_let_go) != 0) return;
#endif
  if (http_lc.global_init(HTTP_GLOBAL_DEFAULT) != 0) return;
  // ("libcurl/8.7.1 SecureTransport ...": its version, as the tool's is)
  const char *v = http_lc.version ? http_lc.version() : NULL;
  if (v && strncmp(v, "libcurl/", 8) == 0) {
    size_t n = strcspn(v + 8, " ");
    if (n && n < sizeof http_agent - 6) snprintf(http_agent, sizeof http_agent, "curl/%.*s", (int)n, v + 8);
  }
  http_lc_ok = 1;
}

// (this thread's handle -- or NULL, libcurl not to be used)
static HttpEasy *http_mine(void) {
#ifdef HEFF_THREADS
  pthread_once(&http_once, http_load);
  if (!http_lc_ok) return NULL;
  HttpEasy *e = pthread_getspecific(http_key);
  if (!e) {
    e = calloc(1, sizeof *e);
    if (!e) return NULL;
    if (pthread_setspecific(http_key, e) != 0) { free(e); return NULL; }
  }
#else
  static int loaded;
  if (!loaded) { loaded = 1; http_load(); }
  if (!http_lc_ok) return NULL;
  HttpEasy *e = &http_easy;
#endif
  pid_t me = getpid();
  if (e->h && e->pid == me) return e;
  // (none yet, or the parent's: one of this process's own)
  e->h = http_lc.easy_init();
  if (!e->h) return NULL;
  e->pid = me;
  return e;
}

// (where a body is gathered: `b`, given room for all of it at its first
// part where its length is said)
typedef struct {
  Buf *b;
  void *h;
  int sized;
} HttpSink;

static size_t http_sink(char *p, size_t size, size_t n, void *u) {
  HttpSink *s = u;
  size_t len = size * n;
  if (!s->sized) {
    s->sized = 1;
    int64_t whole = -1;
    Buf *b = s->b;
    if (http_lc.getinfo && http_lc.getinfo(s->h, HTTP_INFO_CONTENT_LENGTH_DOWNLOAD_T, &whole) == 0
        && whole > 0 && whole <= HTTP_ROOM_MAX && b->n + (size_t)whole + 1 > b->cap) {
      size_t cap = b->n + (size_t)whole + 1;
      char *room = heff_alloc(cap);
      if (b->n) memcpy(room, b->s, b->n);
      room[b->n] = 0;
      free(b->s);
      b->s = room;
      b->cap = cap;
    }
  }
  buf_put(s->b, p, len);
  return len;
}

// (a header line, as written before a body)
static size_t http_head_sink(char *p, size_t size, size_t n, void *u) {
  buf_put(u, p, size * n);
  return size * n;
}

// (the request, by libcurl: 1, done, in `g`; 0, nothing done, where
// libcurl is not to be used)
static int http_curl(HttpReq *q, HttpGot *g) {
  HttpEasy *e = http_mine();
  if (!e) return 0;
  void *h = e->h, *list = NULL;
  for (int i = 0; i < q->nh; i++) {
    void *more = http_lc.slist_append(list, q->hs[i]);
    if (!more) { http_lc.slist_free_all(list); return 0; }
    list = more;
  }
  HttpSink sink = { &g->body, h, 0 };
  http_lc.easy_reset(h);
  int bad = http_lc.setopt(h, HTTP_OPT_ERRORBUFFER, e->err) != 0
    || http_lc.setopt(h, HTTP_OPT_URL, q->url) != 0
    || http_lc.setopt(h, HTTP_OPT_WRITEFUNCTION, http_sink) != 0
    || http_lc.setopt(h, HTTP_OPT_WRITEDATA, &sink) != 0
    || http_lc.setopt(h, HTTP_OPT_HEADERFUNCTION, http_head_sink) != 0
    || http_lc.setopt(h, HTTP_OPT_HEADERDATA, &g->head) != 0
    || http_lc.setopt(h, HTTP_OPT_HTTPHEADER, list) != 0
    || (q->follow && (http_lc.setopt(h, HTTP_OPT_FOLLOWLOCATION, 1L) != 0
                      || http_lc.setopt(h, HTTP_OPT_MAXREDIRS, 50L) != 0))
    || (q->head && http_lc.setopt(h, HTTP_OPT_NOBODY, 1L) != 0)
    || (q->custom && http_lc.setopt(h, HTTP_OPT_CUSTOMREQUEST, q->method) != 0)
    || (q->send_body && (http_lc.setopt(h, HTTP_OPT_POSTFIELDSIZE_LARGE, (int64_t)q->nbody) != 0
                         || http_lc.setopt(h, HTTP_OPT_POSTFIELDS, q->nbody ? (const void *)q->body : "") != 0))
    || (q->timeout > 0 && http_lc.setopt(h, HTTP_OPT_TIMEOUT_MS, (long)q->timeout) != 0);
  if (bad) {
    http_lc.easy_reset(h);
    http_lc.slist_free_all(list);
    return 0;
  }
  // (as the tool has them; what an older libcurl has not, it goes without)
  http_lc.setopt(h, HTTP_OPT_BUFFERSIZE, 102400L);
  http_lc.setopt(h, HTTP_OPT_TCP_KEEPALIVE, 1L);
  http_lc.setopt(h, HTTP_OPT_NOSIGNAL, 1L);
  if (http_agent[0]) http_lc.setopt(h, HTTP_OPT_USERAGENT, http_agent);
  e->err[0] = 0;
  int r = http_lc.perform(h);
  // (nothing of this call's left where the handle points)
  http_lc.easy_reset(h);
  http_lc.slist_free_all(list);
  if (r == 0) {
    g->ok = 1;
    return 1;
  }
  g->body.n = 0;
  g->code = r;
  const char *why = e->err[0] ? e->err : http_lc.strerror(r);
  buf_str(&g->why, why ? why : "");
  return 1;
}
#else
static int http_curl(HttpReq *q, HttpGot *g) { (void)q; (void)g; return 0; }
#endif

static V heff_provider_http(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Send: {
    V f = heff_dup(heff_field(p, 1));
    HttpReq q;
    HttpGot g = {0};
    if (!http_req(heff_field(p, 0), &q)) {
      g.code = HTTP_BAD_ARGUMENT;
      buf_str(&g.why, "A libcurl function was given a bad argument");
    } else if (!http_curl(&q, &g)) {
      g.head.n = g.body.n = g.why.n = 0;
      http_spawned(&q, &g);
    }
    http_req_free(&q);
    // (the request's body read: let go of)
    heff_drop(p);
    V x = heff_answer(f, http_answer(&g));
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of http");
    return 0;
  }
}
