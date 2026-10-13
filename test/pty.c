// A pseudo-terminal for the tests: `pty COLS ROWS CMD [ARGS...]` runs
// the command on a terminal of its own, of that size -- its standard
// streams the terminal, in a session of its own whose terminal it is --
// what is written to this program's standard input given to it as typed,
// and what it draws written to this program's standard output, as it
// comes.  It ends when the command does, with its status (128 and the
// signal, where a signal ended it).  POSIX alone (posix_openpt), so
// that it builds where zig does, with nothing else: test/terminal.mjs
// builds and runs it (`script`, which BSD's and util-linux's have, takes
// no input that is not a terminal on macOS).
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: pty COLS ROWS CMD [ARGS...]\n");
    return 2;
  }
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
    perror("pty");
    return 2;
  }
  char *name = ptsname(master);
  struct winsize ws = { (unsigned short)atoi(argv[2]), (unsigned short)atoi(argv[1]), 0, 0 };
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 2;
  }
  if (pid == 0) {
    setsid();
    int slave = open(name, O_RDWR);
    if (slave < 0) _exit(127);
#ifdef TIOCSCTTY
    ioctl(slave, TIOCSCTTY, 0);
#endif
    ioctl(slave, TIOCSWINSZ, &ws);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2) close(slave);
    close(master);
    execvp(argv[3], argv + 3);
    _exit(127);
  }
  // (what comes in, given the terminal; what it writes, written out --
  // until the command has ended and nothing more is to be read)
  struct pollfd fds[2] = { { 0, POLLIN, 0 }, { master, POLLIN, 0 } };
  char buf[65536];
  int status = 0, ended = 0;
  for (;;) {
    if (!ended && waitpid(pid, &status, WNOHANG) == pid) ended = 1;
    int r = poll(fds, 2, ended ? 0 : 50);
    if (r < 0 && errno != EINTR) break;
    if (r <= 0) {
      if (ended) break;
      continue;
    }
    if (fds[0].revents & (POLLIN | POLLHUP)) {
      ssize_t n = read(0, buf, sizeof buf);
      if (n <= 0) fds[0].fd = -1;
      else if (write(master, buf, (size_t)n) < 0) fds[0].fd = -1;
    }
    if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t n = read(master, buf, sizeof buf);
      if (n <= 0) {
        if (!ended) waitpid(pid, &status, 0);
        ended = 1;
        break;
      }
      fwrite(buf, 1, (size_t)n, stdout);
      fflush(stdout);
    }
  }
  if (!ended) waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}
