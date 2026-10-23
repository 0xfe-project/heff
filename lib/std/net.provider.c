// TCP sockets' side of std.net, for the C backend: an effect
// provider.  A socket is a `NetSock`, the host's own: a foreign value
// the program's `with` holds, closed by its `Release` when the body
// ends, or where the program drops its last reference to it -- a body
// left by a throw or a dropped continuation -- as the value's function
// letting it go.  An operation that waits keeps its operation, and so
// the socket, until it is done; one given a socket closed is `EBADF`.
//
// Each operation is a `NetOp`, done in steps: its wait's `step` does
// what can be done now, without waiting, and says it is done
// (HEFF_STEP_DONE, its answer or its errno in the op) or what it waits for
// (HEFF_STEP_WAIT: `fd` readable or writable, `events`), to be stepped again
// once it is.  So every socket is non-blocking, and an operation waits
// only in the runtime's reactor: where no scheduler runs, there and then, the whole
// program waiting with it; where std.reactor's green threads run, the
// op pending, the other threads going on, and its own once it is done.
// The heff side is the same either way: each operation gives its
// answer to a function.  A connection made, a read and a write may have
// a time limit: their wait's deadline, kept either way (the reactor's,
// or the poll's where there is none), the op done `ETIMEDOUT` where it
// passes first.  A name resolved by getaddrinfo waits with no
// descriptor: under a scheduler, a host that is no address is resolved
// by a thread of its own, which says it is done by a descriptor written;
// without one, it is resolved where the program is, not cut short.

#ifdef _WIN32

