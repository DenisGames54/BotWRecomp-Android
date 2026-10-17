// iOS build only: the two OpenSSL digests the vendored Cemu code uses (SHA-1 via EVP in
// FetchShader.cpp, one-shot SHA256 in MetalPipelineCache.cpp), on top of CommonCrypto, so the
// iOS app does not need a cross-built libcrypto. Same digests, so cache keys match macOS.
#pragma once
#include <CommonCrypto/CommonDigest.h>
#include <stddef.h>

#define SHA_DIGEST_LENGTH CC_SHA1_DIGEST_LENGTH
#define SHA256_DIGEST_LENGTH CC_SHA256_DIGEST_LENGTH

static inline unsigned char* SHA256(const unsigned char* d, size_t n, unsigned char* md) {
    CC_SHA256(d, (CC_LONG)n, md);
    return md;
}
static inline unsigned char* SHA1(const unsigned char* d, size_t n, unsigned char* md) {
    CC_SHA1(d, (CC_LONG)n, md);
    return md;
}
