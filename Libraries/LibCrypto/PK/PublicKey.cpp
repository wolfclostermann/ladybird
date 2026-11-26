/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/PK/PublicKey.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

namespace Crypto::PK {

ErrorOr<bool> verify_with_subject_public_key_info(ReadonlyBytes subject_public_key_info, Hash::HashKind hash_kind, ReadonlyBytes message, ReadonlyBytes signature)
{
    auto const* data = subject_public_key_info.data();
    auto* key_pointer = d2i_PUBKEY(nullptr, &data, static_cast<long>(subject_public_key_info.size()));
    if (!key_pointer)
        return false;
    auto key = TRY(OpenSSL_PKEY::wrap(key_pointer));

    // Trailing bytes after the key are not part of it.
    if (data != subject_public_key_info.data() + subject_public_key_info.size())
        return false;

    auto ctx = TRY(OpenSSL_MD_CTX::create());
    OPENSSL_TRY(EVP_DigestVerifyInit_ex(ctx.ptr(), nullptr, TRY(Hash::openssl_digest_name(hash_kind)), nullptr, nullptr, key.ptr(), nullptr));
    // NB: Anything but 1 means the signature does not check out, including a malformed one.
    return EVP_DigestVerify(ctx.ptr(), signature.data(), signature.size(), message.data(), message.size()) == 1;
}

}
