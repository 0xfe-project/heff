// Public-key signatures, std.crypto's, for the C backend: done by
// OpenSSL's libcrypto (3.0 or later, its EVP interface).  No provider
// written by hand: the module describes each operation's function, and
// the compiler makes the provider; these are the functions.
//
// A key is an `EVP_PKEY`, let go of by `EVP_PKEY_free` when the program
// drops its last reference.  What reads or makes a key, or signs, gives
// an outcome (`CrOutcome`), a foreign value of its own: the key or the
// signature, or what kind of failure it was (as the module numbers the
// kinds), why, and an RSA key's bits where it has too few.  The module
// takes the key or the signature out of it, or, there being none, asks
// it why, each an operation of its own.  Why is the outcome's, not the
// thread's: between two operations of the module's another of the
// program's may run on the same thread -- a release due, run before
// the next operation; a handler's own, between what it hands on -- and
// fail too, so what a thread was last told would not be this one's.
// OpenSSL's queue of errors is emptied after every operation, so that
// one never reads another's.  A PEM is given as bytes, NULs and all: a
// string with a NUL in it is not given C at all, and a PEM with one is
// OpenSSL's to refuse, as any other text that holds no key.
//
// A key read or made is one an algorithm signs or verifies with: RSA's
// of as many bits as the module gives (`min`) or more, P-256's,
// Ed25519's.  Any other is refused as it is read, so that signing and
// verifying need not ask.  A private key's numbers are not checked as
// it is read (to check an RSA key's is to test its primes, tens of
// milliseconds); one whose numbers cannot sign -- an even modulus, an
// even prime -- is found so as it signs, an invalid key.
//
// The algorithms are JWS's (RFC 7518): RS256, RSASSA-PKCS1-v1_5 with
// SHA-256; PS256, RSASSA-PSS with SHA-256, MGF1 of SHA-256 and a salt as
// long as the digest; ES256, ECDSA on P-256 with SHA-256, its signature
// `r` and `s` each 32 bytes, one after the other (not DER, as OpenSSL
// has it); EdDSA, Ed25519.

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/decoder.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

// (the algorithms, as the module numbers them)
enum { CR_RS256 = 0, CR_PS256 = 1, CR_ES256 = 2, CR_EDDSA = 3 };

// (the kinds of failure, as the module numbers them)
enum {
  CR_FAILED = 0,
  CR_UNREADABLE = 1,
  CR_ENCRYPTED = 2,
  CR_UNSUPPORTED = 3,
  CR_INVALID = 4,
  CR_TOO_SHORT = 5,
  CR_WRONG_KEY = 6
};

// (what an operation came to: a key, or a signature and its length; or,
// neither, the failure's kind, an RSA key's bits and why)
typedef struct {
  EVP_PKEY *key;
  unsigned char *sig;
  size_t n;
  int kind, bits;
  char why[256];
} CrOutcome;

static void cr_outcome_free(CrOutcome *o) {
  EVP_PKEY_free(o->key);
  free(o->sig);
  free(o);
}

// (an outcome, of nothing yet; NULL where there is no room for one)
static CrOutcome *cr_outcome(void) { return calloc(1, sizeof(CrOutcome)); }

// (a failure of the kind, said so; OpenSSL's errors forgotten)
static void cr_failed(CrOutcome *o, int kind, int bits, const char *what) {
  snprintf(o->why, sizeof o->why, "%s", what);
  o->kind = kind;
  o->bits = bits;
  ERR_clear_error();
}

// (what OpenSSL said of its last error, or `otherwise`)
static const char *cr_reason(const char *otherwise) {
  unsigned long e = ERR_peek_last_error();
  const char *reason = e ? ERR_reason_error_string(e) : NULL;
  return reason ? reason : otherwise;
}

// (a failure of the kind: what, and what OpenSSL said of it)
static void cr_failed_said(CrOutcome *o, int kind, const char *what) {
  char w[sizeof o->why];
  unsigned long e = ERR_peek_last_error();
  const char *reason = e ? ERR_reason_error_string(e) : NULL;
  if (reason)
    snprintf(w, sizeof w, "%s (%s)", what, reason);
  else
    snprintf(w, sizeof w, "%s", what);
  cr_failed(o, kind, 0, w);
}

