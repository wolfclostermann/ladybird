/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Span.h>
#include <LibCrypto/Hash/Hash.h>

namespace Crypto::PK {

// Checks a signature with a public key given as a DER SubjectPublicKeyInfo (RFC 5280, section 4.1.2.7). RSA keys
// check PKCS #1 v1.5 signatures; EC keys check DER-encoded ECDSA signatures.
ErrorOr<bool> verify_with_subject_public_key_info(ReadonlyBytes subject_public_key_info, Hash::HashKind, ReadonlyBytes message, ReadonlyBytes signature);

}
