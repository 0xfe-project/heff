// The network's side of lib/std/net.hf, for the C backend: an effect
// provider (the shape of one is said in lib/std/io.provider.c).  What is
// at a `data:` URL is read from it; at any other, `curl` (on the PATH of
// every system heff runs on) fetches it.  As Node.js's fetch, a failure
// is its message, not the page.

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

static V net_fetch(const char *u) {
  Buf b = {0};
  if (strncmp(u, "data:", 5) == 0) {
    if (!net_data(u, &b)) buf_str(&b, "TypeError: fetch failed");
  } else {
    Buf err = {0};
    char *as[] = { "curl", "-sSL", "--fail", "--", (char *)u, NULL };
    int64_t status = heff_capture(as, &b, &err);
    if (status != 0) {
      b.n = 0;
      buf_str(&b, "TypeError: fetch failed");
      if (err.n) {
        buf_str(&b, ": ");
        while (err.n && (err.s[err.n - 1] == '\n' || err.s[err.n - 1] == '\r')) err.n--;
        buf_put(&b, err.s, err.n);
      }
    }
    free(err.s);
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