// (a failure of no kind of the module's: OpenSSL's own, where it has no
// room)
static void cr_fail(CrOutcome *o, const char *what) { cr_failed_said(o, CR_FAILED, what); }

// (the outcome's key, one more reference to it; NULL, errno EINVAL,
// where it has none)
static EVP_PKEY *cr_outcome_key(CrOutcome *o) {
  if (!o->key) {
    errno = EINVAL;
    return NULL;
  }
  EVP_PKEY_up_ref(o->key);
  return o->key;
}

// (the outcome's signature, its length in `n`; NULL, errno EINVAL, where
// it has none)
static const unsigned char *cr_outcome_signature(CrOutcome *o, size_t *n) {
  if (!o->sig) {
    errno = EINVAL;
    return NULL;
  }
  *n = o->n;
  return o->sig;
}

static int cr_outcome_kind(CrOutcome *o) { return o->kind; }

static int cr_outcome_bits(CrOutcome *o) { return o->bits; }

static const char *cr_outcome_why(CrOutcome *o) { return o->why; }

// (the key, if an algorithm signs or verifies with it, an RSA key's of
// `min` bits or more; else let go of, the failure said, and NULL)
static EVP_PKEY *cr_usable(CrOutcome *o, EVP_PKEY *k, int min) {
  char what[96];
  if (EVP_PKEY_is_a(k, "RSA")) {
    int bits = EVP_PKEY_get_bits(k);
    if (bits >= min) {
      ERR_clear_error();
      return k;
    }
    EVP_PKEY_free(k);
    cr_failed(o, CR_TOO_SHORT, bits, "an RSA key too short");
    return NULL;
  }
  if (EVP_PKEY_is_a(k, "EC")) {
    char group[64];
    size_t n = 0;
    if (EVP_PKEY_get_utf8_string_param(k, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof group, &n)) {
      if (strcmp(group, "prime256v1") == 0 || strcmp(group, "P-256") == 0) {
        ERR_clear_error();
        return k;
      }
      snprintf(what, sizeof what, "EC, %s", group);
    } else
      snprintf(what, sizeof what, "EC, a curve of no name");
    EVP_PKEY_free(k);
    cr_failed(o, CR_UNSUPPORTED, 0, what);
    return NULL;
  }
  if (EVP_PKEY_is_a(k, "ED25519")) {
    ERR_clear_error();
    return k;
  }
  const char *type = EVP_PKEY_get0_type_name(k);
  snprintf(what, sizeof what, "%s", type ? type : "a key of no name");
  EVP_PKEY_free(k);
  cr_failed(o, CR_UNSUPPORTED, 0, what);
  return NULL;
}

// (a PEM's passphrase, asked of no one: an encrypted key is not read, and
// `asked` says one was wanted)
static int cr_no_passphrase(char *buf, int size, int rw, void *asked) {
  (void)buf; (void)size; (void)rw;
  *(bool *)asked = true;
  return -1;
}

// (a private key of a PEM, its `n` bytes: PKCS#8, or RSA's or EC's own;
// RSA's of `min` bits or more)
static CrOutcome *cr_private_pem(const unsigned char *pem, size_t n, int min) {
  CrOutcome *o = cr_outcome();
  if (!o) return NULL;
  bool asked = false;
  BIO *b = BIO_new_mem_buf(pem, (int)n);
  EVP_PKEY *k = b ? PEM_read_bio_PrivateKey(b, NULL, cr_no_passphrase, &asked) : NULL;
  BIO_free(b);
  if (!k) {
    if (asked)
      cr_failed(o, CR_ENCRYPTED, 0, "an encrypted key");
    else
      cr_failed(o, CR_UNREADABLE, 0, cr_reason("not a private key in PEM"));
    return o;
  }
  o->key = cr_usable(o, k, min);
  return o;
}

