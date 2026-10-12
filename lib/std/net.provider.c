// TCP sockets' side of lib/std/net.hf, for the C backend: an effect
// provider (the shape of one is said in lib/std/io.provider.c).  A
// socket is a `NetSock` the world holds (lib/std/handle.hf), let go of
// -- closed -- by `Close`; the program has its number.
//
// Each operation is a `NetOp`, done in steps: `step` does what can be
// done now, without waiting, and says it is done (NET_DONE, its answer
// or its errno in the op) or what it waits for (NET_WAIT: `fd` readable
// or writable, `events`), to be stepped again once it is.  So every
// socket is non-blocking, and an operation waits only in `net_wait`,
// which polls its descriptor until the next step can go on -- here, the
// whole program waiting with it.  For green threads (#43, step 4), the
// scheduler takes the op where `net_wait` is called -- its descriptor
// and events among those it polls, the operation's function and the
// continuation kept with it -- and runs other threads; when the
// descriptor is ready it steps the op again, and once it is done hands
// its answer to the continuation, as here.  The heff side is the same
// either way: each operation gives its answer to a function.  (A name
// resolved by getaddrinfo waits with no descriptor: that step would be
// done by a thread of its own, its completion a descriptor written.)

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

// (the world's sockets, told from what other providers hand out)
#define NET_MAGIC 0x6e657473u

typedef struct {
  uint32_t magic;
  int fd;
} NetSock;

static void net_let_go(void *x) {
  NetSock *s = x;
  close(s->fd);
  s->magic = 0;
  free(s);
}

// (the socket numbered `i`, or the program failed: it is none)
static NetSock *net_sock(int64_t i) {
  NetSock *s = heff_held(i);
  if (s->magic != NET_MAGIC) heff_fail("std.net: the handle is no socket");
  return s;
}

// (a handle's number: `MkHandle i`)
static int64_t net_number(V h) { return INT_OF(heff_field(h, 0)); }

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

static uint32_t net_error_shape[3] = { LBL_code, LBL_name, LBL_message };
static uint32_t net_address_shape[2] = { LBL_host, LBL_port };

// (a `NetError` of getaddrinfo's code `gai`, or where it is 0 or says
// the system's, of errno `err`)
static V net_error(int err, int gai) {
  V f[3];
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
  return heff_rec(3, net_error_shape, f);
}

static V net_left(int err, int gai) {
  V x = net_error(err, gai);
  return heff_tag_n(TAG_Left, 1, &x);
}

static V net_right(V x) { return heff_tag_n(TAG_Right, 1, &x); }

// (a socket held by the world: `Right (MkHandle i)`)
static V net_handle(int fd) {
  NetSock *s = heff_alloc(sizeof *s);
  s->magic = NET_MAGIC;
  s->fd = fd;
  V i = MK_INT(heff_hold(s, net_let_go));
  return net_right(heff_tag_n(TAG_MkHandle, 1, &i));
}

// An operation, in steps
// ---------------------------------------------------------------------

enum { NET_DONE, NET_WAIT };

typedef struct NetOp NetOp;
struct NetOp {
  int (*step)(NetOp *);
  int fd;              // (what it waits on, at NET_WAIT; at NET_DONE, the
  short events;        //  socket made, else -1)
  int err, gai;        // (why it failed: errno, getaddrinfo's code)
  // a connection made, a socket listening
  struct addrinfo *ais, *ai;
  int backlog;
  // read, write
  uint8_t *buf;
  const uint8_t *data;
  int64_t n, done;
};

static int net_connect_step(NetOp *o);

// (the op waited for until it is done -- the whole program waiting; for
// green threads, a scheduler's to do instead: see the top)
static void net_wait(NetOp *o) {
  while (o->step(o) == NET_WAIT) {
    struct pollfd p = { o->fd, o->events, 0 };
    if (poll(&p, 1, -1) < 0 && errno != EINTR) {
      o->err = errno;
      // (a connection begun, not made)
      if (o->step == net_connect_step && o->fd >= 0) {
        close(o->fd);
        o->fd = -1;
      }
      return;
    }
  }
}

