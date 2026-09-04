#ifndef SEMANTIC_DIGEST_H
#define SEMANTIC_DIGEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEMANTIC_DIGEST_SHA256_LEN 32u
#define SEMANTIC_DIGEST_SHA256_BLOCK_LEN 64u

struct semantic_digest_sha256_context {
    uint32_t state[8];
    uint64_t total_len;
    uint8_t block[SEMANTIC_DIGEST_SHA256_BLOCK_LEN];
    size_t block_len;
    uint32_t lifecycle_cookie;
};

bool semantic_digest_sha256_init(
    struct semantic_digest_sha256_context *context);
bool semantic_digest_sha256_update(
    struct semantic_digest_sha256_context *context,
    const void *data,
    size_t data_len);
bool semantic_digest_sha256_final(
    struct semantic_digest_sha256_context *context,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN]);
bool semantic_digest_sha256(
    const void *data,
    size_t data_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN]);

/*
 * Equality is intentionally independent of the first differing byte. The
 * inputs are fixed-size semantic commitments rather than secrets, but keeping
 * one shared comparison prevents callers from reintroducing prefix checks.
 */
bool semantic_digest_equal(const uint8_t *left,
                           const uint8_t *right,
                           size_t digest_len);

#ifdef SEMANTIC_DIGEST_ENABLE_CALL_COUNTER
/*
 * Test-only instrumentation. The native test build defines this so a test can
 * assert how many SHA-256 computations one received packet costs. Production
 * (Zephyr) builds never define it, so no counter exists in firmware images.
 * The counter advances once per completed digest, i.e. once per successful
 * semantic_digest_sha256_final().
 */
unsigned long semantic_digest_sha256_call_count(void);
void semantic_digest_sha256_call_count_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