// (a public key of a PEM, its `n` bytes: SubjectPublicKeyInfo, or RSA's
// own; RSA's of `min` bits or more)
static CrOutcome *cr_public_pem(const unsigned char *pem, size_t n, int min) {
  CrOutcome *o = cr_outcome();
  if (!o) return NULL;
  EVP_PKEY *k = NULL;
  OSSL_DECODER_CTX *d = OSSL_DECODER_CTX_new_for_pkey(
      &k, "PEM", NULL, NULL, EVP_PKEY_PUBLIC_KEY, NULL, NULL);
  const unsigned char *p = pem;
  size_t left = n;
  int ok = d && OSSL_DECODER_from_data(d, &p, &left) > 0 && k;
  OSSL_DECODER_CTX_free(d);
  if (!ok) {
    EVP_PKEY_free(k);
    cr_failed(o, CR_UNREADABLE, 0, cr_reason("not a public key in PEM"));
    return o;
  }
  o->key = cr_usable(o, k, min);
  return o;
}

// (a public key of the type named, made of its parameters)
static EVP_PKEY *cr_from_params(const char *type, OSSL_PARAM_BLD *bld) {
  OSSL_PARAM *ps = bld ? OSSL_PARAM_BLD_to_param(bld) : NULL;
  EVP_PKEY_CTX *c = ps ? EVP_PKEY_CTX_new_from_name(NULL, type, NULL) : NULL;
  EVP_PKEY *k = NULL;
  if (!c || EVP_PKEY_fromdata_init(c) <= 0
      || EVP_PKEY_fromdata(c, &k, EVP_PKEY_PUBLIC_KEY, ps) <= 0) {
    EVP_PKEY_free(k);
    k = NULL;
  }
  EVP_PKEY_CTX_free(c);
  OSSL_PARAM_free(ps);
  return k;
}

// (an RSA public key of its modulus and exponent, big-endian, as a JWK's
// `n` and `e` are: the exponent odd and 3 or more, the modulus of `min`
// bits or more.  OpenSSL makes a key of any such numbers, so that it
// fails only where it has no room)
static CrOutcome *cr_rsa_jwk(const unsigned char *n, size_t nn,
                             const unsigned char *e, size_t en, int min) {
  CrOutcome *o = cr_outcome();
  if (!o) return NULL;
  if (nn == 0 || en == 0) {
    cr_failed(o, CR_INVALID, 0, nn == 0 ? "no modulus" : "no exponent");
    return o;
  }
  BIGNUM *bn = BN_bin2bn(n, (int)nn, NULL);
  BIGNUM *be = BN_bin2bn(e, (int)en, NULL);
  OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
  EVP_PKEY *k = NULL;
  if (!bn || !be || !bld)
    cr_fail(o, "not an RSA public key");
  else if (!BN_is_odd(be) || BN_num_bits(be) < 2)
    cr_failed(o, CR_INVALID, 0, "an exponent even or less than 3");
  else if (BN_num_bits(bn) < min)
    cr_failed(o, CR_TOO_SHORT, BN_num_bits(bn), "an RSA key too short");
  else if (OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bn)
           && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, be)
           && (k = cr_from_params("RSA", bld)))
    ERR_clear_error();
  else
    cr_fail(o, "not an RSA public key");
  OSSL_PARAM_BLD_free(bld);
  BN_free(bn);
  BN_free(be);
  o->key = k;
  return o;
}

// (a P-256 public key of its point's coordinates, 32 bytes each, as a
// JWK's `x` and `y` are: a point on the curve)
static CrOutcome *cr_p256_jwk(const unsigned char *x, size_t xn,
                              const unsigned char *y, size_t yn) {
  CrOutcome *o = cr_outcome();
  if (!o) return NULL;
  if (xn != 32 || yn != 32) {
    cr_failed(o, CR_INVALID, 0, "a coordinate not of 32 bytes");
    return o;
  }
  unsigned char point[65];
  point[0] = 4;
  memcpy(point + 1, x, 32);
  memcpy(point + 33, y, 32);
  OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
  EVP_PKEY *k = NULL;
  if (bld
      && OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0)
      && OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof point))
    k = cr_from_params("EC", bld);
  OSSL_PARAM_BLD_free(bld);
  if (k) {
    EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_from_pkey(NULL, k, NULL);
    bool on = c && EVP_PKEY_public_check(c) == 1;
    EVP_PKEY_CTX_free(c);
    if (on) {
      ERR_clear_error();
      o->key = k;
      return o;
    }
    EVP_PKEY_free(k);
  }
  // (OpenSSL makes no key of a point off the curve, or does and finds it
  // so: either way, a point not on it)
  cr_failed(o, CR_INVALID, 0, "a point not on the curve");
  return o;
}