// (the addresses of `host`'s `port`, for a connection or, `passive`, to
// listen on: 0, or getaddrinfo's code (or EAI_SYSTEM, errno))
static int net_resolve(NetOp *o, V host, int64_t port, int passive) {
  if (port < 0 || port > 65535) { o->err = EINVAL; return EAI_SYSTEM; }
  char *h = heff_cstr_text(host);
  if (!h) { o->err = EINVAL; return EAI_SYSTEM; }
  char service[8];
  snprintf(service, sizeof service, "%d", (int)port);
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);
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
static int net_connect_step(NetOp *o) {
  if (o->fd >= 0) {
    // (the connection begun is made, or why not)
    int e = 0;
    socklen_t n = sizeof e;
    if (getsockopt(o->fd, SOL_SOCKET, SO_ERROR, &e, &n) < 0) e = errno;
    if (e == 0) return NET_DONE;
    o->err = e;
    close(o->fd);
    o->fd = -1;
    o->ai = o->ai->ai_next;
  }
  for (; o->ai; o->ai = o->ai->ai_next) {
    int fd = net_socket(o->ai);
    if (fd < 0) { o->err = errno; continue; }
    int r;
    do r = connect(fd, o->ai->ai_addr, o->ai->ai_addrlen); while (r < 0 && errno == EINTR);
    if (r == 0) { o->fd = fd; return NET_DONE; }
    if (errno == EINPROGRESS) {
      o->fd = fd;
      o->events = POLLOUT;
      return NET_WAIT;
    }
    o->err = errno;
    close(fd);
  }
  return NET_DONE;
}

// A listener: the first address bound and listened on.
static int net_listen_step(NetOp *o) {
  for (; o->ai; o->ai = o->ai->ai_next) {
    int fd = net_socket(o->ai);
    if (fd < 0) { o->err = errno; continue; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, o->ai->ai_addr, o->ai->ai_addrlen) == 0 && listen(fd, o->backlog) == 0) {
      o->fd = fd;
      return NET_DONE;
    }
    o->err = errno;
    close(fd);
  }
  return NET_DONE;
}

// (the descriptor stepped on, and the socket made: in `o->n`)
static int net_accept_step(NetOp *o) {
  for (;;) {
    int c = accept(o->fd, NULL, NULL);
    if (c >= 0) {
      if (net_ready_fd(c) < 0) {
        o->err = errno;
        close(c);
        return NET_DONE;
      }
      o->n = c;
      return NET_DONE;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      o->events = POLLIN;
      return NET_WAIT;
    }
    o->err = errno;
    return NET_DONE;
  }
}

static int net_read_step(NetOp *o) {
  for (;;) {
    ssize_t r = recv(o->fd, o->buf, (size_t)o->n, 0);
    if (r >= 0) {
      o->done = r;
      return NET_DONE;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      o->events = POLLIN;
      return NET_WAIT;
    }
    o->err = errno;
    return NET_DONE;
  }
}

static int net_write_step(NetOp *o) {
  while (o->done < o->n) {
    ssize_t r = send(o->fd, o->data + o->done, (size_t)(o->n - o->done), NET_SEND_FLAGS);
    if (r >= 0) {
      o->done += r;
      continue;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      o->events = POLLOUT;
      return NET_WAIT;
    }
    o->err = errno;
    return NET_DONE;
  }
  return NET_DONE;
}

// (a read's most, a mebibyte)
#define NET_READ_MAX (1 << 20)

// Addresses
// ---------------------------------------------------------------------

// (`Right` the address `sa`, numeric, or `Left` why not)
static V net_address(struct sockaddr *sa, socklen_t n) {
  char host[1025], port[32];  // (NI_MAXHOST, NI_MAXSERV)
  int r = getnameinfo(sa, n, host, sizeof host, port, sizeof port, NI_NUMERICHOST | NI_NUMERICSERV);
  if (r != 0) return net_left(errno, r);
  V f[2] = { heff_str_text(host), heff_int(strtol(port, NULL, 10)) };
  return net_right(heff_rec(2, net_address_shape, f));
}

// (where the socket `fd` is: the other side's, `peer`, or this side's)
static V net_where(int fd, int peer) {
  struct sockaddr_storage ss;
  socklen_t n = sizeof ss;
  int r = peer ? getpeername(fd, (struct sockaddr *)&ss, &n) : getsockname(fd, (struct sockaddr *)&ss, &n);
  if (r < 0) return net_left(errno, 0);
  return net_address((struct sockaddr *)&ss, n);
}

// The operations
// ---------------------------------------------------------------------

