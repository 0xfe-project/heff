// The HTTP client's side of std.http, for the C backend: an
// effect provider.
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
// its time in all bounded where it says so (`--max-time`), and the time
// its connection takes to be made (`--connect-timeout`).  The
// response's headers are those of the last response, as written before
// its body (`-D`): a status line starts the headers again.

// (an operation's strings that are not a C string: as curl calls them)
#define HTTP_BAD_ARGUMENT 43
#define HTTP_FAILED_INIT 2
// (room for why libcurl says a request failed)
#define HTTP_ERROR_SIZE 256

// (a request, as both ways have it)
typedef struct {
  char *url;
  char *method;
  char **hs;          // (its headers, `Name: value`, and curl's taken away)
  int nh;
  const uint8_t *body;
  int64_t nbody;
  int64_t timeout;    // (milliseconds; 0, none)
  int64_t connect;    // (the connection's, milliseconds; 0, curl's own)
  int64_t idle;       // (a wait for the head or a piece, milliseconds; 0, none)
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
// method, headers, body, timeout, connectTimeout, follow, idle), as curl is to be given it,
// its body where `rq` keeps it, so `rq` held while it is used: 0 where
// a string of it has a NUL in it)
static int http_req(V rq, HttpReq *q) {
  memset(q, 0, sizeof *q);
  V url = heff_rget_v(rq, 0), method = heff_rget_v(rq, 1), hl = heff_rget_v(rq, 2);
  V body = heff_rget_v(rq, 3), timeout = heff_rget_v(rq, 4), connect = heff_rget_v(rq, 5);
  V follow = heff_rget_v(rq, 6), idle = heff_rget_v(rq, 7);
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
  q->connect = IS_INT(connect) && INT_OF(connect) > 0 ? INT_OF(connect) : 0;
  q->idle = IS_INT(idle) && INT_OF(idle) > 0 ? INT_OF(idle) : 0;
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
  heff_drop(connect);
  heff_drop(follow);
  heff_drop(idle);
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

// (whether headers' text, `n` bytes at `s`, is a whole head that ends
// with its blank line and is the last: not an interim `1xx`, nor, where
// redirects are followed, a `3xx` saying where to go, which curl
// follows)
static int http_head_complete(const char *s, size_t n, int follow) {
  if (!(n >= 2 && s[n - 1] == '\n' && (s[n - 2] == '\n' || (n >= 3 && s[n - 2] == '\r' && s[n - 3] == '\n'))))
    return 0;
  size_t start = n;
  for (size_t i = 0; i < n;) {
    size_t e = i;
    while (e < n && s[e] != '\n') e++;
    if (e - i >= 5 && memcmp(s + i, "HTTP/", 5) == 0) start = i;
    i = e < n ? e + 1 : e;
  }
  if (start == n) return 0;
  const char *sp = memchr(s + start, ' ', n - start);
  long status = sp ? strtol(sp + 1, NULL, 10) : 0;
  if (status >= 100 && status < 200) return 0;
  if (!follow || status < 300 || status >= 400) return 1;
  for (size_t i = start; i < n;) {
    size_t e = i;
    while (e < n && s[e] != '\n') e++;
    size_t k = 0;
    while (k < 9 && i + k < e && (s[i + k] | 0x20) == "location:"[k]) k++;
    if (k == 9) return 0;
    i = e < n ? e + 1 : e;
  }
  return 1;
}

static uint32_t http_response_shape[3] = { LBL_status, LBL_headers, LBL_body };
static uint32_t http_error_shape[3] = { LBL_code, LBL_message, LBL_url };

// (an `Either HttpError Response` of what came back from `url`)
static V http_answer(HttpGot *g, const char *url) {
  V r;
  if (g->ok) {
    int64_t status;
    V hs = http_headers(g->head.s ? g->head.s : "", g->head.n, &status);
    V f[3] = { heff_int(status), hs, heff_bytes_in(g->body.s ? g->body.s : "", (int64_t)g->body.n) };
    V x = heff_rec(3, http_response_shape, f);
    r = heff_tag_n(TAG_Right, 1, &x);
  } else {
    V f[3] = { heff_int(g->code), heff_str_text_n(g->why.s ? g->why.s : "", (int64_t)g->why.n),
               heff_str_text(url ? url : "") };
    V x = heff_rec(3, http_error_shape, f);
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

// (the request, by a `curl` spawned: its body to `g`, or, where `out`
// is given, to that file, its head read whether or not curl failed,
// as what came before it did)
static void http_spawned(HttpReq *q, HttpGot *g, const char *out) {
  char headf[4096], bodyf[4096], secs[32], csecs[32], idles[32];
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
  if (q->connect > 0) {
    snprintf(csecs, sizeof csecs, "%lld.%03lld", (long long)(q->connect / 1000), (long long)(q->connect % 1000));
    as[n++] = "--connect-timeout";
    as[n++] = csecs;
  }
  if (q->idle > 0) {
    // (curl's tool has no limit of its own on a silence: what is slower
    // than a byte a second for as many seconds, the nearest it has)
    snprintf(idles, sizeof idles, "%lld", (long long)((q->idle + 999) / 1000));
    as[n++] = "--speed-limit";
    as[n++] = "1";
    as[n++] = "--speed-time";
    as[n++] = idles;
  }
  if (out) {
    as[n++] = "-o";
    as[n++] = (char *)out;
  }
  as[n++] = "-D";
  as[n++] = headf;
  as[n++] = "--";
  as[n++] = q->url;
  as[n] = NULL;
  Buf err = {0};
  int64_t status = ok ? heff_capture(as, &g->body, &err) : -1;
  if (status == 0 || (ok && out)) {
    FILE *f = fopen(headf, "rb");
    if (f) {
      char chunk[65536];
      size_t got;
      while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) buf_put(&g->head, chunk, got);
      fclose(f);
    }
  }
  if (status == 0) {
    g->ok = 1;
    if (q->head) g->body.n = 0;
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
// always the spawned curl.
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
#define HTTP_OPT_CONNECTTIMEOUT_MS 156
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
#define HTTP_OPT_PRIVATE 10103
#define HTTP_OPT_SUPPRESS_CONNECT_HEADERS 265
#define HTTP_WRITEFUNC_PAUSE 0x10000001
#define HTTP_PAUSE_CONT 0
#define HTTP_INFO_PRIVATE (0x100000 + 21)
#define HTTP_MOPT_SOCKETFUNCTION 20001
#define HTTP_MOPT_SOCKETDATA 10002
#define HTTP_MOPT_TIMERFUNCTION 20004
#define HTTP_MOPT_TIMERDATA 10005
#define HTTP_POLL_IN 1
#define HTTP_POLL_OUT 2
#define HTTP_POLL_REMOVE 4
#define HTTP_CSELECT_IN 1
#define HTTP_CSELECT_OUT 2
#define HTTP_CSELECT_ERR 4
#define HTTP_SOCKET_TIMEOUT (-1)
#define HTTP_MSG_DONE 1

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
  // (the multi interface, for a group of the reactor's: may be missing)
  void *(*multi_init)(void);
  int (*multi_setopt)(void *, int, ...);
  int (*multi_add)(void *, void *);
  int (*multi_remove)(void *, void *);
  int (*multi_action)(void *, int, int, int *);
  void *(*multi_info)(void *, int *);
  int (*easy_pause)(void *, int);    // (may be missing: no streamed responses then)
  int (*multi_cleanup)(void *);       // (likewise)
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
  HTTP_SYM(multi_init, "curl_multi_init");
  HTTP_SYM(multi_setopt, "curl_multi_setopt");
  HTTP_SYM(multi_add, "curl_multi_add_handle");
  HTTP_SYM(multi_remove, "curl_multi_remove_handle");
  HTTP_SYM(multi_action, "curl_multi_socket_action");
  HTTP_SYM(multi_info, "curl_multi_info_read");
  HTTP_SYM(easy_pause, "curl_easy_pause");
  HTTP_SYM(multi_cleanup, "curl_multi_cleanup");
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

// (libcurl found and ready, once: whether it is to be used)
static int http_loaded(void) {
#ifdef HEFF_THREADS
  pthread_once(&http_once, http_load);
#else
  static int loaded;
  if (!loaded) { loaded = 1; http_load(); }
#endif
  return http_lc_ok;
}

// (this thread's handle -- or NULL, libcurl not to be used)
static HttpEasy *http_mine(void) {
  if (!http_loaded()) return NULL;
#ifdef HEFF_THREADS
  HttpEasy *e = pthread_getspecific(http_key);
  if (!e) {
    e = calloc(1, sizeof *e);
    if (!e) return NULL;
    if (pthread_setspecific(http_key, e) != 0) { free(e); return NULL; }
  }
#else
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

typedef size_t (*HttpWriteFn)(char *, size_t, size_t, void *);

// (the handle `h` set up for the request, as the tool sets its own --
// its body to `body` (given `bodyto`), its headers to `head` (given
// `headto`), its headers sent `list`, why it failed to `err` -- from
// reset: 0, or 1 where libcurl refused it)
static int http_setup(void *h, HttpReq *q, HttpWriteFn body, void *bodyto, HttpWriteFn head, void *headto,
                      void *list, char *err) {
  http_lc.easy_reset(h);
  int bad = http_lc.setopt(h, HTTP_OPT_ERRORBUFFER, err) != 0
    || http_lc.setopt(h, HTTP_OPT_URL, q->url) != 0
    || http_lc.setopt(h, HTTP_OPT_WRITEFUNCTION, body) != 0
    || http_lc.setopt(h, HTTP_OPT_WRITEDATA, bodyto) != 0
    || http_lc.setopt(h, HTTP_OPT_HEADERFUNCTION, head) != 0
    || http_lc.setopt(h, HTTP_OPT_HEADERDATA, headto) != 0
    || http_lc.setopt(h, HTTP_OPT_HTTPHEADER, list) != 0
    || (q->follow && (http_lc.setopt(h, HTTP_OPT_FOLLOWLOCATION, 1L) != 0
                      || http_lc.setopt(h, HTTP_OPT_MAXREDIRS, 50L) != 0))
    || (q->head && http_lc.setopt(h, HTTP_OPT_NOBODY, 1L) != 0)
    || (q->custom && http_lc.setopt(h, HTTP_OPT_CUSTOMREQUEST, q->method) != 0)
    || (q->send_body && (http_lc.setopt(h, HTTP_OPT_POSTFIELDSIZE_LARGE, (int64_t)q->nbody) != 0
                         || http_lc.setopt(h, HTTP_OPT_POSTFIELDS, q->nbody ? (const void *)q->body : "") != 0))
    || (q->timeout > 0 && http_lc.setopt(h, HTTP_OPT_TIMEOUT_MS, (long)q->timeout) != 0)
    || (q->connect > 0 && http_lc.setopt(h, HTTP_OPT_CONNECTTIMEOUT_MS, (long)q->connect) != 0);
  if (bad) {
    http_lc.easy_reset(h);
    return 1;
  }
  // (as the tool has them; what an older libcurl has not, it goes without)
  http_lc.setopt(h, HTTP_OPT_BUFFERSIZE, 102400L);
  http_lc.setopt(h, HTTP_OPT_TCP_KEEPALIVE, 1L);
  http_lc.setopt(h, HTTP_OPT_NOSIGNAL, 1L);
  if (http_agent[0]) http_lc.setopt(h, HTTP_OPT_USERAGENT, http_agent);
  err[0] = 0;
  return 0;
}

// (the request's headers, as libcurl takes them; NULL, with `*ok` 0,
// where it could not)
static void *http_list(HttpReq *q, int *ok) {
  void *list = NULL;
  *ok = 1;
  for (int i = 0; i < q->nh; i++) {
    void *more = http_lc.slist_append(list, q->hs[i]);
    if (!more) {
      http_lc.slist_free_all(list);
      *ok = 0;
      return NULL;
    }
    list = more;
  }
  return list;
}

// (what libcurl's code `r` comes to, in `g`)
static void http_ended(HttpGot *g, int r, const char *err) {
  if (r == 0) {
    g->ok = 1;
    return;
  }
  g->body.n = 0;
  g->code = r;
  const char *why = err[0] ? err : http_lc.strerror(r);
  buf_str(&g->why, why ? why : "");
}

// (the request, by libcurl: 1, done, in `g`; 0, nothing done, where
// libcurl is not to be used)
static int http_curl(HttpReq *q, HttpGot *g) {
  HttpEasy *e = http_mine();
  if (!e) return 0;
  void *h = e->h;
  int ok;
  void *list = http_list(q, &ok);
  if (!ok) return 0;
  HttpSink sink = { &g->body, h, 0 };
  if (http_setup(h, q, http_sink, &sink, http_head_sink, &g->head, list, e->err)) {
    http_lc.slist_free_all(list);
    return 0;
  }
  int r = http_lc.perform(h);
  // (nothing of this call's left where the handle points)
  http_lc.easy_reset(h);
  http_lc.slist_free_all(list);
  http_ended(g, r, e->err);
  return 1;
}

// Requests pending in the reactor: libcurl's multi interface
// ---------------------------------------------------------------------
//
// Where a scheduler of std.reactor's runs (the runtime's reactor, a
// group being started in), a request is a transfer of this thread's
// multi handle, pending while the other threads go on: its easy
// handle added to the multi, the request's wait done when libcurl says
// so.  The multi is a source of the reactor's: the sockets libcurl asks
// to be watched (its socket function) and how long until it is to be
// told the time is up (its timer function) are polled with the
// reactor's others, and libcurl told of each ready, and of the time
// (`curl_multi_socket_action`); a transfer done (`curl_multi_info_read`)
// is its wait done.  Connections are the multi's, kept for the next
// transfer; easy handles let go of are kept, a few, for the next.  Each
// thread of the system has a multi of its own, made at its first such
// request; one a process was forked with is the parent's, left as it
// is.  Where libcurl has no multi interface, a request waits, as without
// a scheduler.

typedef struct {
  HeffWait w;          // (first: a transfer is its wait, its `fd` -1:
                       //  the multi's to say when it is done)
  void *h;
  void *list;
  HttpReq q;
  HttpGot g;
  HttpSink sink;
  char err[HTTP_ERROR_SIZE];
  V held;              // (the operation: the request's body is its)
  int result;
} HttpTransfer;

typedef struct { int fd; short events; } HttpWatched;

// (this thread's multi: its sockets watched, the time it is to be
// told of, easy handles kept)
typedef struct {
  void *multi;
  pid_t pid;
  HttpWatched *watched;
  int nwatched, capwatched;
  int64_t deadline;    // (ms of the monotonic clock; -1, none)
  void *idle[8];
  int nidle;
  HeffSource source;
} HttpMulti;

static HEFF_TLS HttpMulti http_m;

static int64_t http_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// (libcurl's word on a socket: watched for these, or no longer)
// (the sockets watched, `*n` of them at `*w`, room for `*cap`: `fd`
// watched for what libcurl says, or no longer)
static void http_watch(HttpWatched **w, int *n, int *cap, int fd, int what) {
  int i = 0;
  while (i < *n && (*w)[i].fd != fd) i++;
  if (what == HTTP_POLL_REMOVE) {
    if (i < *n) (*w)[i] = (*w)[--*n];
    return;
  }
  if (i == *n) {
    if (*n == *cap) {
      int more = *cap ? *cap * 2 : 16;
      HttpWatched *m = heff_alloc(sizeof *m * (size_t)more);
      if (*n) memcpy(m, *w, sizeof *m * (size_t)*n);
      free(*w);
      *w = m;
      *cap = more;
    }
    (*n)++;
  }
  (*w)[i].fd = fd;
  (*w)[i].events = (short)(((what & HTTP_POLL_IN) ? POLLIN : 0) | ((what & HTTP_POLL_OUT) ? POLLOUT : 0));
}

static int http_socket_fn(void *easy, int fd, int what, void *u, void *sp) {
  (void)easy; (void)u; (void)sp;
  http_watch(&http_m.watched, &http_m.nwatched, &http_m.capwatched, fd, what);
  return 0;
}

// (libcurl's word on its time: to be told in `ms`, or never, -1)
static int http_timer_fn(void *multi, long ms, void *u) {
  (void)multi; (void)u;
  http_m.deadline = ms < 0 ? -1 : http_now() + ms;
  return 0;
}

// (the source's descriptors, and how long at most to wait for them)
static int http_fds(struct pollfd *p, int room, int *ms) {
  if (room >= http_m.nwatched)
    for (int i = 0; i < http_m.nwatched; i++) {
      p[i].fd = http_m.watched[i].fd;
      p[i].events = http_m.watched[i].events;
    }
  if (http_m.deadline >= 0) {
    int64_t left = http_m.deadline - http_now();
    *ms = left < 0 ? 0 : left > 1000000 ? 1000000 : (int)left;
  }
  return http_m.nwatched;
}

// (the transfers libcurl is done with: their waits done)
static void http_finished(void) {
  int left;
  int *m;
  while ((m = http_lc.multi_info(http_m.multi, &left))) {
    // (a CURLMsg: its kind, its easy handle, its code)
    struct { int msg; void *easy; union { void *p; int result; } data; } *msg = (void *)m;
    if (msg->msg != HTTP_MSG_DONE) continue;
    HttpTransfer *t = NULL;
    http_lc.getinfo(msg->easy, HTTP_INFO_PRIVATE, &t);
    if (!t) continue;
    t->result = msg->data.result;
    http_lc.multi_remove(http_m.multi, t->h);
    heff_wait_done(&t->w);
  }
}

// (after the poll: libcurl told of each socket ready, and of its time)
static void http_ready(struct pollfd *p, int n) {
  int running;
  for (int i = 0; i < n; i++) {
    if (!p[i].revents) continue;
    // (a socket closed or failed is read from, or written to, as libcurl
    // waits to: what it then finds is its to say, as when it polls itself)
    int ev = ((p[i].revents & (POLLIN | POLLHUP | POLLERR)) ? HTTP_CSELECT_IN : 0)
      | ((p[i].revents & (POLLOUT | POLLERR)) ? HTTP_CSELECT_OUT : 0)
      | ((p[i].revents & POLLNVAL) ? HTTP_CSELECT_ERR : 0);
    http_lc.multi_action(http_m.multi, p[i].fd, ev, &running);
  }
  if (http_m.deadline >= 0 && http_now() >= http_m.deadline) {
    http_m.deadline = -1;
    http_lc.multi_action(http_m.multi, HTTP_SOCKET_TIMEOUT, 0, &running);
  }
  http_finished();
}

// (this thread's multi, made at its first request: 0, or -1 where
// there is none to be had)
static int http_multi_mine(void) {
  if (!http_loaded() || !http_lc.multi_init || !http_lc.multi_setopt || !http_lc.multi_add
      || !http_lc.multi_remove || !http_lc.multi_action || !http_lc.multi_info || !http_lc.getinfo)
    return -1;
  pid_t me = getpid();
  if (http_m.multi && http_m.pid == me) return 0;
  // (none yet, or the parent's: one of this process's own)
  void *m = http_lc.multi_init();
  if (!m) return -1;
  http_lc.multi_setopt(m, HTTP_MOPT_SOCKETFUNCTION, http_socket_fn);
  http_lc.multi_setopt(m, HTTP_MOPT_SOCKETDATA, NULL);
  http_lc.multi_setopt(m, HTTP_MOPT_TIMERFUNCTION, http_timer_fn);
  http_lc.multi_setopt(m, HTTP_MOPT_TIMERDATA, NULL);
  http_m.multi = m;
  http_m.pid = me;
  http_m.nwatched = 0;
  http_m.deadline = -1;
  http_m.nidle = 0;
  http_m.source.fds = http_fds;
  http_m.source.ready = http_ready;
  heff_reactor_source(&http_m.source);
  return 0;
}

// (the transfer's handle kept for the next, or let go of; and what it
// held)
static void http_transfer_free(HttpTransfer *t) {
  if (t->h) {
    http_lc.easy_reset(t->h);
    if (http_m.nidle < (int)(sizeof http_m.idle / sizeof http_m.idle[0])) http_m.idle[http_m.nidle++] = t->h;
    else http_lc.easy_cleanup(t->h);
  }
  if (t->list) http_lc.slist_free_all(t->list);
  http_req_free(&t->q);
  heff_drop(t->held);
  free(t);
}

static V http_transfer_answer(HeffWait *w) {
  HttpTransfer *t = (HttpTransfer *)w;
  http_ended(&t->g, t->result, t->err);
  V r = http_answer(&t->g, t->q.url);
  http_transfer_free(t);
  return r;
}

static void http_transfer_cancel(HeffWait *w) {
  HttpTransfer *t = (HttpTransfer *)w;
  if (!t->w.done) http_lc.multi_remove(http_m.multi, t->h);
  free(t->g.head.s);
  free(t->g.body.s);
  free(t->g.why.s);
  http_transfer_free(t);
}

// (never stepped: the multi says when it is done)
static int http_transfer_step(HeffWait *w) {
  (void)w;
  return HEFF_STEP_WAIT;
}

// (the request `q` of the operation `p`, a transfer of this thread's
// multi, pending: what `heff_outside` gives back; or 0, nothing done,
// where there is no multi -- `q` and `p` the caller's again)
static V http_transfer(HttpReq *q, V p, V f, V k) {
  if (http_multi_mine() < 0) return 0;
  void *h = http_m.nidle ? http_m.idle[--http_m.nidle] : http_lc.easy_init();
  if (!h) return 0;
  HttpTransfer *t = heff_alloc(sizeof *t);
  memset(t, 0, sizeof *t);
  t->h = h;
  t->w.fd = -1;
  t->w.step = http_transfer_step;
  t->w.answer = http_transfer_answer;
  t->w.cancel = http_transfer_cancel;
  int ok;
  t->list = http_list(q, &ok);
  t->sink.b = &t->g.body;
  t->sink.h = h;
  if (!ok || http_setup(h, q, http_sink, &t->sink, http_head_sink, &t->g.head, t->list, t->err)
      || http_lc.setopt(h, HTTP_OPT_PRIVATE, (void *)t) != 0
      || http_lc.multi_add(http_m.multi, h) != 0) {
    if (t->list) http_lc.slist_free_all(t->list);
    http_lc.easy_reset(h);
    if (http_m.nidle < (int)(sizeof http_m.idle / sizeof http_m.idle[0])) http_m.idle[http_m.nidle++] = h;
    else http_lc.easy_cleanup(h);
    free(t);
    return 0;
  }
  // (the request and the operation the transfer's from now on)
  t->q = *q;
  memset(q, 0, sizeof *q);
  t->held = p;
  return heff_outside(&t->w, f, k);
}
#else
static int http_curl(HttpReq *q, HttpGot *g) { (void)q; (void)g; return 0; }
static V http_transfer(HttpReq *q, V p, V f, V k) { (void)q; (void)p; (void)f; (void)k; return 0; }
#endif

// Streamed responses
// ---------------------------------------------------------------------
//
// A request whose response is read a piece at a time (`Open`, then
// `Piece` after `Piece`) is a transfer of a multi handle of its own, a
// pair of a multi and an easy handle: no thread's, driven by whichever
// thread asks for its next piece, as libcurl allows a handle to be used
// by one thread after another, never by two at once.  Its head is
// gathered as it comes, whole at its blank line unless curl goes on to
// another (an interim `1xx`, a redirect it follows), or at the body's
// first piece, or the transfer's end.  A piece of the body is kept until
// the program asks for it; once a piece's worth is kept, what comes next
// pauses the transfer (`CURL_WRITEFUNC_PAUSE`: libcurl keeps what it was
// given, the server no longer read from), and asking for the next piece
// goes on with it.  So what is held of a body at once is about a piece,
// whatever its length.
//
// Asked for where no group is being started in, the head or the next
// piece is waited for there: the pair's sockets polled, and libcurl told
// of each ready, until it has come.  In a group, it is a wait of the
// reactor's, done when it has come: each thread's waits for pieces are a
// source of its reactor's, the sockets of their pairs polled with the
// reactor's others.  Either way, `idle` bounds that wait: past it, the
// transfer is ended, curl's code 28.
//
// A response is read by one at a time: whoever asks for a piece while
// another is being waited for -- another thread, holding the response
// too -- is told so, an error, and the read goes on.  It is a foreign
// value, let go of by whichever thread drops it last: never while a
// piece is asked for, as the asking holds it.  Its transfer is ended
// then, and the pair kept, a few of them, for the next response, its
// connections with it: a multi keeps its connections for the next
// transfer.  A pair a process was forked with is its parent's, left as
// it is.
//
// Where libcurl is not to be used, the spawned curl writes the body to
// a temporary file, and its pieces are read from that: never all of it
// in memory, but none of it before curl is done.  Its silence is
// bounded as the tool's own bound is, in whole seconds (`--speed-time`).

// (a piece's worth: once this much is kept, the transfer pauses)
#define HTTP_PIECE ((size_t)1 << 16)

typedef struct HttpPull HttpPull;
typedef struct HttpPair HttpPair;

typedef struct {
  HttpPair *pair;       // (its multi and easy handle, libcurl's; NULL, curl spawned)
  void *list;
  HttpReq q;
  V rq;                 // (the request: its body's bytes the transfer's to send)
  Buf head;             // (its headers' text, as they come)
  Buf got;              // (what came of the body, not yet asked for)
  int paused;
  int head_done;        // (its head whole)
  int done;             // (its transfer ended)
  int64_t code;         // (once ended, curl's code: 0, the body whole)
  Buf why;
  char err[HTTP_ERROR_SIZE];
  int busy;             // (being read, by a thread: atomic)
  HttpPull *waiting;    // (a wait of the reactor's for it)
  FILE *file;           // (curl spawned: the body as curl wrote it)
  char bodyf[4096];
} HttpStream;

static uint32_t http_opened_shape[3] = { LBL_status, LBL_headers, LBL_transfer };

// (the response taken to be read: 0 where it is being read already)
static int http_stream_take(HttpStream *s) {
  return !__atomic_exchange_n(&s->busy, 1, __ATOMIC_ACQ_REL);
}

// (the response read, for the next to read)
static void http_stream_give(HttpStream *s) { __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE); }

// (an error of the response's: curl's code `code`, and why)
static V http_stream_error(HttpStream *s, int64_t code, const char *why, size_t n) {
  V f[3] = { heff_int(code), heff_str_text_n(why, (int64_t)n), heff_str_text(s->q.url ? s->q.url : "") };
  V x = heff_rec(3, http_error_shape, f);
  return heff_tag_n(TAG_Left, 1, &x);
}

// (the head come, an `Either HttpError Opened`, `v` -- the response, a
// foreign value -- kept in it; or, the transfer ended with no head, why,
// `v` let go of)
static V http_opened(HttpStream *s, V v) {
  if (!s->head_done && s->code != 0) {
    V r = http_stream_error(s, s->code, s->why.s ? s->why.s : "", s->why.n);
    heff_drop(v);
    return r;
  }
  int64_t status;
  V hs = http_headers(s->head.s ? s->head.s : "", s->head.n, &status);
  V f[3] = { heff_int(status), hs, v };
  V x = heff_rec(3, http_opened_shape, f);
  return heff_tag_n(TAG_Right, 1, &x);
}

// (the next piece, an `Either HttpError Bytes`: what came of the body;
// at its end, no bytes, or why it did not end whole)
static V http_piece(HttpStream *s) {
  if (s->file) {
    static HEFF_TLS char chunk[HTTP_PIECE];
    size_t n = fread(chunk, 1, sizeof chunk, s->file);
    if (n > 0) {
      V b = heff_bytes_in(chunk, (int64_t)n);
      return heff_tag_n(TAG_Right, 1, &b);
    }
    fclose(s->file);
    s->file = NULL;
  }
  if (s->got.n > 0) {
    V b = heff_bytes_in(s->got.s, (int64_t)s->got.n);
    s->got.n = 0;
    return heff_tag_n(TAG_Right, 1, &b);
  }
  if (s->code != 0) return http_stream_error(s, s->code, s->why.s ? s->why.s : "", s->why.n);
  V b = heff_bytes_in("", 0);
  return heff_tag_n(TAG_Right, 1, &b);
}

// (the response, by a `curl` spawned, its body to a file of its own)
static void http_stream_spawned(HttpStream *s) {
  HttpGot g = {0};
  s->done = 1;
  int fd = http_temp(s->bodyf, sizeof s->bodyf);
  if (fd < 0) {
    s->bodyf[0] = 0;
    s->code = HTTP_FAILED_INIT;
    buf_str(&s->why, "a file for curl could not be written");
    return;
  }
  http_close(fd);
  http_spawned(&s->q, &g, s->bodyf);
  s->head = g.head;
  s->head_done = g.head.n && (g.ok || http_head_complete(g.head.s, g.head.n, s->q.follow));
  if (!g.ok) {
    s->code = g.code;
    s->why = g.why;
  } else
    free(g.why.s);
  free(g.body.s);
  if (s->head_done && !s->q.head) s->file = fopen(s->bodyf, "rb");
}

#ifdef HTTP_CURL
// (a wait of the reactor's for a streamed response's head, or for its
// next piece; among its thread's, while it waits)
struct HttpPull {
  HeffWait w;           // (first: the pull is its wait)
  HttpStream *s;
  V held;               // (the head's: the response, a foreign value; a
                        //  piece's: the operation)
  int head;
  int at, n;            // (where its pair's sockets are among the source's polled)
  HttpPull *prev, *next;
};

// (a response's multi and easy handle, the sockets libcurl asks to be
// watched and when it is to be told the time is up; the response it is
// carrying)
struct HttpPair {
  void *multi, *easy;
  long pid;
  HttpWatched *watched;
  int nwatched, capwatched;
  int64_t deadline;     // (ms of the monotonic clock; -1, none)
  HttpStream *s;
};

// (pairs let go of, kept for the next response, by any thread)
#define HTTP_PAIRS_KEPT 8
static HttpPair *http_pairs[HTTP_PAIRS_KEPT];
static int http_npairs;
#ifdef HEFF_THREADS
static pthread_mutex_t http_pairs_mu = PTHREAD_MUTEX_INITIALIZER;
#define HTTP_PAIRS_LOCK() pthread_mutex_lock(&http_pairs_mu)
#define HTTP_PAIRS_UNLOCK() pthread_mutex_unlock(&http_pairs_mu)
#else
#define HTTP_PAIRS_LOCK() ((void)0)
#define HTTP_PAIRS_UNLOCK() ((void)0)
#endif

// (whether what is waited for -- the head, or the next piece -- has come)
static int http_stream_has(HttpStream *s, int head) {
  return s->done || (head ? s->head_done : s->got.n > 0);
}

// (libcurl's word on a socket of the pair's: watched for these, or no
// longer)
static int http_pair_socket(void *easy, int fd, int what, void *u, void *sp) {
  (void)easy; (void)sp;
  HttpPair *p = u;
  http_watch(&p->watched, &p->nwatched, &p->capwatched, fd, what);
  return 0;
}

// (libcurl's word on the pair's time: to be told in `ms`, or never, -1)
static int http_pair_timer(void *multi, long ms, void *u) {
  (void)multi;
  HttpPair *p = u;
  p->deadline = ms < 0 ? -1 : http_now() + ms;
  return 0;
}

// (a pair, kept or made: NULL where libcurl has not what it takes)
static HttpPair *http_pair_get(void) {
  if (!http_loaded() || !http_lc.multi_init || !http_lc.multi_setopt || !http_lc.multi_add
      || !http_lc.multi_remove || !http_lc.multi_action || !http_lc.multi_info || !http_lc.multi_cleanup
      || !http_lc.easy_pause)
    return NULL;
  long me = (long)getpid();
  HttpPair *p = NULL;
  HTTP_PAIRS_LOCK();
  // (those a process was forked with are its parent's: left as they are)
  while (http_npairs && !p) {
    p = http_pairs[--http_npairs];
    if (p->pid != me) p = NULL;
  }
  HTTP_PAIRS_UNLOCK();
  if (p) return p;
  p = heff_alloc(sizeof *p);
  memset(p, 0, sizeof *p);
  p->multi = http_lc.multi_init();
  p->easy = http_lc.easy_init();
  if (!p->multi || !p->easy) {
    if (p->multi) http_lc.multi_cleanup(p->multi);
    if (p->easy) http_lc.easy_cleanup(p->easy);
    free(p);
    return NULL;
  }
  p->pid = me;
  p->deadline = -1;
  http_lc.multi_setopt(p->multi, HTTP_MOPT_SOCKETFUNCTION, http_pair_socket);
  http_lc.multi_setopt(p->multi, HTTP_MOPT_SOCKETDATA, p);
  http_lc.multi_setopt(p->multi, HTTP_MOPT_TIMERFUNCTION, http_pair_timer);
  http_lc.multi_setopt(p->multi, HTTP_MOPT_TIMERDATA, p);
  return p;
}

// (a pair let go of: kept for the next, or cleaned up)
static void http_pair_put(HttpPair *p) {
  http_lc.easy_reset(p->easy);
  p->s = NULL;
  HTTP_PAIRS_LOCK();
  int kept = http_npairs < HTTP_PAIRS_KEPT;
  if (kept) http_pairs[http_npairs++] = p;
  HTTP_PAIRS_UNLOCK();
  if (kept) return;
  http_lc.multi_cleanup(p->multi);
  http_lc.easy_cleanup(p->easy);
  free(p->watched);
  free(p);
}

// (a piece of the body come: kept -- or, a piece's worth kept already,
// the transfer paused)
static size_t http_stream_sink(char *p, size_t size, size_t n, void *u) {
  HttpStream *s = u;
  size_t len = size * n;
  if (s->got.n >= HTTP_PIECE) {
    s->paused = 1;
    return HTTP_WRITEFUNC_PAUSE;
  }
  s->head_done = 1;
  buf_put(&s->got, p, len);
  if (s->waiting) heff_wait_done(&s->waiting->w);
  return len;
}

// (a header line come: the head whole at its blank line, where it is
// the last)
static size_t http_stream_head(char *p, size_t size, size_t n, void *u) {
  HttpStream *s = u;
  size_t len = size * n;
  buf_put(&s->head, p, len);
  if ((len == 1 || (len == 2 && p[0] == '\r')) && p[len - 1] == '\n'
      && http_head_complete(s->head.s, s->head.n, s->q.follow)) {
    s->head_done = 1;
    if (s->waiting && s->waiting->head) heff_wait_done(&s->waiting->w);
  }
  return len;
}

// (the transfer ended, before libcurl was done with it or as it was:
// curl's code `code`, and why)
static void http_stream_cut(HttpStream *s, int64_t code, const char *why) {
  if (s->done) return;
  http_lc.multi_remove(s->pair->multi, s->pair->easy);
  s->done = 1;
  s->code = code;
  if (code) buf_str(&s->why, why);
  if (s->waiting) heff_wait_done(&s->waiting->w);
}

// (the transfer ended, waited for longer than `idle`: curl's code 28)
static void http_stream_silent(HttpStream *s) {
  char why[128];
  snprintf(why, sizeof why, "Operation timed out after %lld milliseconds with no data received", (long long)s->q.idle);
  http_stream_cut(s, 28, why);
}

// (libcurl told of the pair's sockets ready, `n` of them at `fds`, as
// polled, and of its time; the transfer done, where libcurl is done)
static void http_pair_act(HttpPair *p, struct pollfd *fds, int n) {
  int running;
  for (int i = 0; i < n; i++) {
    if (!fds[i].revents) continue;
    int ev = ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) ? HTTP_CSELECT_IN : 0)
      | ((fds[i].revents & (POLLOUT | POLLERR)) ? HTTP_CSELECT_OUT : 0)
      | ((fds[i].revents & POLLNVAL) ? HTTP_CSELECT_ERR : 0);
    http_lc.multi_action(p->multi, fds[i].fd, ev, &running);
  }
  if (p->deadline >= 0 && http_now() >= p->deadline) {
    p->deadline = -1;
    http_lc.multi_action(p->multi, HTTP_SOCKET_TIMEOUT, 0, &running);
  }
  int left;
  int *m;
  while ((m = http_lc.multi_info(p->multi, &left))) {
    struct { int msg; void *easy; union { void *p; int result; } data; } *msg = (void *)m;
    if (msg->msg != HTTP_MSG_DONE || !p->s) continue;
    HttpStream *s = p->s;
    const char *why = s->err[0] ? s->err : http_lc.strerror(msg->data.result);
    http_stream_cut(s, msg->data.result, why ? why : "");
  }
}

// (the response's transfer, of a pair of its own: 1, begun; 0, nothing
// done, where libcurl is not to be used)
static int http_stream_curl(HttpStream *s) {
  HttpPair *p = http_pair_get();
  if (!p) return 0;
  int ok;
  void *list = http_list(&s->q, &ok);
  if (!ok || http_setup(p->easy, &s->q, http_stream_sink, s, http_stream_head, s, list, s->err)) {
    if (list) http_lc.slist_free_all(list);
    http_pair_put(p);
    return 0;
  }
  // (a proxy's answer to `CONNECT` is not the response's head)
  http_lc.setopt(p->easy, HTTP_OPT_SUPPRESS_CONNECT_HEADERS, 1L);
  p->s = s;
  if (http_lc.multi_add(p->multi, p->easy) != 0) {
    http_lc.slist_free_all(list);
    http_pair_put(p);
    return 0;
  }
  s->pair = p;
  s->list = list;
  return 1;
}

// (the head, or the next piece, waited for here, no group being started
// in: the pair's sockets polled, and libcurl told of each ready)
static void http_stream_drive(HttpStream *s, int head) {
  HttpPair *pr = s->pair;
  int64_t until = s->q.idle > 0 ? http_now() + s->q.idle : 0;
  struct pollfd *p = NULL;
  int room = 0;
  while (!http_stream_has(s, head)) {
    if (room < pr->nwatched + 1) {
      free(p);
      room = pr->nwatched + 16;
      p = heff_alloc(sizeof *p * (size_t)room);
    }
    int n = pr->nwatched;
    for (int i = 0; i < n; i++) {
      p[i].fd = pr->watched[i].fd;
      p[i].events = pr->watched[i].events;
      p[i].revents = 0;
    }
    int ms = -1;
    if (pr->deadline >= 0) {
      int64_t left = pr->deadline - http_now();
      ms = left < 0 ? 0 : left > 1000000 ? 1000000 : (int)left;
    }
    if (until) {
      int64_t left = until - http_now();
      if (left <= 0) {
        http_stream_silent(s);
        break;
      }
      if (ms < 0 || ms > left) ms = (int)left;
    }
    if (n == 0 && ms < 0) ms = 1000;
    if (poll(p, (nfds_t)n, ms) < 0) {
      if (errno == EINTR) continue;
      http_stream_cut(s, HTTP_FAILED_INIT, strerror(errno));
      break;
    }
    http_pair_act(pr, p, n);
  }
  free(p);
}

// (this thread's waits for responses' pieces, and the source of its
// reactor's that has their pairs' sockets polled)
static HEFF_TLS HttpPull *http_pulls;
static HEFF_TLS HeffSource http_pull_source;

static int http_pulls_fds(struct pollfd *p, int room, int *ms) {
  int n = 0;
  for (HttpPull *u = http_pulls; u; u = u->next) {
    u->at = n;
    u->n = 0;
    if (u->w.done) continue;
    HttpPair *pr = u->s->pair;
    if (p && n + pr->nwatched <= room) {
      for (int i = 0; i < pr->nwatched; i++) {
        p[n + i].fd = pr->watched[i].fd;
        p[n + i].events = pr->watched[i].events;
      }
      u->n = pr->nwatched;
    }
    n += pr->nwatched;
    if (pr->deadline >= 0) {
      int64_t left = pr->deadline - http_now();
      int t = left < 0 ? 0 : left > 1000000 ? 1000000 : (int)left;
      if (*ms < 0 || t < *ms) *ms = t;
    }
  }
  return n;
}

static void http_pulls_ready(struct pollfd *p, int n) {
  for (HttpPull *u = http_pulls; u; u = u->next) {
    if (u->w.done || u->at + u->n > n) continue;
    http_pair_act(u->s->pair, p + u->at, u->n);
    if (http_stream_has(u->s, u->head)) heff_wait_done(&u->w);
  }
}

// (the pull no longer among its thread's, its response free to be read)
static void http_pull_off(HttpPull *u) {
  if (u->prev) u->prev->next = u->next;
  else http_pulls = u->next;
  if (u->next) u->next->prev = u->prev;
  u->s->waiting = NULL;
  http_stream_give(u->s);
}

static V http_pull_answer(HeffWait *w) {
  HttpPull *u = (HttpPull *)w;
  HttpStream *s = u->s;
  if (w->err == ETIMEDOUT && !http_stream_has(s, u->head)) http_stream_silent(s);
  V r;
  if (u->head) {
    // (the response, not yet the program's, may be let go of here)
    http_pull_off(u);
    r = http_opened(s, u->held);
  } else {
    r = http_piece(s);
    http_pull_off(u);
    heff_drop(u->held);
  }
  free(u);
  return r;
}

static void http_pull_cancel(HeffWait *w) {
  HttpPull *u = (HttpPull *)w;
  http_pull_off(u);
  V held = u->held;
  free(u);
  heff_drop(held);
}

// (the head, or the next piece, of `s` pending in the reactor, its wait
// holding `held`)
static V http_pull(HttpStream *s, int head, V held, V f, V k) {
  if (!http_pull_source.on) {
    http_pull_source.fds = http_pulls_fds;
    http_pull_source.ready = http_pulls_ready;
    heff_reactor_source(&http_pull_source);
  }
  HttpPull *u = heff_alloc(sizeof *u);
  memset(u, 0, sizeof *u);
  u->w.fd = -1;
  u->w.until = s->q.idle > 0 ? heff_now_ms() + s->q.idle : 0;
  u->w.step = http_transfer_step;
  u->w.answer = http_pull_answer;
  u->w.cancel = http_pull_cancel;
  u->s = s;
  u->held = held;
  u->head = head;
  u->next = http_pulls;
  if (http_pulls) http_pulls->prev = u;
  http_pulls = u;
  s->waiting = u;
  return heff_outside(&u->w, f, k);
}

// (what is asked for of `s` -- its head, or its next piece -- come, or
// waited for here: 0; else the operation, pending in the reactor, `s`
// read until its wait is answered)
static V http_stream_wait(HttpStream *s, int head, V held, V f, V k) {
  if (!s->pair) return 0;
  if (!head && s->got.n == 0 && s->paused && !s->done) {
    s->paused = 0;
    http_lc.easy_pause(s->pair->easy, HTTP_PAUSE_CONT);
  }
  if (http_stream_has(s, head)) return 0;
  if (heff_group) return http_pull(s, head, held, f, k);
  http_stream_drive(s, head);
  return 0;
}

// (the transfer ended and its pair kept, by whichever thread lets go of
// the response; one a process was forked with left as it is)
static void http_stream_release(HttpStream *s) {
  HttpPair *p = s->pair;
  if (!p || p->pid != (long)getpid()) return;
  if (!s->done) http_lc.multi_remove(p->multi, p->easy);
  http_pair_put(p);
  if (s->list) http_lc.slist_free_all(s->list);
}
#else
static int http_stream_curl(HttpStream *s) { (void)s; return 0; }
static V http_stream_wait(HttpStream *s, int head, V held, V f, V k) {
  (void)s; (void)head; (void)held; (void)f; (void)k;
  return 0;
}
static void http_stream_release(HttpStream *s) { (void)s; }
#endif

static void http_stream_let_go(void *p) {
  HttpStream *s = p;
  http_stream_release(s);
  if (s->file) fclose(s->file);
  if (s->bodyf[0]) remove(s->bodyf);
  http_req_free(&s->q);
  heff_drop(s->rq);
  free(s->head.s);
  free(s->got.s);
  free(s->why.s);
  free(s);
}

static V heff_provider_http(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Send: {
    V f = heff_dup(heff_field(p, 1));
    HttpReq q;
    HttpGot g = {0};
    int ok = http_req(heff_field(p, 0), &q);
    // (in a group of the reactor's, pending while the other threads go on)
    if (ok && heff_group) {
      V r = http_transfer(&q, p, f, k);
      if (r) return r;
    }
    if (!ok) {
      g.code = HTTP_BAD_ARGUMENT;
      buf_str(&g.why, "A libcurl function was given a bad argument");
    } else if (!http_curl(&q, &g)) {
      g.head.n = g.body.n = g.why.n = 0;
      http_spawned(&q, &g, NULL);
    }
    V r = http_answer(&g, q.url);
    http_req_free(&q);
    // (the request's body read: let go of)
    heff_drop(p);
    V x = heff_answer(f, r);
    return heff_tail(k, 1, &x);
  }
  case TAG_Open: {
    V f = heff_dup(heff_field(p, 1));
    V rq = heff_dup(heff_field(p, 0));
    heff_drop(p);
    HttpStream *s = heff_alloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->rq = rq;
    V r;
    if (!http_req(rq, &s->q)) {
      const char *why = "A libcurl function was given a bad argument";
      r = http_stream_error(s, HTTP_BAD_ARGUMENT, why, strlen(why));
      http_stream_let_go(s);
    } else {
      if (!http_stream_curl(s)) http_stream_spawned(s);
      V v = heff_foreign(s, http_stream_let_go);
      V pending = http_stream_wait(s, 1, v, f, k);
      if (pending) return pending;
      r = http_opened(s, v);
    }
    V x = heff_answer(f, r);
    return heff_tail(k, 1, &x);
  }
  case TAG_Piece: {
    V f = heff_dup(heff_field(p, 1));
    HttpStream *s = heff_foreign_ptr(heff_field(p, 0));
    V r;
    if (!http_stream_take(s)) {
      const char *why = "A streamed response's next piece is asked for while it is being read";
      r = http_stream_error(s, HTTP_FAILED_INIT, why, strlen(why));
    } else {
      V pending = http_stream_wait(s, 0, p, f, k);
      if (pending) return pending;
      r = http_piece(s);
      http_stream_give(s);
    }
    heff_drop(p);
    V x = heff_answer(f, r);
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of http");
    return 0;
  }
}