// (an Ed25519 public key of its 32 bytes, as a JWK's `x` is.  OpenSSL
// makes one of any 32 bytes, so that it fails only where it has no room)
static CrOutcome *cr_ed25519_jwk(const unsigned char *x, size_t xn) {
  CrOutcome *o = cr_outcome();
  if (!o) return NULL;
  if (xn != 32) {
    cr_failed(o, CR_INVALID, 0, "a key not of 32 bytes");
    return o;
  }
  o->key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, x, xn);
  if (!o->key)
    cr_fail(o, "not an Ed25519 public key");
  else
    ERR_clear_error();
  return o;
}

// (the public part of a key, a key of its own: its SubjectPublicKeyInfo
// written and read back, nothing private kept)
static EVP_PKEY *cr_public_of(EVP_PKEY *k) {
  unsigned char *der = NULL;
  int n = i2d_PUBKEY(k, &der);
  const unsigned char *p = der;
  EVP_PKEY *pub = n > 0 ? d2i_PUBKEY(NULL, &p, n) : NULL;
  OPENSSL_free(der);
  ERR_clear_error();
  return pub;
}

// (a public key as PEM, SubjectPublicKeyInfo, malloc'd)
static char *cr_public_pem_of(EVP_PKEY *k) {
  BIO *b = BIO_new(BIO_s_mem());
  char *text = NULL;
  if (b && PEM_write_bio_PUBKEY(b, k)) {
    char *data = NULL;
    long n = BIO_get_mem_data(b, &data);
    text = malloc((size_t)n + 1);
    if (text) {
      memcpy(text, data, (size_t)n);
      text[n] = 0;
    }
  }
  BIO_free(b);
  ERR_clear_error();
  return text;
}

// (whether the key is of the algorithm's kind: RSA's, P-256's -- every EC
// key read or made is --, Ed25519's)
static bool cr_fits(int alg, EVP_PKEY *k) {
  switch (alg) {
  case CR_RS256:
  case CR_PS256:
    return EVP_PKEY_is_a(k, "RSA");
  case CR_ES256:
    return EVP_PKEY_is_a(k, "EC");
  case CR_EDDSA:
    return EVP_PKEY_is_a(k, "ED25519");
  default:
    return false;
  }
}

// (a context to sign or verify by the algorithm with the key)
static EVP_MD_CTX *cr_digest(int alg, EVP_PKEY *k, bool signing) {
  EVP_MD_CTX *m = EVP_MD_CTX_new();
  EVP_PKEY_CTX *pc = NULL;
  const char *md = alg == CR_EDDSA ? NULL : "SHA256";
  int ok = m
           && (signing ? EVP_DigestSignInit_ex(m, &pc, md, NULL, NULL, k, NULL)
                       : EVP_DigestVerifyInit_ex(m, &pc, md, NULL, NULL, k, NULL)) > 0;
  if (ok && alg == CR_RS256)
    ok = EVP_PKEY_CTX_set_rsa_padding(pc, RSA_PKCS1_PADDING) > 0;
  if (ok && alg == CR_PS256)
    ok = EVP_PKEY_CTX_set_rsa_padding(pc, RSA_PKCS1_PSS_PADDING) > 0
         && EVP_PKEY_CTX_set_rsa_pss_saltlen(pc, RSA_PSS_SALTLEN_DIGEST) > 0
         && EVP_PKEY_CTX_set_rsa_mgf1_md_name(pc, "SHA256", NULL) > 0;
  if (!ok) {
    EVP_MD_CTX_free(m);
    return NULL;
  }
  return m;
}

