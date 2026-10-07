// The terminal's side of src/heff/report.hf, for the C backend: an
// effect provider (the shape of one is said in lib/std/io.provider.c).

#include <unistd.h>

static V heff_provider_report(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_UseColor: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    V x = heff_answer(f, heff_bool(isatty(1) && !getenv("NO_COLOR")));
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of report");
    return 0;
  }
}
