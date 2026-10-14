// The terminal's side of heff.lsp.report, for the C backend: an
// effect provider.

#include <unistd.h>

// (colour on the stream `fd`: it is a terminal, or `HEFF_TTY=1` says it
// is, and `NO_COLOR` is not set)
static int heff_report_color(int fd) {
  const char *tty = getenv("HEFF_TTY"), *no = getenv("NO_COLOR");
  return (isatty(fd) || (tty && tty[0] == '1' && !tty[1])) && !(no && no[0]);
}

static V heff_provider_report(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_UseColor:
  case TAG_ErrColor: {
    int fd = heff_tag_of(p) == TAG_UseColor ? 1 : 2;
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    V x = heff_answer(f, heff_bool(heff_report_color(fd)));
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of report");
    return 0;
  }
}
