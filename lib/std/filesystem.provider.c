// The disk's side of lib/std/filesystem.hf, for the C backend: an effect
// provider (the shape of one is said in lib/std/io.provider.c).

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

// (a file's text, if it can be read)
static V heff_read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return heff_nothing;
  Buf b = {0};
  char chunk[65536];
  size_t got;
  while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) buf_put(&b, chunk, got);
  int bad = ferror(f);
  fclose(f);
  V s = bad ? 0 : heff_str_utf8((const unsigned char *)(b.s ? b.s : ""), (int64_t)b.n);
  free(b.s);
  return bad ? heff_nothing : heff_just(s);
}

static int heff_write_file(const char *path, V s) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  heff_put(f, s);
  return fclose(f) == 0;
}

// (a directory and those it is in)
static int heff_mkdir_p(char *path) {
  for (char *c = path + 1; *c; c++)
    if (*c == '/') {
      *c = 0;
      int ok = mkdir(path, 0777) == 0 || errno == EEXIST;
      *c = '/';
      if (!ok) return 0;
    }
  return mkdir(path, 0777) == 0 || errno == EEXIST;
}

// (the names in a directory, sorted)
static V heff_read_dir(const char *path) {
  DIR *d = opendir(path);
  if (!d) return heff_nothing;
  int64_t n = 0, cap = 64;
  char **ns = heff_alloc(cap * sizeof(char *));
  struct dirent *e;
  while ((e = readdir(d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    if (n == cap) {
      char **n2 = heff_alloc(cap * 2 * sizeof(char *));
      memcpy(n2, ns, n * sizeof(char *));
      free(ns);
      ns = n2;
      cap *= 2;
    }
    ns[n++] = strdup(e->d_name);
  }
  closedir(d);
  qsort(ns, n, sizeof(char *), heff_cmp_cstr);
  V l = heff_strs(ns, n);
  for (int64_t i = 0; i < n; i++) free(ns[i]);
  free(ns);
  return heff_just(l);
}

// (every file and directory under `p`, its mode's write bits off, or on:
// a directory is made writable before what it holds, read-only after)
static int heff_chmod_all(const char *p, int writable) {
  struct stat st;
  if (lstat(p, &st) != 0) return errno == ENOENT ? -1 : 0;
  if (S_ISLNK(st.st_mode)) return 1;
  mode_t mode = writable ? (st.st_mode | 0200) : (st.st_mode & ~0222);
  mode &= 07777;
  if (S_ISDIR(st.st_mode)) {
    if (writable && chmod(p, mode) != 0) return 0;
    DIR *d = opendir(p);
    if (!d) return 0;
    struct dirent *e;
    int ok = 1;
    while (ok && (e = readdir(d))) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      char *q = heff_join(p, e->d_name);
      ok = heff_chmod_all(q, writable) != 0;
      free(q);
    }
    closedir(d);
    if (!ok) return 0;
    if (!writable && chmod(p, mode) != 0) return 0;
    return 1;
  }
  return chmod(p, mode) == 0;
}

// (a file, or a directory and all it holds; nothing there is no failure)
static int heff_remove_all(const char *p) {
  struct stat st;
  if (lstat(p, &st) != 0) return errno == ENOENT;
  if (!S_ISDIR(st.st_mode)) return unlink(p) == 0;
  DIR *d = opendir(p);
  if (!d) return 0;
  struct dirent *e;
  int ok = 1;
  while (ok && (e = readdir(d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char *q = heff_join(p, e->d_name);
    ok = heff_remove_all(q);
    free(q);
  }
  closedir(d);
  return ok && rmdir(p) == 0;
}

static V heff_provider_fs(V p, V k) {
  char *path = heff_cstr(heff_field(p, 0));
  V x;
  switch (heff_tag_of(p)) {
  case TAG_ReadFile: {
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    x = heff_answer(f, heff_read_file(path));
    break;
  }
  case TAG_WriteFile: {
    int ok = heff_write_file(path, heff_field(p, 1));
    V f = heff_dup(heff_field(p, 2));
    heff_drop(p);
    x = heff_answer(f, heff_bool(ok));
    break;
  }
  case TAG_MkDir: {
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    x = heff_answer(f, heff_bool(heff_mkdir_p(path)));
    break;
  }
  case TAG_ReadDir: {
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    x = heff_answer(f, heff_read_dir(path));
    break;
  }
  case TAG_Remove: {
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    int ok = heff_chmod_all(path, 1) != 0 && heff_remove_all(path);
    x = heff_answer(f, heff_bool(ok));
    break;
  }
  case TAG_ReadOnly: {
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    x = heff_answer(f, heff_bool(heff_chmod_all(path, 0) == 1));
    break;
  }
  default:
    heff_fail("not an operation of fs");
    return 0;
  }
  free(path);
  return heff_tail(k, 1, &x);
}
