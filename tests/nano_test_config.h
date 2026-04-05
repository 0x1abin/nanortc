/*
 * nanortc — Test configuration helper
 *
 * Selects the correct crypto provider based on build-time config.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_TEST_CONFIG_H_
#define NANORTC_TEST_CONFIG_H_

#include "nanortc_crypto.h"

static inline const nanortc_crypto_provider_t *nano_test_crypto(void)
{
#if defined(NANORTC_CRYPTO_OPENSSL)
    return nanortc_crypto_openssl();
#else
    return nanortc_crypto_mbedtls();
#endif
}

#endif /* NANORTC_TEST_CONFIG_H_ */
