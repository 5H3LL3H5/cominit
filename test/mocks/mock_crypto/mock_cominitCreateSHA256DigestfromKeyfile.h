// SPDX-License-Identifier: MIT
/**
 * @file mock_cominitCreateSHA256DigestfromKeyfile.h
 * @brief Header declaring a mock function for cominitCreateSHA256DigestfromKeyfile().
 */
#ifndef __MOCK_COMINIT_CREATESHA256DIGESTFROMFILE_H__
#define __MOCK_COMINIT_CREATESHA256DIGESTFROMFILE_H__

#include <tss2/tss2_common.h>
#include <tss2/tss2_esys.h>

/**
 * Mock function for cominitCreateSHA256DigestfromKeyfile().
 *
 * Implemented using cmocka. Inputs may be checked and return code set using cmocka API. Otherwise the function is a
 * no-op.
 */
// NOLINTNEXTLINE(readability-identifier-naming)    Rationale: Naming scheme fixed due to linker wrapping.
int __wrap_cominitCreateSHA256DigestfromKeyfile(const char *keyfile, unsigned char *digest, size_t digestLen);

#endif /* __MOCK_COMINIT_CREATESHA256DIGESTFROMFILE_H__ */
