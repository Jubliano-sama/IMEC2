#include "semantic_digest.h"

#include <limits.h>
#include <string.h>

#define SHA256_ROTATE_RIGHT(value, count) \
    (((value) >> (count)) | ((value) << (32u - (count))))
#define SHA256_CONTEXT_LIFECYCLE_COOKIE UINT32_C(0x53483236)

static const uint32_t sha256_round_constants[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491),
    UINT32_C(0xb5c0fbcf), UINT32_C(0xe9b5dba5),
    UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5),
    UINT32_C(0xd807aa98), UINT32_C(0x12835b01),
    UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe),
    UINT32_C(0x9bdc06a7), UINT32_C(0xc19bf174),
    UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc),
    UINT32_C(0x2de92c6f), UINT32_C(0x4a7484aa),
    UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d),
    UINT32_C(0xb00327c8), UINT32_C(0xbf597fc7),
    UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967),
    UINT32_C(0x27b70a85), UINT32_C(0x2e1b2138),
    UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb),
    UINT32_C(0x81c2c92e), UINT32_C(0x92722c85),
    UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3),
    UINT32_C(0xd192e819), UINT32_C(0xd6990624),
    UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08),
    UINT32_C(0x2748774c), UINT32_C(0x34b0bcb5),
    UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3),
    UINT32_C(0x748f82ee), UINT32_C(0x78a5636f),
    UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb),
    UINT32_C(0xbef9a3f7), UINT32_C(0xc67178f2),
};

static uint32_t get_u32_be(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24u) |
           ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) |
           bytes[3];
}

static void put_u32_be(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24u);
    bytes[1] = (uint8_t)(value >> 16u);
    bytes[2] = (uint8_t)(value >> 8u);
    bytes[3] = (uint8_t)value;
}

static void sha256_transform(struct semantic_digest_sha256_context *context,
                             const uint8_t block[SEMANTIC_DIGEST_SHA256_BLOCK_LEN])
{
    uint32_t schedule[64];
    uint32_t a = context->state[0];
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    uint32_t e = context->state[4];
    uint32_t f = context->state[5];
    uint32_t g = context->state[6];
    uint32_t h = context->state[7];

    for (size_t i = 0u; i < 16u; i++) {
        schedule[i] = get_u32_be(&block[i * 4u]);
    }
    for (size_t i = 16u; i < 64u; i++) {
        uint32_t s0 =
            SHA256_ROTATE_RIGHT(schedule[i - 15u], 7u) ^
            SHA256_ROTATE_RIGHT(schedule[i - 15u], 18u) ^
            (schedule[i - 15u] >> 3u);
        uint32_t s1 =
            SHA256_ROTATE_RIGHT(schedule[i - 2u], 17u) ^
            SHA256_ROTATE_RIGHT(schedule[i - 2u], 19u) ^
            (schedule[i - 2u] >> 10u);

        schedule[i] = schedule[i - 16u] + s0 +
                      schedule[i - 7u] + s1;
    }

    for (size_t i = 0u; i < 64u; i++) {
        uint32_t choice = (e & f) ^ ((~e) & g);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t sum0 = SHA256_ROTATE_RIGHT(a, 2u) ^
                        SHA256_ROTATE_RIGHT(a, 13u) ^
                        SHA256_ROTATE_RIGHT(a, 22u);
        uint32_t sum1 = SHA256_ROTATE_RIGHT(e, 6u) ^
                        SHA256_ROTATE_RIGHT(e, 11u) ^
                        SHA256_ROTATE_RIGHT(e, 25u);
        uint32_t temp1 = h + sum1 + choice +
                         sha256_round_constants[i] + schedule[i];
        uint32_t temp2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static bool sha256_context_valid(
    const struct semantic_digest_sha256_context *context)
{
    if (context == NULL ||
        context->lifecycle_cookie != SHA256_CONTEXT_LIFECYCLE_COOKIE ||
        context->block_len >= SEMANTIC_DIGEST_SHA256_BLOCK_LEN ||
        context->total_len > UINT64_MAX / 8u ||
        context->total_len < context->block_len) {
        return false;
    }
    return true;
}

bool semantic_digest_sha256_init(
    struct semantic_digest_sha256_context *context)
{
    static const uint32_t initial_state[8] = {
        UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85),
        UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
        UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
        UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19),
    };

    if (context == NULL) {
        return false;
    }
    memcpy(context->state, initial_state, sizeof(initial_state));
    context->total_len = 0u;
    context->block_len = 0u;
    memset(context->block, 0, sizeof(context->block));
    context->lifecycle_cookie = SHA256_CONTEXT_LIFECYCLE_COOKIE;
    return true;
}

