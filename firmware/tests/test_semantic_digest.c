#include "semantic_digest.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void expect_hex_digest(const void *data,
                              size_t data_len,
                              const char *expected_hex)
{
    static const char hex_digits[] = "0123456789abcdef";
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];
    char actual_hex[(SEMANTIC_DIGEST_SHA256_LEN * 2u) + 1u];

    assert(semantic_digest_sha256(data, data_len, digest));
    for (size_t i = 0u; i < sizeof(digest); i++) {
        actual_hex[i * 2u] = hex_digits[digest[i] >> 4u];
        actual_hex[(i * 2u) + 1u] = hex_digits[digest[i] & UINT8_C(0x0f)];
    }
    actual_hex[sizeof(digest) * 2u] = '\0';
    assert(strcmp(actual_hex, expected_hex) == 0);
}

static void test_known_vectors(void)
{
    static const char million_a_block[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    struct semantic_digest_sha256_context context;
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];
    static const uint8_t million_a_expected[SEMANTIC_DIGEST_SHA256_LEN] = {
        0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92,
        0x81, 0xa1, 0xc7, 0xe2, 0x84, 0xd7, 0x3e, 0x67,
        0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97, 0x20, 0x0e,
        0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0,
    };

    expect_hex_digest(NULL, 0u,
                      "e3b0c44298fc1c149afbf4c8996fb924"
                      "27ae41e4649b934ca495991b7852b855");
    expect_hex_digest("abc", 3u,
                      "ba7816bf8f01cfea414140de5dae2223"
                      "b00361a396177a9cb410ff61f20015ad");
    expect_hex_digest(
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        56u,
        "248d6a61d20638b8e5c026930c3e6039"
        "a33ce45964ff2167f6ecedd419db06c1");

    assert(semantic_digest_sha256_init(&context));
    for (size_t i = 0u; i < 1000000u / (sizeof(million_a_block) - 1u); i++) {
        assert(semantic_digest_sha256_update(
            &context, million_a_block, sizeof(million_a_block) - 1u));
    }
    assert(semantic_digest_sha256_final(&context, digest));
    assert(semantic_digest_equal(digest, million_a_expected, sizeof(digest)));
}

static void test_chunking_and_guards(void)
{
    static const uint8_t message[] =
        "a portable semantic digest must not depend on update boundaries";
    struct semantic_digest_sha256_context context;
    uint8_t one_shot[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t chunked[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t changed[SEMANTIC_DIGEST_SHA256_LEN];

    assert(semantic_digest_sha256(message, sizeof(message) - 1u, one_shot));
    assert(semantic_digest_sha256_init(&context));
    for (size_t i = 0u; i < sizeof(message) - 1u; i++) {
        assert(semantic_digest_sha256_update(&context, &message[i], 1u));
    }
    assert(semantic_digest_sha256_final(&context, chunked));
    assert(semantic_digest_equal(one_shot, chunked, sizeof(one_shot)));

    memcpy(changed, one_shot, sizeof(changed));
    changed[sizeof(changed) - 1u] ^= 1u;
    assert(!semantic_digest_equal(one_shot, changed, sizeof(one_shot)));
    assert(!semantic_digest_equal(NULL, changed, sizeof(changed)));
    assert(!semantic_digest_equal(one_shot, changed, 0u));
    assert(!semantic_digest_sha256(NULL, 1u, changed));
    assert(!semantic_digest_sha256(message, sizeof(message), NULL));
}

static void test_context_lifecycle_guards(void)
{
    struct semantic_digest_sha256_context context = {0};
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];

    assert(!semantic_digest_sha256_update(&context, NULL, 0u));
    assert(!semantic_digest_sha256_final(&context, digest));

    assert(semantic_digest_sha256_init(&context));
    context.block_len = SEMANTIC_DIGEST_SHA256_BLOCK_LEN;
    assert(!semantic_digest_sha256_update(&context, NULL, 0u));
    assert(!semantic_digest_sha256_final(&context, digest));

    assert(semantic_digest_sha256_init(&context));
    context.total_len = (UINT64_MAX / 8u) + 1u;
    assert(!semantic_digest_sha256_update(&context, NULL, 0u));
    assert(!semantic_digest_sha256_final(&context, digest));

    assert(semantic_digest_sha256_init(&context));
    assert(semantic_digest_sha256_final(&context, digest));
    assert(!semantic_digest_sha256_update(&context, NULL, 0u));
    assert(!semantic_digest_sha256_final(&context, digest));
}

int main(void)
{
    test_known_vectors();
    test_chunking_and_guards();
    test_context_lifecycle_guards();
    puts("semantic digest tests passed");
    return 0;
}
