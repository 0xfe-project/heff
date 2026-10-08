// The socket calls' side of src/testbridge/sys.hf, for the C backend: an
// effect provider (the shape of one is said in lib/std/io.provider.c).
// Each operation is the POSIX call, as it is: its numbers given to it as
// the program gave them -- the program has the constants (AF_NETLINK,
// SOCK_RAW, ...), so no Linux header is here, and this compiles on any
// POSIX system (on macOS too, where socket() then fails at run time).
// The answer is `Right` what the call returns, or `Left` its errno.
// Bytes are a list of integers, 0 to 255, each way.

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

// (an integer of the program's, as a C int; a big integer is none a call
// takes: -1)
static int sys_int(V v) { return IS_INT(v) ? (int)INT_OF(v) : -1; }

static V sys_left(int e) {
  V x = MK_INT(e);
  return heff_tag(TAG_Left, 1, &x);
}

static V sys_right(V x) { return heff_tag(TAG_Right, 1, &x); }

// (a list of bytes, as a buffer: malloc'd, its length in `*n`)
static unsigned char *sys_bytes(V l, size_t *n) {
  size_t m = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) m++;
  unsigned char *b = heff_alloc(m ? m : 1), *o = b;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) *o++ = (unsigned char)sys_int(heff_field(c, 0));
  *n = m;
  return b;
}

// (a buffer, as a list of bytes)
static V sys_list(const unsigned char *b, size_t n) {
  V l = NTAG(TAG_Nil);
  for (size_t i = n; i > 0; i--) l = heff_cons(MK_INT(b[i - 1]), l);
  return l;
}

static V heff_provider_sys(V p, V k) {
  V r, f;
  switch (heff_tag_of(p)) {
  case TAG_Socket: {
    int fd = socket(sys_int(heff_field(p, 0)), sys_int(heff_field(p, 1)), sys_int(heff_field(p, 2)));
    r = fd < 0 ? sys_left(errno) : sys_right(MK_INT(fd));
    f = heff_dup(heff_field(p, 3));
    break;
  }
  case TAG_Send: {
    size_t n;
    unsigned char *b = sys_bytes(heff_field(p, 1), &n);
    ssize_t w = send(sys_int(heff_field(p, 0)), b, n, 0);
    int e = errno;
    free(b);
    r = w < 0 ? sys_left(e) : sys_right(MK_INT(w));
    f = heff_dup(heff_field(p, 2));
    break;
  }
  case TAG_Recv: {
    int max = sys_int(heff_field(p, 1));
    unsigned char *b = heff_alloc(max > 0 ? (size_t)max : 1);
    ssize_t got = recv(sys_int(heff_field(p, 0)), b, max > 0 ? (size_t)max : 0, 0);
    int e = errno;
    r = got < 0 ? sys_left(e) : sys_right(sys_list(b, (size_t)got));
    free(b);
    f = heff_dup(heff_field(p, 2));
    break;
  }
  case TAG_Close: {
    int c = close(sys_int(heff_field(p, 0)));
    r = c < 0 ? sys_left(errno) : sys_right(NTAG(TAG_MkUnit));
    f = heff_dup(heff_field(p, 1));
    break;
  }
  default:
    heff_fail("not an operation of sys");
    return 0;
  }
  heff_drop(p);
  V x = heff_answer(f, r);
  return heff_tail(k, 1, &x);
}
