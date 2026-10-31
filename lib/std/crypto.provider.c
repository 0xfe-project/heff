// Public-key signatures, std.crypto's, for the C backend: done by
// OpenSSL's libcrypto (3.0 or later, its EVP interface).  No provider
// written by hand: the module describes each operation's function, and
// the compiler makes the provider; these are the functions.
//
// A key is an `EVP_PKEY`, let go of by `EVP_PKEY_free` when the program
// drops its last reference.  A function that fails says why in a buffer
// of its thread's, with what OpenSSL said of it, sets `errno` and gives
// `NULL`; the operation `Why` reads the buffer.  OpenSSL's queue of
// errors is emptied after every operation, so that one never reads
// another's.
//
// The algorithms are JWS's (RFC 7518): RS256, RSASSA-PKCS1-v1_5 with
// SHA-256; PS256, RSASSA-PSS with SHA-256, MGF1 of SHA-256 and a salt as
// long as the digest; ES256, ECDSA on P-256 with SHA-256, its signature
// `r` and `s` each 32 bytes, one after the other (not DER, as OpenSSL
// has it); EdDSA, Ed25519.

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
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

// (why the thread's last operation failed)
static _Thread_local char cr_why[256];

// (a failure: why, and what OpenSSL said of it last; its errors
// forgotten, errno EINVAL)
static void cr_fail(const char *what) {
  unsigned long e = ERR_peek_last_error();
  const char *reason = e ? ERR_reason_error_string(e) : NULL;
  if (reason)
    snprintf(cr_why, sizeof cr_why, "%s (%s)", what, reason);
  else
    snprintf(cr_why, sizeof cr_why, "%s", what);
  ERR_clear_error();
  errno = EINVAL;
}

static const char *cr_why_of(void) { return cr_why; }

// (a PEM's passphrase, asked of no one: an encrypted key is not read)
static int cr_no_passphrase(char *buf, int size, int rw, void *u) {
  (void)buf; (void)size; (void)rw; (void)u;
  return -1;
}

// (a private key of a PEM: PKCS#8, or RSA's or EC's own)
static EVP_PKEY *cr_private_pem(const char *pem) {
  BIO *b = BIO_new_mem_buf(pem, -1);
  EVP_PKEY *k = b ? PEM_read_bio_PrivateKey(b, NULL, cr_no_passphrase, NULL) : NULL;
  BIO_free(b);
  if (!k) {
    cr_fail("not a private key in PEM");
    return NULL;
  }
  ERR_clear_error();
  return k;
}

// (a public key of a PEM: SubjectPublicKeyInfo, or RSA's own)
static EVP_PKEY *cr_public_pem(const char *pem) {
  EVP_PKEY *k = NULL;
  OSSL_DECODER_CTX *d = OSSL_DECODER_CTX_new_for_pkey(
      &k, "PEM", NULL, NULL, EVP_PKEY_PUBLIC_KEY, NULL, NULL);
  const unsigned char *p = (const unsigned char *)pem;
  size_t n = strlen(pem);
  int ok = d && OSSL_DECODER_from_data(d, &p, &n) > 0 && k;
  OSSL_DECODER_CTX_free(d);
  if (!ok) {
    EVP_PKEY_free(k);
    cr_fail("not a public key in PEM");
    return NULL;
  }
  ERR_clear_error();
  return k;
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
// `n` and `e` are)
static EVP_PKEY *cr_rsa_jwk(const unsigned char *n, size_t nn,
                            const unsigned char *e, size_t en) {
  EVP_PKEY *k = NULL;
  if (nn > 0 && en > 0) {
    BIGNUM *bn = BN_bin2bn(n, (int)nn, NULL);
    BIGNUM *be = BN_bin2bn(e, (int)en, NULL);
    OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
    if (bn && be && bld
        && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bn)
        && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, be))
      k = cr_from_params("RSA", bld);
    OSSL_PARAM_BLD_free(bld);
    BN_free(bn);
    BN_free(be);
  }
  if (!k) {
    cr_fail("not an RSA public key");
    return NULL;
  }
  ERR_clear_error();
  return k;
}

// (a P-256 public key of its point's coordinates, 32 bytes each, as a
// JWK's `x` and `y` are: a point on the curve)
static EVP_PKEY *cr_p256_jwk(const unsigned char *x, size_t xn,
                             const unsigned char *y, size_t yn) {
  EVP_PKEY *k = NULL;
  if (xn == 32 && yn == 32) {
    unsigned char point[65];
    point[0] = 4;
    memcpy(point + 1, x, 32);
    memcpy(point + 33, y, 32);
    OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
    if (bld
        && OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0)
        && OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof point))
      k = cr_from_params("EC", bld);
    OSSL_PARAM_BLD_free(bld);
    if (k) {
      EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_from_pkey(NULL, k, NULL);
      if (!c || EVP_PKEY_public_check(c) != 1) {
        EVP_PKEY_free(k);
        k = NULL;
      }
      EVP_PKEY_CTX_free(c);
    }
  }
  if (!k) {
    cr_fail("not a P-256 public key");
    return NULL;
  }
  ERR_clear_error();
  return k;
}

// (an Ed25519 public key of its 32 bytes, as a JWK's `x` is)
static EVP_PKEY *cr_ed25519_jwk(const unsigned char *x, size_t xn) {
  EVP_PKEY *k = xn == 32 ? EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, x, xn) : NULL;
  if (!k) {
    cr_fail("not an Ed25519 public key");
    return NULL;
  }
  ERR_clear_error();
  return k;
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

// (whether the key is of the algorithm's kind: RSA's, P-256's, Ed25519's)
static bool cr_fits(int alg, EVP_PKEY *k) {
  switch (alg) {
  case CR_RS256:
  case CR_PS256:
    return EVP_PKEY_is_a(k, "RSA");
  case CR_ES256: {
    char group[64];
    size_t n = 0;
    return EVP_PKEY_is_a(k, "EC")
           && EVP_PKEY_get_utf8_string_param(k, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof group, &n)
           && (strcmp(group, "prime256v1") == 0 || strcmp(group, "P-256") == 0);
  }
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

// (the message signed by the algorithm with the key, malloc'd: ES256's
// signature `r` and `s`)
static unsigned char *cr_sign(int alg, EVP_PKEY *k, const unsigned char *msg, size_t n,
                              size_t *out) {
  if (!cr_fits(alg, k)) {
    cr_fail("the key is not of the algorithm's kind");
    return NULL;
  }
  EVP_MD_CTX *m = cr_digest(alg, k, true);
  size_t len = 0;
  unsigned char *s = NULL;
  if (!m || EVP_DigestSign(m, NULL, &len, msg, n) <= 0 || !(s = malloc(len ? len : 1))
      || EVP_DigestSign(m, s, &len, msg, n) <= 0) {
    free(s);
    EVP_MD_CTX_free(m);
    cr_fail("not signed");
    return NULL;
  }
  EVP_MD_CTX_free(m);
  if (alg == CR_ES256) {
    unsigned char *raw = cr_der_to_raw(s, len);
    free(s);
    if (!raw) {
      cr_fail("not signed");
      return NULL;
    }
    s = raw;
    len = 64;
  }
  ERR_clear_error();
  *out = len;
  return s;
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
