/*
 * Bitcoin Echo — Main Entry Point
 *
 * Build once. Build right. Stop.
 */

#include <stdio.h>

#include "echo_types.h"
#include "echo_config.h"
#include "echo_assert.h"
#include "platform.h"

/*
 * Compile-time verification of critical type sizes.
 * These must hold for Bitcoin protocol correctness.
 */
ECHO_STATIC_ASSERT(sizeof(hash256_t) == 32, "hash256_t must be 32 bytes");
ECHO_STATIC_ASSERT(sizeof(hash160_t) == 20, "hash160_t must be 20 bytes");
ECHO_STATIC_ASSERT(sizeof(satoshi_t) == 8, "satoshi_t must be 8 bytes");

int main(int argc, char *argv[])
{
    (void)argc;  /* Unused for now */
    (void)argv;  /* Unused for now */

    printf("Bitcoin Echo v%s (%s)\n", ECHO_VERSION_STRING, ECHO_NETWORK_NAME);
    printf("A complete Bitcoin protocol implementation in pure C.\n");

    return ECHO_OK;
}
