#include "app_gateway_operation_owner.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void test_stale_release_cannot_clear_successor(void)
{
    struct app_gateway_operation_owner owner = {0};
    struct app_gateway_operation_lease first = {0};
    struct app_gateway_operation_lease successor = {0};

    assert(app_gateway_operation_owner_claim(
               &owner,
               APP_GATEWAY_OPERATION_OWNER_ASSIGNMENT,
               &first) == 0);
    assert(app_gateway_operation_owner_release(&owner, &first) == 0);
    assert(app_gateway_operation_owner_claim(
               &owner,
               APP_GATEWAY_OPERATION_OWNER_ASSIGNMENT,
               &successor) == 0);
    assert(successor.generation != first.generation);
    assert(app_gateway_operation_owner_release(&owner, &first) == -ESTALE);
    assert(owner.active.owner == APP_GATEWAY_OPERATION_OWNER_ASSIGNMENT);
    assert(owner.active.generation == successor.generation);
    assert(app_gateway_operation_owner_release(&owner, &successor) == 0);
}

static void test_wrap_skips_invalid_generation_zero(void)
{
    struct app_gateway_operation_owner owner = {
        .next_generation = UINT32_MAX,
    };
    struct app_gateway_operation_lease lease = {0};

    assert(app_gateway_operation_owner_claim(
               &owner,
               APP_GATEWAY_OPERATION_OWNER_ASSIGNMENT,
               &lease) == 0);
    assert(lease.generation == 1u);
}

int main(void)
{
    test_stale_release_cannot_clear_successor();
    test_wrap_skips_invalid_generation_zero();
    puts("gateway operation owner tests passed");
    return 0;
}