// (no sockets on Windows yet: an operation fails the program)
static V heff_provider_net(V p, V k) {
  heff_drop(p);
  heff_drop(k);
  heff_fail("std.net: no sockets on Windows yet");
  return 0;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>

// (this provider's sockets, told from what other providers hand out)
#define NET_MAGIC 0x6e657473u

typedef struct {
  uint32_t magic;
  int fd;              // (-1 once it is closed)
  char *address;       // (where it is, as a failure names it: the host and
                       //  port it was given, or an accepted one's other side)
} NetSock;

// (the socket closed, once: what waits on it told so, not polled
// closed; 0, or -1 with errno)
static int net_close(NetSock *s) {
  if (s->fd < 0) return 0;
  heff_reactor_closing(s->fd);
  int r = close(s->fd);
  s->fd = -1;
  return r;
}

// (the program letting go of its last reference: closed, unless the
// `with` closed it)
static void net_let_go(void *x) {
  NetSock *s = x;
  net_close(s);
  s->magic = 0;
  free(s->address);
  free(s);
}

// (the socket a foreign value is, or the program failed: it is none)
static NetSock *net_sock(V v) {
  NetSock *s = heff_foreign_ptr(v);
  if (s->magic != NET_MAGIC) heff_fail("std.net: no socket");
  return s;
}

// (a descriptor made non-blocking, not to be given to a program run;
// 0, or -1 with errno)
static int net_ready_fd(int fd) {
  int fl = fcntl(fd, F_GETFL);
  if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
  int fd_fl = fcntl(fd, F_GETFD);
  if (fd_fl < 0 || fcntl(fd, F_SETFD, fd_fl | FD_CLOEXEC) < 0) return -1;
#ifdef SO_NOSIGPIPE
  // (a write to a connection closed an error, EPIPE, not a signal)
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  return 0;
}

#ifdef MSG_NOSIGNAL
#define NET_SEND_FLAGS MSG_NOSIGNAL
#else
#define NET_SEND_FLAGS 0
#endif

// Errors
// ---------------------------------------------------------------------

// (an errno's name, the same on every system where its number is not)
static const char *net_errno_name(int e) {
  switch (e) {
#define NET_E(x) case x: return #x;
  NET_E(EACCES) NET_E(EADDRINUSE) NET_E(EADDRNOTAVAIL) NET_E(EAFNOSUPPORT)
  NET_E(EAGAIN) NET_E(EBADF) NET_E(ECONNABORTED) NET_E(ECONNREFUSED)
  NET_E(ECONNRESET) NET_E(EHOSTUNREACH) NET_E(EINTR) NET_E(EINVAL) NET_E(EIO)
  NET_E(EISCONN) NET_E(EMFILE) NET_E(ENETDOWN) NET_E(ENETRESET)
  NET_E(ENETUNREACH) NET_E(ENFILE) NET_E(ENOBUFS) NET_E(ENOMEM) NET_E(ENOSYS)
  NET_E(ENOTCONN) NET_E(ENOTSOCK) NET_E(EPERM) NET_E(EPIPE) NET_E(EPROTO)
  NET_E(EPROTONOSUPPORT) NET_E(ETIMEDOUT)
#ifdef EHOSTDOWN
  NET_E(EHOSTDOWN)
#endif
#undef NET_E
  default: return "E?";
  }
}

// (getaddrinfo's code's name)
static const char *net_gai_name(int e) {
  switch (e) {
#define NET_E(x) case x: return #x;
  NET_E(EAI_AGAIN) NET_E(EAI_BADFLAGS) NET_E(EAI_FAIL) NET_E(EAI_FAMILY)
  NET_E(EAI_MEMORY) NET_E(EAI_NONAME) NET_E(EAI_SERVICE) NET_E(EAI_SOCKTYPE)
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
  NET_E(EAI_NODATA)
#endif
#if defined(EAI_ADDRFAMILY)
  NET_E(EAI_ADDRFAMILY)
#endif
#undef NET_E
  default: return "EAI_?";
  }
}

static uint32_t net_error_shape[4] = { LBL_code, LBL_name, LBL_message, LBL_address };
static uint32_t net_address_shape[2] = { LBL_host, LBL_port };

// (a `NetError` of getaddrinfo's code `gai`, or where it is 0 or says
// the system's, of errno `err`; of the address `address`)
static V net_error(int err, int gai, const char *address) {
  V f[4];
  if (gai && gai != EAI_SYSTEM) {
    f[0] = heff_int(gai);
    f[1] = heff_str_text(net_gai_name(gai));
    f[2] = heff_str_text(gai_strerror(gai));
  } else {
    const char *name = net_errno_name(err);
    char other[24];
    if (name[1] == '?') { snprintf(other, sizeof other, "errno %d", err); name = other; }
    f[0] = heff_int(err);
    f[1] = heff_str_text(name);
    f[2] = heff_str_text(strerror(err));
  }
  f[3] = heff_str_text(address ? address : "");
  return heff_rec(4, net_error_shape, f);
}

static V net_left(int err, int gai, const char *address) {
  V x = net_error(err, gai, address);
  return heff_tag_n(TAG_Left, 1, &x);
}

static V net_right(V x) { return heff_tag_n(TAG_Right, 1, &x); }

// (a socket made, the host's own, at `address`, taken: `Right` a foreign
// value, closed where the program lets go of it)
static V net_value(int fd, char *address) {
  NetSock *s = heff_alloc(sizeof *s);
  s->magic = NET_MAGIC;
  s->fd = fd;
  s->address = address;
  return net_right(heff_foreign(s, net_let_go));
}

// (a host and a port as a failure names them, as a URL has them -- an
// IPv6 host in brackets: malloc'd)
static char *net_named(const char *host, const char *port) {
  size_t n = strlen(host) + strlen(port) + 4;
  char *a = heff_alloc(n);
  snprintf(a, n, strchr(host, ':') ? "[%s]:%s" : "%s:%s", host, port);
  return a;
}

// (the other side of the connection `fd`, as a failure names it)
static char *net_peer_named(int fd) {
  struct sockaddr_storage ss;
  socklen_t n = sizeof ss;
  char host[1025], port[32];  // (NI_MAXHOST, NI_MAXSERV)
  if (getpeername(fd, (struct sockaddr *)&ss, &n) < 0
      || getnameinfo((struct sockaddr *)&ss, n, host, sizeof host, port, sizeof port, NI_NUMERICHOST | NI_NUMERICSERV) != 0)
    return heff_strdup("");
  return net_named(host, port);
}

// An operation, in steps
// ---------------------------------------------------------------------

enum { NET_CONNECT, NET_LISTEN, NET_ACCEPT, NET_READ, NET_WRITE };

typedef struct NetLookup NetLookup;

typedef struct NetOp NetOp;
struct NetOp {
  HeffWait w;          // (first: an op is its wait -- its `fd`, at
                       //  HEFF_STEP_DONE, the socket made for a connection or a
                       //  listener, else -1)
  int what;
  int err, gai;        // (why it failed: errno, getaddrinfo's code)
  // a connection made, a socket listening
  struct addrinfo *ais, *ai;
  int backlog;
  int (*then)(HeffWait *);  // (its step once its name is resolved)
  NetLookup *lookup;
  // accept, read, write
  uint8_t *buf;
  const uint8_t *data;
  int64_t n, done;
  V held;              // (what the op holds: its operation, and so the socket
                       //  it is given -- not let go of under it -- and a
                       //  write's bytes)
  char *address;       // (its socket's, or the host and port it is given,
                       //  as a failure names them: malloc'd)
};

// (an op, done within `ms` milliseconds from now where they are more
// than 0)
static NetOp *net_op(int what, int64_t ms) {
  NetOp *o = heff_alloc(sizeof *o);
  memset(o, 0, sizeof *o);
  o->what = what;
  o->w.fd = -1;
  o->w.until = ms > 0 ? heff_now_ms() + ms : 0;
  o->n = -1;
  return o;
}

// (the addresses of `host`'s `port`, for a connection or, `passive`, to
// listen on -- `numeric`, only where the host is written as an address,
// EAI_NONAME where it is not: 0, or getaddrinfo's code (or EAI_SYSTEM,
// errno))
static int net_resolve(NetOp *o, V host, int64_t port, int passive, int numeric) {
  if (port < 0 || port > 65535) { o->err = EINVAL; return EAI_SYSTEM; }
  char *h = heff_cstr_text(host);
  if (!h) { o->err = EINVAL; return EAI_SYSTEM; }
  char service[8];
  snprintf(service, sizeof service, "%d", (int)port);
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0) | (numeric ? AI_NUMERICHOST : 0);
  int r = getaddrinfo(passive && !*h ? NULL : h, service, &hints, &o->ais);
  if (r == EAI_SYSTEM) o->err = errno;
  free(h);
  o->ai = o->ais;
  return r;
}