// (an ECDSA signature, DER, as JWS has it: `r` and `s`, 32 bytes each,
// malloc'd)
static unsigned char *cr_der_to_raw(const unsigned char *der, size_t n) {
  const unsigned char *p = der;
  ECDSA_SIG *sig = d2i_ECDSA_SIG(NULL, &p, (long)n);
  if (!sig) return NULL;
  const BIGNUM *r = NULL, *s = NULL;
  ECDSA_SIG_get0(sig, &r, &s);
  unsigned char *raw = malloc(64);
  if (raw && (BN_bn2binpad(r, raw, 32) != 32 || BN_bn2binpad(s, raw + 32, 32) != 32)) {
    free(raw);
    raw = NULL;
  }
  ECDSA_SIG_free(sig);
  return raw;
}

// (JWS's ECDSA signature, 64 bytes, as DER, OpenSSL's to free; its length
// in `len`)
static unsigned char *cr_raw_to_der(const unsigned char *raw, size_t n, size_t *len) {
  if (n != 64) return NULL;
  ECDSA_SIG *sig = ECDSA_SIG_new();
  BIGNUM *r = BN_bin2bn(raw, 32, NULL);
  BIGNUM *s = BN_bin2bn(raw + 32, 32, NULL);
  if (!sig || !r || !s || !ECDSA_SIG_set0(sig, r, s)) {
    BN_free(r);
    BN_free(s);
    ECDSA_SIG_free(sig);
    return NULL;
  }
  unsigned char *der = NULL;
  int l = i2d_ECDSA_SIG(sig, &der);
  ECDSA_SIG_free(sig);
  if (l <= 0) return NULL;
  *len = (size_t)l;
  return der;
}

// (the message signed by the algorithm with the key, the signature
// malloc'd, ES256's `r` and `s`; else the failure said, and NULL.  With
// a key of the algorithm's kind, read as a key the algorithm signs with,
// OpenSSL fails to sign only of the key's numbers, or where it has no
// room: a key that does not sign)
static unsigned char *cr_signed(CrOutcome *o, int alg, EVP_PKEY *k,
                                const unsigned char *msg, size_t n, size_t *out) {
  if (!cr_fits(alg, k)) {
    cr_failed(o, CR_WRONG_KEY, 0, "the key is not of the algorithm's kind");
    return NULL;
  }
  EVP_MD_CTX *m = cr_digest(alg, k, true);
  if (!m) {
    cr_fail(o, "not signed");
    return NULL;
  }
  size_t len = 0;
  unsigned char *s = NULL;
  bool room = true;
  bool done = EVP_DigestSign(m, NULL, &len, msg, n) > 0
              && (room = (s = malloc(len ? len : 1)) != NULL)
              && EVP_DigestSign(m, s, &len, msg, n) > 0;
  EVP_MD_CTX_free(m);
  if (!done) {
    if (room)
      cr_failed_said(o, CR_INVALID, "a key that does not sign");
    else
      cr_fail(o, "not signed");
    free(s);
    return NULL;
  }
  if (alg == CR_ES256) {
    unsigned char *raw = cr_der_to_raw(s, len);
    free(s);
    if (!raw) {
      cr_fail(o, "not signed");
      return NULL;
    }
    s = raw;
    len = 64;
  }
  ERR_clear_error();
  *out = len;
  return s;
}

// (... as an outcome)
static CrOutcome *cr_sign(int alg, EVP_PKEY *k, const unsigned char *msg, size_t n) {
  CrOutcome *o = cr_outcome();
  if (!o) return NULL;
  o->sig = cr_signed(o, alg, k, msg, n, &o->n);
  return o;
}

// (whether the signature is the key's of the message by the algorithm: a
// key of another kind, or a signature not of the algorithm's shape, is
// not)
static bool cr_verify(int alg, EVP_PKEY *k, const unsigned char *msg, size_t n,
                      const unsigned char *sig, size_t sn) {
  bool ok = false;
  if (cr_fits(alg, k)) {
    unsigned char *der = NULL;
    const unsigned char *s = sig;
    size_t len = sn;
    if (alg == CR_ES256) s = der = cr_raw_to_der(sig, sn, &len);
    EVP_MD_CTX *m = s ? cr_digest(alg, k, false) : NULL;
    ok = m && EVP_DigestVerify(m, s, len, msg, n) == 1;
    EVP_MD_CTX_free(m);
    OPENSSL_free(der);
  }
  ERR_clear_error();
  return ok;
}
