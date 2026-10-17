// iOS build only: minimal EVP digest API (SHA-1 / SHA-256) over CommonCrypto. See sha.h.
#pragma once
#include <CommonCrypto/CommonDigest.h>
#include <stdlib.h>
#include "sha.h"

typedef struct botw_evp_md { int kind; } EVP_MD;  // 1 = SHA-1, 256 = SHA-256
typedef struct botw_evp_md_ctx {
    int kind;
    union { CC_SHA1_CTX sha1; CC_SHA256_CTX sha256; } u;
} EVP_MD_CTX;
typedef struct engine_st ENGINE;

static inline const EVP_MD* EVP_sha1(void) { static const EVP_MD m = {1}; return &m; }
static inline const EVP_MD* EVP_sha256(void) { static const EVP_MD m = {256}; return &m; }
static inline EVP_MD_CTX* EVP_MD_CTX_new(void) { return (EVP_MD_CTX*)calloc(1, sizeof(EVP_MD_CTX)); }
static inline void EVP_MD_CTX_free(EVP_MD_CTX* c) { free(c); }
static inline int EVP_DigestInit_ex(EVP_MD_CTX* c, const EVP_MD* t, ENGINE* e) {
    (void)e;
    c->kind = t->kind;
    if (c->kind == 1) CC_SHA1_Init(&c->u.sha1); else CC_SHA256_Init(&c->u.sha256);
    return 1;
}
static inline int EVP_DigestInit(EVP_MD_CTX* c, const EVP_MD* t) { return EVP_DigestInit_ex(c, t, NULL); }
static inline int EVP_DigestUpdate(EVP_MD_CTX* c, const void* d, size_t n) {
    if (c->kind == 1) CC_SHA1_Update(&c->u.sha1, d, (CC_LONG)n); else CC_SHA256_Update(&c->u.sha256, d, (CC_LONG)n);
    return 1;
}
static inline int EVP_DigestFinal_ex(EVP_MD_CTX* c, unsigned char* md, unsigned int* s) {
    if (c->kind == 1) { CC_SHA1_Final(md, &c->u.sha1); if (s) *s = CC_SHA1_DIGEST_LENGTH; }
    else { CC_SHA256_Final(md, &c->u.sha256); if (s) *s = CC_SHA256_DIGEST_LENGTH; }
    return 1;
}