// (a new socket of the address `ai`'s, ready: -1 with errno, or it)
static int net_socket(struct addrinfo *ai) {
  int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd < 0) return -1;
  if (net_ready_fd(fd) < 0) {
    int e = errno;
    close(fd);
    errno = e;
    return -1;
  }
  return fd;
}

// A connection: each address tried in turn, the last one's error the
// op's where none connects.
static int net_connect_step(HeffWait *w) {
  NetOp *o = (NetOp *)w;
  if (w->fd >= 0) {
    // (the connection begun is made, or why not)
    int e = 0;
    socklen_t n = sizeof e;
    if (getsockopt(w->fd, SOL_SOCKET, SO_ERROR, &e, &n) < 0) e = errno;
    if (e == 0) return HEFF_STEP_DONE;
    o->err = e;
    close(w->fd);
    w->fd = -1;
    o->ai = o->ai->ai_next;
  }
  for (; o->ai; o->ai = o->ai->ai_next) {
    int fd = net_socket(o->ai);
    if (fd < 0) { o->err = errno; continue; }
    int r;
    do r = connect(fd, o->ai->ai_addr, o->ai->ai_addrlen); while (r < 0 && errno == EINTR);
    if (r == 0) { w->fd = fd; return HEFF_STEP_DONE; }
    if (errno == EINPROGRESS) {
      w->fd = fd;
      w->events = POLLOUT;
      return HEFF_STEP_WAIT;
    }
    o->err = errno;
    close(fd);
  }
  return HEFF_STEP_DONE;
}

