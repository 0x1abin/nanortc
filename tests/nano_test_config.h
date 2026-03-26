/*
 * nanortc — Test configuration helper
 *
 * Selects the correct crypto provider based on build-time config.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_TEST_CONFIG_H_
#define NANO_TEST_CONFIG_H_

#include "nano_crypto.h"

static inline const nano_crypto_provider_t *nano_test_crypto(void)
{
#if defined(NANORTC_CRYPTO_OPENSSL)
    return nano_crypto_openssl();
#else
    return nano_crypto_mbedtls();
#endif
}

#endif /* NANO_TEST_CONFIG_H_ */