// (a number given as an `Int`: -1 where it is beyond a word's)
static int64_t net_int(V v) { return IS_INT(v) ? INT_OF(v) : -1; }

// (a connection made, or a socket listening: its answer)
static V net_made(NetOp *o, int gai) {
  V r;
  if (gai) r = net_left(o->err, gai);
  else if (o->fd >= 0) r = net_handle(o->fd);
  else r = net_left(o->err ? o->err : EADDRNOTAVAIL, 0);
  if (o->ais) freeaddrinfo(o->ais);
  return r;
}

static V heff_provider_net(V p, V k) {
  NetOp o;
  memset(&o, 0, sizeof o);
  o.fd = -1;
  switch (heff_tag_of(p)) {
  case TAG_Connect: {
    V f = heff_dup(heff_field(p, 2));
    int gai = net_resolve(&o, heff_field(p, 0), net_int(heff_field(p, 1)), 0);
    heff_drop(p);
    if (!gai) {
      o.step = net_connect_step;
      net_wait(&o);
    }
    V x = heff_answer(f, net_made(&o, gai));
    return heff_tail(k, 1, &x);
  }
  case TAG_Listen: {
    V f = heff_dup(heff_field(p, 3));
    int64_t backlog = net_int(heff_field(p, 2));
    o.backlog = backlog < 1 ? 1 : backlog > 65535 ? 65535 : (int)backlog;
    int gai = net_resolve(&o, heff_field(p, 0), net_int(heff_field(p, 1)), 1);
    heff_drop(p);
    if (!gai) {
      o.step = net_listen_step;
      net_wait(&o);
    }
    V x = heff_answer(f, net_made(&o, gai));
    return heff_tail(k, 1, &x);
  }
  case TAG_Accept: {
    NetSock *s = net_sock(net_number(heff_field(p, 0)));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    o.fd = s->fd;
    o.n = -1;
    o.step = net_accept_step;
    net_wait(&o);
    V x = heff_answer(f, o.n >= 0 ? net_handle((int)o.n) : net_left(o.err, 0));
    return heff_tail(k, 1, &x);
  }
  case TAG_Read: {
    NetSock *s = net_sock(net_number(heff_field(p, 0)));
    int64_t n = net_int(heff_field(p, 1));
    V f = heff_dup(heff_field(p, 2));
    heff_drop(p);
    V r;
    if (n <= 0) r = net_right(heff_bytes_in("", 0));
    else {
      o.n = n > NET_READ_MAX ? NET_READ_MAX : n;
      o.buf = heff_alloc((size_t)o.n);
      o.fd = s->fd;
      o.step = net_read_step;
      net_wait(&o);
      r = o.err ? net_left(o.err, 0) : net_right(heff_bytes_in(o.buf, o.done));
      free(o.buf);
    }
    V x = heff_answer(f, r);
    return heff_tail(k, 1, &x);
  }
  case TAG_Write: {
    NetSock *s = net_sock(net_number(heff_field(p, 0)));
    V f = heff_dup(heff_field(p, 2));
    // (the bytes are `p`'s, held until they are written)
    o.data = heff_bytes_data(heff_field(p, 1), &o.n);
    o.fd = s->fd;
    o.step = net_write_step;
    net_wait(&o);
    heff_drop(p);
    V x = heff_answer(f, o.err ? net_left(o.err, 0) : net_right(NTAG(TAG_MkUnit)));
    return heff_tail(k, 1, &x);
  }
  case TAG_Shutdown: {
    NetSock *s = net_sock(net_number(heff_field(p, 0)));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V r = shutdown(s->fd, SHUT_WR) < 0 ? net_left(errno, 0) : net_right(NTAG(TAG_MkUnit));
    V x = heff_answer(f, r);
    return heff_tail(k, 1, &x);
  }
  case TAG_Close: {
    int64_t i = net_int(heff_field(p, 0));
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    net_sock(i);
    heff_release(i);
    return heff_tail(k, 1, &x);
  }
  case TAG_Peer: {
    NetSock *s = net_sock(net_number(heff_field(p, 0)));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, net_where(s->fd, 1));
    return heff_tail(k, 1, &x);
  }
  case TAG_Local: {
    NetSock *s = net_sock(net_int(heff_field(p, 0)));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, net_where(s->fd, 0));
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of net");
    return 0;
  }
}

#endif