// A listener: the first address bound and listened on.
static int net_listen_step(HeffWait *w) {
  NetOp *o = (NetOp *)w;
  for (; o->ai; o->ai = o->ai->ai_next) {
    int fd = net_socket(o->ai);
    if (fd < 0) { o->err = errno; continue; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, o->ai->ai_addr, o->ai->ai_addrlen) == 0 && listen(fd, o->backlog) == 0) {
      w->fd = fd;
      return HEFF_STEP_DONE;
    }
    o->err = errno;
    close(fd);
  }
  return HEFF_STEP_DONE;
}

// (the listener's descriptor stepped on, and the socket made: in `o->n`)
static int net_accept_step(HeffWait *w) {
  NetOp *o = (NetOp *)w;
  for (;;) {
    int c = accept(w->fd, NULL, NULL);
    if (c >= 0) {
      if (net_ready_fd(c) < 0) {
        o->err = errno;
        close(c);
        return HEFF_STEP_DONE;
      }
      o->n = c;
      return HEFF_STEP_DONE;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      w->events = POLLIN;
      return HEFF_STEP_WAIT;
    }
    o->err = errno;
    return HEFF_STEP_DONE;
  }
}

static int net_read_step(HeffWait *w) {
  NetOp *o = (NetOp *)w;
  for (;;) {
    ssize_t r = recv(w->fd, o->buf, (size_t)o->n, 0);
    if (r >= 0) {
      o->done = r;
      return HEFF_STEP_DONE;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      w->events = POLLIN;
      return HEFF_STEP_WAIT;
    }
    o->err = errno;
    return HEFF_STEP_DONE;
  }
}

static int net_write_step(HeffWait *w) {
  NetOp *o = (NetOp *)w;
  while (o->done < o->n) {
    ssize_t r = send(w->fd, o->data + o->done, (size_t)(o->n - o->done), NET_SEND_FLAGS);
    if (r >= 0) {
      o->done += r;
      continue;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      w->events = POLLOUT;
      return HEFF_STEP_WAIT;
    }
    o->err = errno;
    return HEFF_STEP_DONE;
  }
  return HEFF_STEP_DONE;
}

// (a read's most, a mebibyte)
#define NET_READ_MAX (1 << 20)

// A name resolved by a thread of its own
// ---------------------------------------------------------------------
//
// getaddrinfo waits with no descriptor to poll: where a scheduler runs,
// a host that is no address written as it is (`localhost`, a name) is
// resolved by a thread of the system started for it, which says it is
// done by a byte written to a socket of a pair whose other end the
// reactor polls.  The thread touches nothing of heff's; the lookup is
// let go of by the last of it and its op (an op let go of undone -- its
// group closed -- leaves the thread to end by itself).

struct NetLookup {
  char *host;          // (NULL: all of the host's, to listen on)
  char service[8];
  int passive;
  struct addrinfo *ais;
  int r, err;
  int ends[2];         // (the op's end, polled, and the thread's, written to)
  int refs;
};

