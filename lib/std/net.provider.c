// The network's side of lib/std/net.hf, for the C backend: an effect
// provider (the shape of one is said in lib/std/io.provider.c).  What is
// at a `data:` URL is read from it; at any other, libcurl fetches it, in
// the process, its connection kept for the next fetch -- or, where there
// is no libcurl, `curl` (on the PATH of every system heff runs on),
// spawned for each.  As Node.js's fetch, a failure is its message, not
// the page: the same message either way, as curl's own tool says it.

static int net_hex(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int net_b64(char c) {
  return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
}

// (a `data:[type][;base64],payload` URL's text, its escapes undone)
static int net_data(const char *u, Buf *b) {
  const char *comma = strchr(u, ',');
  if (!comma) return 0;
  int base64 = 0;
  for (const char *c = u + 5; c + 7 <= comma; c++)
    if (strncmp(c, ";base64", 7) == 0 && (c + 7 == comma || c[7] == ';')) base64 = 1;
  Buf raw = {0};
  for (const char *c = comma + 1; *c; c++) {
    int a, d;
    if (*c == '%' && (a = net_hex(c[1])) >= 0 && (d = net_hex(c[2])) >= 0) {
      char x = (char)(a * 16 + d);
      buf_put(&raw, &x, 1);
      c += 2;
    } else buf_put(&raw, c, 1);
  }
  if (!base64) {
    buf_put(b, raw.s ? raw.s : "", raw.n);
    free(raw.s);
    return 1;
  }
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < raw.n; i++) {
    int v = net_b64(raw.s[i]);
    if (v < 0) continue;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      char x = (char)((acc >> bits) & 0xFF);
      buf_put(b, &x, 1);
    }
  }
  free(raw.s);
  return 1;
}

// (what is at `u`, by a `curl` spawned: 1 and the body in `b`, or 0 and
// what curl said, its last line ends taken off)
static int net_spawned(const char *u, Buf *b) {
  Buf err = {0};
  char *as[] = { "curl", "-sSL", "--fail", "--", (char *)u, NULL };
  int64_t status = heff_capture(as, b, &err);
  if (status != 0) {
    b->n = 0;
    while (err.n && (err.s[err.n - 1] == '\n' || err.s[err.n - 1] == '\r')) err.n--;
    if (err.n) buf_put(b, err.s, err.n);
  }
  free(err.s);
  return status == 0;
}

// libcurl in the process
// ---------------------------------------------------------------------
//
// Found when the program first fetches (dlopen), not linked: no header
// of it, no `-lcurl`, so a program builds as any other (`heff build`
// links `-lm` alone), and runs where there is no libcurl -- a static
// musl program, Windows -- by the spawned curl.  Whatever of it is not
// there, or fails before a request is made, the spawned curl does that
// request (and, libcurl not loaded, every one).  `-DHEFF_NET_SPAWN`:
// always the spawned curl (test/backends.mjs holds the two to each
// other).
//
// Each thread keeps an easy handle of its own, made at its first fetch,
// whose connections libcurl keeps open for the next: what makes a fetch
// some 40 microseconds where a spawned curl is some 6 milliseconds.
// It is set up as `curl -sSL --fail` sets its own (following redirects,
// fifty at most; a status of 400 or more a failure; its user agent), and
// a failure said as the tool says it, `curl: (CODE) why`.  Threads
// (`HEFF_THREADS`): libcurl made ready once (`pthread_once`), each
// thread's handle kept under a key, let go of when the thread ends;
// otherwise a static.  A handle a process was forked with is its
// parent's -- its connections the parent's sockets -- and is left as it
// is, never used nor cleaned up (which could write on them): the child
// makes its own.

#if !defined(_WIN32) && !defined(HEFF_NET_SPAWN)
#include <dlfcn.h>
#define NET_CURL 1
#endif

