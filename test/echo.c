// A server for the tests, that says what it was sent: `echo` listens at
// a port of 127.0.0.1 no one listens at, says it (a line), and answers
// each request, one at a time, `201`, `X-Seen: yes` and a body of its
// method, its target and its body, a space between.  `echo port` only
// says a port no one listens at.  POSIX alone, so that it builds where
// zig does, with nothing else: the test builds and runs it.
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  int l = socket(AF_INET, SOCK_STREAM, 0);
  int on = 1;
  setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t n = sizeof a;
  if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof a) != 0 ||
      getsockname(l, (struct sockaddr *)&a, &n) != 0 || listen(l, 16) != 0) {
    perror("echo");
    return 2;
  }
  printf("%d\n", ntohs(a.sin_port));
  fflush(stdout);
  if (argc > 1 && strcmp(argv[1], "port") == 0) return 0;
  static char q[65536];
  for (;;) {
    int c = accept(l, NULL, NULL);
    if (c < 0) continue;
    // (the request read: its head, then as much body as it says)
    size_t got = 0;
    char *body = NULL;
    long want = 0;
    for (;;) {
      ssize_t r = read(c, q + got, sizeof q - 1 - got);
      if (r <= 0) break;
      got += (size_t)r;
      q[got] = 0;
      if (!body && (body = strstr(q, "\r\n\r\n"))) {
        body += 4;
        for (char *h = q; h < body; h = strstr(h, "\r\n") + 2)
          if (strncasecmp(h, "content-length:", 15) == 0) want = atol(h + 15);
      }
      if (body && (long)(q + got - body) >= want) break;
    }
    if (!body) { close(c); continue; }
    char method[16] = "", target[4096] = "";
    sscanf(q, "%15s %4095s", method, target);
    static char said[65536 + 8192];
    int m = snprintf(said, sizeof said, "%s %s %.*s", method, target, (int)want, body);
    char head[256];
    int h = snprintf(head, sizeof head,
                     "HTTP/1.1 201 Created\r\nContent-Type: text/plain\r\nX-Seen: yes\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", m);
    if (write(c, head, (size_t)h) == h) (void)!write(c, said, (size_t)m);
    close(c);
  }
}