static void net_lookup_let_go(NetLookup *l) {
  if (__atomic_sub_fetch(&l->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
  if (l->ais) freeaddrinfo(l->ais);
  close(l->ends[0]);
  close(l->ends[1]);
  free(l->host);
  free(l);
}

static void *net_lookup_run(void *u) {
  NetLookup *l = u;
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV | (l->passive ? AI_PASSIVE : 0);
  l->r = getaddrinfo(l->host, l->service, &hints, &l->ais);
  if (l->r == EAI_SYSTEM) l->err = errno;
  ssize_t s;
  do s = send(l->ends[1], "", 1, NET_SEND_FLAGS); while (s < 0 && errno == EINTR);
  net_lookup_let_go(l);
  return NULL;
}

// (the name resolved: its addresses the op's, and its next step taken)
static int net_resolved_step(HeffWait *w) {
  NetOp *o = (NetOp *)w;
  NetLookup *l = o->lookup;
  char c;
  ssize_t r = recv(l->ends[0], &c, 1, 0);
  if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return HEFF_STEP_WAIT;
  o->ais = l->ais;
  o->ai = o->ais;
  l->ais = NULL;
  o->gai = r == 1 ? l->r : EAI_SYSTEM;
  o->err = r == 1 ? l->err : (r < 0 ? errno : EIO);
  o->lookup = NULL;
  net_lookup_let_go(l);
  w->fd = -1;
  if (o->gai) return HEFF_STEP_DONE;
  w->step = o->then;
  return w->step(w);
}

// (`host`'s `port` resolved by a thread of its own, the op waiting for
// it; 0, or -1 where no thread could be started)
static int net_lookup(NetOp *o, V host, int64_t port, int passive) {
  NetLookup *l = heff_alloc(sizeof *l);
  memset(l, 0, sizeof *l);
  char *h = heff_cstr_text(host);
  l->host = h && *h ? h : NULL;
  if (h && !*h) free(h);
  snprintf(l->service, sizeof l->service, "%d", (int)port);
  l->passive = passive;
  l->refs = 2;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, l->ends) < 0) {
    free(l->host);
    free(l);
    return -1;
  }
  net_ready_fd(l->ends[0]);
  net_ready_fd(l->ends[1]);
  pthread_attr_t attr;
  pthread_t t;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int r = pthread_create(&t, &attr, net_lookup_run, l);
  pthread_attr_destroy(&attr);
  if (r != 0) {
    close(l->ends[0]);
    close(l->ends[1]);
    free(l->host);
    free(l);
    return -1;
  }
  o->lookup = l;
  o->w.fd = l->ends[0];
  o->w.events = POLLIN;
  o->w.step = net_resolved_step;
  return 0;
}

// Addresses
// ---------------------------------------------------------------------

// (`Right` the address `sa`, numeric, or `Left` why not)
static V net_address(struct sockaddr *sa, socklen_t n) {
  char host[1025], port[32];  // (NI_MAXHOST, NI_MAXSERV)
  int r = getnameinfo(sa, n, host, sizeof host, port, sizeof port, NI_NUMERICHOST | NI_NUMERICSERV);
  if (r != 0) return net_left(errno, r, "");
  V f[2] = { heff_str_text(host), heff_int(strtol(port, NULL, 10)) };
  return net_right(heff_rec(2, net_address_shape, f));
}

// (where the socket `s` is: the other side's, `peer`, or this side's)
static V net_where(NetSock *s, int peer) {
  struct sockaddr_storage ss;
  socklen_t n = sizeof ss;
  int fd = s->fd;
  int r = peer ? getpeername(fd, (struct sockaddr *)&ss, &n) : getsockname(fd, (struct sockaddr *)&ss, &n);
  if (r < 0) return net_left(errno, 0, s->address);
  return net_address((struct sockaddr *)&ss, n);
}

// An op's answer, and an op let go of
// ---------------------------------------------------------------------

// (what the op holds, let go of, and it: not the socket it made)
static void net_op_free(NetOp *o) {
  if (o->ais) freeaddrinfo(o->ais);
  if (o->lookup) net_lookup_let_go(o->lookup);
  free(o->buf);
  if (o->held) heff_drop(o->held);
  free(o->address);
  free(o);
}