#ifdef NET_CURL
// (libcurl's ABI, as curl/curl.h has it: these numbers never change)
#define NET_GLOBAL_DEFAULT 3L
#define NET_OPT_FAILONERROR 45
#define NET_OPT_FOLLOWLOCATION 52
#define NET_OPT_MAXREDIRS 68
#define NET_OPT_BUFFERSIZE 98
#define NET_OPT_NOSIGNAL 99
#define NET_OPT_TCP_KEEPALIVE 213
#define NET_OPT_WRITEDATA 10001
#define NET_OPT_URL 10002
#define NET_OPT_ERRORBUFFER 10010
#define NET_OPT_USERAGENT 10018
#define NET_OPT_WRITEFUNCTION 20011
#define NET_INFO_CONTENT_LENGTH_DOWNLOAD_T (0x600000 + 15)
#define NET_ERROR_SIZE 256

// (a body said to be no longer than this is given room for all of it at
// once; a longer one grows as it comes, so that a length a server only
// says is not memory taken)
#define NET_ROOM_MAX ((int64_t)1 << 28)

static struct {
  int (*global_init)(long);
  void *(*easy_init)(void);
  void (*easy_cleanup)(void *);
  int (*setopt)(void *, int, ...);
  int (*perform)(void *);
  const char *(*strerror)(int);
  int (*getinfo)(void *, int, ...);   // (may be missing)
  const char *(*version)(void);       // (may be missing)
} net_lc;
static int net_lc_ok;
// (the user agent curl's tool says, `curl/VERSION`; "" where unknown)
static char net_agent[64];

// (a thread's handle, the process it was made in, and where libcurl says
// why a request failed)
typedef struct {
  void *h;
  pid_t pid;
  char err[NET_ERROR_SIZE];
} NetEasy;

#ifdef HEFF_THREADS
static pthread_once_t net_once = PTHREAD_ONCE_INIT;
static pthread_key_t net_key;

static void net_let_go(void *p) {
  NetEasy *e = p;
  if (e->h && e->pid == getpid()) net_lc.easy_cleanup(e->h);
  free(e);
}
#else
static NetEasy net_easy;
#endif

// (the library found and its functions taken, once; net_lc_ok if all
// that is needed is there and it is ready)
static void net_load(void) {
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
#define NET_SYM(f, name) (*(void **)&net_lc.f = dlsym(lib, name))
  NET_SYM(global_init, "curl_global_init");
  NET_SYM(easy_init, "curl_easy_init");
  NET_SYM(easy_cleanup, "curl_easy_cleanup");
  NET_SYM(setopt, "curl_easy_setopt");
  NET_SYM(perform, "curl_easy_perform");
  NET_SYM(strerror, "curl_easy_strerror");
  NET_SYM(getinfo, "curl_easy_getinfo");
  NET_SYM(version, "curl_version");
#undef NET_SYM
  if (!net_lc.global_init || !net_lc.easy_init || !net_lc.easy_cleanup || !net_lc.setopt
      || !net_lc.perform || !net_lc.strerror)
    return;
#ifdef HEFF_THREADS
  if (pthread_key_create(&net_key, net_let_go) != 0) return;
#endif
  if (net_lc.global_init(NET_GLOBAL_DEFAULT) != 0) return;
  // ("libcurl/8.7.1 SecureTransport ...": its version, as the tool's is)
  const char *v = net_lc.version ? net_lc.version() : NULL;
  if (v && strncmp(v, "libcurl/", 8) == 0) {
    size_t n = strcspn(v + 8, " ");
    if (n && n < sizeof net_agent - 6) snprintf(net_agent, sizeof net_agent, "curl/%.*s", (int)n, v + 8);
  }
  net_lc_ok = 1;
}