bool semantic_digest_sha256_update(
    struct semantic_digest_sha256_context *context,
    const void *data,
    size_t data_len)
{
    const uint8_t *bytes = data;

    if (!sha256_context_valid(context) ||
        (data == NULL && data_len != 0u) ||
        data_len > (UINT64_MAX / 8u) - context->total_len) {
        return false;
    }
    context->total_len += data_len;

    while (data_len != 0u) {
        size_t available =
            SEMANTIC_DIGEST_SHA256_BLOCK_LEN - context->block_len;
        size_t copy_len = data_len < available ? data_len : available;

        memcpy(&context->block[context->block_len], bytes, copy_len);
        context->block_len += copy_len;
        bytes += copy_len;
        data_len -= copy_len;
        if (context->block_len == SEMANTIC_DIGEST_SHA256_BLOCK_LEN) {
            sha256_transform(context, context->block);
            context->block_len = 0u;
        }
    }
    return true;
}

#ifdef SEMANTIC_DIGEST_ENABLE_CALL_COUNTER
static unsigned long semantic_digest_sha256_calls;

unsigned long semantic_digest_sha256_call_count(void)
{
    return semantic_digest_sha256_calls;
}

void semantic_digest_sha256_call_count_reset(void)
{
    semantic_digest_sha256_calls = 0u;
}
#endif

bool semantic_digest_sha256_final(
    struct semantic_digest_sha256_context *context,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    uint64_t bit_len;

    if (!sha256_context_valid(context) || digest == NULL) {
        return false;
    }
    bit_len = context->total_len * 8u;
    context->block[context->block_len++] = UINT8_C(0x80);
    if (context->block_len > 56u) {
        memset(&context->block[context->block_len], 0,
               SEMANTIC_DIGEST_SHA256_BLOCK_LEN - context->block_len);
        sha256_transform(context, context->block);
        context->block_len = 0u;
    }
    memset(&context->block[context->block_len], 0, 56u - context->block_len);
    for (size_t i = 0u; i < sizeof(bit_len); i++) {
        context->block[63u - i] = (uint8_t)(bit_len >> (i * 8u));
    }
    sha256_transform(context, context->block);
    for (size_t i = 0u; i < 8u; i++) {
        put_u32_be(&digest[i * 4u], context->state[i]);
    }
    memset(context, 0, sizeof(*context));
#ifdef SEMANTIC_DIGEST_ENABLE_CALL_COUNTER
    semantic_digest_sha256_calls++;
#endif
    return true;
}

bool semantic_digest_sha256(
    const void *data,
    size_t data_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct semantic_digest_sha256_context context;

    return semantic_digest_sha256_init(&context) &&
           semantic_digest_sha256_update(&context, data, data_len) &&
           semantic_digest_sha256_final(&context, digest);
}

bool semantic_digest_equal(const uint8_t *left,
                           const uint8_t *right,
                           size_t digest_len)
{
    uint8_t difference = 0u;

    if (left == NULL || right == NULL || digest_len == 0u) {
        return false;
    }
    for (size_t i = 0u; i < digest_len; i++) {
        difference |= (uint8_t)(left[i] ^ right[i]);
    }
    return difference == 0u;
}