static V net_answer(HeffWait *w) {
  NetOp *o = (NetOp *)w;
  // (why waiting failed, where it did: the op's error)
  int err = w->err ? w->err : o->err;
  V r;
  switch (o->what) {
  case NET_CONNECT:
  case NET_LISTEN:
    if (w->err && w->fd >= 0 && !o->lookup) {
      // (a connection begun, not made: timed out, or polled in vain; a
      // name being resolved is its lookup's, let go of below)
      close(w->fd);
      w->fd = -1;
    }
    if (o->gai && !w->err) r = net_left(err, o->gai, o->address);
    else if (w->err) r = net_left(w->err, 0, o->address);
    else if (w->fd >= 0) r = net_value(w->fd, heff_strdup(o->address));
    else r = net_left(err ? err : EADDRNOTAVAIL, 0, o->address);
    break;
  case NET_ACCEPT:
    r = !w->err && o->n >= 0 ? net_value((int)o->n, net_peer_named((int)o->n)) : net_left(err, 0, o->address);
    break;
  case NET_READ:
    r = err ? net_left(err, 0, o->address) : net_right(heff_bytes_in(o->buf, o->done));
    break;
  default:
    r = err ? net_left(err, 0, o->address) : net_right(NTAG(TAG_MkUnit));
    break;
  }
  net_op_free(o);
  return r;
}

static void net_cancel(HeffWait *w) {
  NetOp *o = (NetOp *)w;
  // (a socket being connected, or listened on, is the op's own)
  if ((o->what == NET_CONNECT || o->what == NET_LISTEN) && !o->lookup && w->fd >= 0) close(w->fd);
  net_op_free(o);
}

// (the op, its first step taken: done, its answer given to `k`; or
// waited for, here or by the reactor -- `heff_outside`)
static V net_go(NetOp *o, V f, V k) {
  o->w.answer = net_answer;
  o->w.cancel = net_cancel;
  if (o->w.step(&o->w) == HEFF_STEP_WAIT) return heff_outside(&o->w, f, k);
  V x = heff_answer(f, net_answer(&o->w));
  return heff_tail(k, 1, &x);
}

// (a connection made, or a socket listening, to `host`'s `port`: its
// name resolved -- where a scheduler runs and it is no address, by a
// thread of its own -- then `step` taken)
static V net_made(NetOp *o, V host, int64_t port, int passive, int (*step)(HeffWait *), V f, V k) {
  char *h = heff_cstr_text(host), number[24];
  snprintf(number, sizeof number, "%lld", (long long)port);
  o->address = net_named(h ? h : "", number);
  free(h);
  o->then = step;
  o->w.step = step;
  int gai = net_resolve(o, host, port, passive, heff_group != 0);
  if (gai == EAI_NONAME && heff_group && net_lookup(o, host, port, passive) == 0) gai = 0;
  else if (gai == EAI_NONAME && heff_group) gai = net_resolve(o, host, port, passive, 0);
  if (gai) {
    // (not resolved: no step taken)
    o->gai = gai;
    o->w.step = NULL;
    V x = heff_answer(f, net_answer(&o->w));
    return heff_tail(k, 1, &x);
  }
  return net_go(o, f, k);
}

// The operations
// ---------------------------------------------------------------------

// (a number given as an `Int`: -1 where it is beyond a word's)
static int64_t net_int(V v) { return IS_INT(v) ? INT_OF(v) : -1; }

// (an operation given a socket its `with` closed, `s`: `EBADF`)
static V net_closed(V p, NetSock *s, V f, V k) {
  V r = net_left(EBADF, 0, s->address);
  heff_drop(p);
  V x = heff_answer(f, r);
  return heff_tail(k, 1, &x);
}