// (this thread's handle, made ready -- or NULL, libcurl not to be used)
static NetEasy *net_mine(void) {
#ifdef HEFF_THREADS
  pthread_once(&net_once, net_load);
  if (!net_lc_ok) return NULL;
  NetEasy *e = pthread_getspecific(net_key);
  if (!e) {
    e = calloc(1, sizeof *e);
    if (!e) return NULL;
    if (pthread_setspecific(net_key, e) != 0) { free(e); return NULL; }
  }
#else
  static int loaded;
  if (!loaded) { loaded = 1; net_load(); }
  if (!net_lc_ok) return NULL;
  NetEasy *e = &net_easy;
#endif
  pid_t me = getpid();
  if (e->h && e->pid == me) return e;
  // (none yet, or the parent's: one of this process's own)
  e->h = NULL;
  void *h = net_lc.easy_init();
  if (!h) return NULL;
  if (net_lc.setopt(h, NET_OPT_FOLLOWLOCATION, 1L) != 0
      || net_lc.setopt(h, NET_OPT_FAILONERROR, 1L) != 0
      || net_lc.setopt(h, NET_OPT_ERRORBUFFER, e->err) != 0) {
    net_lc.easy_cleanup(h);
    return NULL;
  }
  // (as the tool has them; what an older libcurl has not, it goes without)
  net_lc.setopt(h, NET_OPT_MAXREDIRS, 50L);
  net_lc.setopt(h, NET_OPT_BUFFERSIZE, 102400L);
  net_lc.setopt(h, NET_OPT_TCP_KEEPALIVE, 1L);
  net_lc.setopt(h, NET_OPT_NOSIGNAL, 1L);
  if (net_agent[0]) net_lc.setopt(h, NET_OPT_USERAGENT, net_agent);
  e->h = h;
  e->pid = me;
  return e;
}

// (where a body is gathered: `b`, given room for all of it at its first
// part where its length is said)
typedef struct {
  Buf *b;
  void *h;
  int sized;
} NetSink;

static size_t net_sink(char *p, size_t size, size_t n, void *u) {
  NetSink *s = u;
  size_t len = size * n;
  if (!s->sized) {
    s->sized = 1;
    int64_t whole = -1;
    Buf *b = s->b;
    if (net_lc.getinfo && net_lc.getinfo(s->h, NET_INFO_CONTENT_LENGTH_DOWNLOAD_T, &whole) == 0
        && whole > 0 && whole <= NET_ROOM_MAX && b->n + (size_t)whole + 1 > b->cap) {
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

// (what is at `u`, by libcurl: 1 and the body in `b`; 0 and why, as the
// tool says it; -1, nothing done, where libcurl is not to be used)
static int net_curl(const char *u, Buf *b) {
  NetEasy *e = net_mine();
  if (!e) return -1;
  NetSink sink = { b, e->h, 0 };
  if (net_lc.setopt(e->h, NET_OPT_URL, u) != 0
      || net_lc.setopt(e->h, NET_OPT_WRITEFUNCTION, net_sink) != 0
      || net_lc.setopt(e->h, NET_OPT_WRITEDATA, &sink) != 0)
    return -1;
  e->err[0] = 0;
  int r = net_lc.perform(e->h);
  // (the sink is this call's: not left where the handle points)
  net_lc.setopt(e->h, NET_OPT_WRITEDATA, (void *)NULL);
  if (r == 0) return 1;
  b->n = 0;
  const char *why = e->err[0] ? e->err : net_lc.strerror(r);
  char m[NET_ERROR_SIZE + 32];
  snprintf(m, sizeof m, "curl: (%d) %s", r, why ? why : "");
  buf_str(b, m);
  return 0;
}
#else
static int net_curl(const char *u, Buf *b) { (void)u; (void)b; return -1; }
#endif

static V net_fetch(const char *u) {
  Buf b = {0};
  if (strncmp(u, "data:", 5) == 0) {
    if (!net_data(u, &b)) buf_str(&b, "TypeError: fetch failed");
  } else {
    int r = net_curl(u, &b);
    if (r < 0) {
      b.n = 0;
      r = net_spawned(u, &b);
    }
    if (!r) {
      Buf e = {0};
      buf_str(&e, "TypeError: fetch failed");
      if (b.n) {
        buf_str(&e, ": ");
        buf_put(&e, b.s, b.n);
      }
      free(b.s);
      b = e;
    }
  }
  V s = heff_str_bytes(b.s ? b.s : "", (int64_t)b.n);
  free(b.s);
  return s;
}

static V heff_provider_net(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Fetch: {
    char *u = heff_cstr_text(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V t = u ? net_fetch(u) : heff_str_text("TypeError: fetch failed");
    free(u);
    V x = heff_answer(f, t);
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of net");
    return 0;
  }
}