static V heff_provider_net(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Connect: {
    V f = heff_dup(heff_field(p, 3));
    V host = heff_dup(heff_field(p, 0));
    int64_t port = net_int(heff_field(p, 1));
    int64_t ms = net_int(heff_field(p, 2));
    heff_drop(p);
    NetOp *o = net_op(NET_CONNECT, ms);
    V r = net_made(o, host, port, 0, net_connect_step, f, k);
    heff_drop(host);
    return r;
  }
  case TAG_Listen: {
    V f = heff_dup(heff_field(p, 3));
    V host = heff_dup(heff_field(p, 0));
    int64_t port = net_int(heff_field(p, 1));
    int64_t backlog = net_int(heff_field(p, 2));
    heff_drop(p);
    NetOp *o = net_op(NET_LISTEN, 0);
    o->backlog = backlog < 1 ? 1 : backlog > 65535 ? 65535 : (int)backlog;
    V r = net_made(o, host, port, 1, net_listen_step, f, k);
    heff_drop(host);
    return r;
  }
  // (an operation given a socket: `p` is kept while it waits, and so the
  // socket; one closed is `EBADF`)
  case TAG_Accept: {
    NetSock *s = net_sock(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    if (s->fd < 0) return net_closed(p, s, f, k);
    NetOp *o = net_op(NET_ACCEPT, 0);
    o->address = heff_strdup(s->address);
    o->held = p;
    o->w.fd = s->fd;
    o->w.step = net_accept_step;
    return net_go(o, f, k);
  }
  case TAG_Read: {
    NetSock *s = net_sock(heff_field(p, 0));
    int64_t n = net_int(heff_field(p, 1));
    int64_t ms = net_int(heff_field(p, 2));
    V f = heff_dup(heff_field(p, 3));
    if (s->fd < 0) return net_closed(p, s, f, k);
    if (n <= 0) {
      heff_drop(p);
      V x = heff_answer(f, net_right(heff_bytes_in("", 0)));
      return heff_tail(k, 1, &x);
    }
    NetOp *o = net_op(NET_READ, ms);
    o->address = heff_strdup(s->address);
    o->n = n > NET_READ_MAX ? NET_READ_MAX : n;
    o->buf = heff_alloc((size_t)o->n);
    o->held = p;
    o->w.fd = s->fd;
    o->w.step = net_read_step;
    return net_go(o, f, k);
  }
  case TAG_Write: {
    NetSock *s = net_sock(heff_field(p, 0));
    int64_t ms = net_int(heff_field(p, 2));
    V f = heff_dup(heff_field(p, 3));
    if (s->fd < 0) return net_closed(p, s, f, k);
    NetOp *o = net_op(NET_WRITE, ms);
    o->address = heff_strdup(s->address);
    // (the bytes are `p`'s, held until they are written)
    o->data = heff_bytes_data(heff_field(p, 1), &o->n);
    o->held = p;
    o->w.fd = s->fd;
    o->w.step = net_write_step;
    return net_go(o, f, k);
  }
  case TAG_Shutdown: {
    NetSock *s = net_sock(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    if (s->fd < 0) return net_closed(p, s, f, k);
    V r = shutdown(s->fd, SHUT_WR) < 0 ? net_left(errno, 0, s->address) : net_right(NTAG(TAG_MkUnit));
    heff_drop(p);
    V x = heff_answer(f, r);
    return heff_tail(k, 1, &x);
  }
  // (the `with`'s release: a connection's writing ended -- its other
  // side reads to the end -- and it closed; a listener closed)
  case TAG_Release:
  case TAG_ReleaseListener: {
    NetSock *s = net_sock(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    if (s->fd < 0) return net_closed(p, s, f, k);
    if (heff_tag_of(p) == TAG_Release) shutdown(s->fd, SHUT_WR);
    V r = net_close(s) < 0 ? net_left(errno, 0, s->address) : net_right(NTAG(TAG_MkUnit));
    heff_drop(p);
    V x = heff_answer(f, r);
    return heff_tail(k, 1, &x);
  }
  case TAG_Peer:
  case TAG_ThisSide:
  case TAG_Local: {
    NetSock *s = net_sock(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    if (s->fd < 0) return net_closed(p, s, f, k);
    V r = net_where(s, heff_tag_of(p) == TAG_Peer);
    heff_drop(p);
    V x = heff_answer(f, r);
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of net");
    return 0;
  }
}

#endif
