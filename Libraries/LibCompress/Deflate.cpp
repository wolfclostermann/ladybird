/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Deflate.h>

#include <zlib.h>

namespace Compress {

ErrorOr<NonnullOwnPtr<DeflateDecompressor>> DeflateDecompressor::create(MaybeOwned<Stream> stream)
{
    auto buffer = TRY(AK::FixedArray<u8>::create(16 * 1024));
    // Negative window bits mean raw deflate data.
    auto zstream = TRY(GenericZlibDecompressor::new_z_stream(-MAX_WBITS));
    return adopt_nonnull_own_or_enomem(new (nothrow) DeflateDecompressor(move(buffer), move(stream), zstream));
}

ErrorOr<ByteBuffer> DeflateDecompressor::decompress_all(ReadonlyBytes bytes)
{
    return ::Compress::decompress_all<DeflateDecompressor>(bytes);
}

u32 crc32(ReadonlyBytes bytes)
{
    uLong crc = ::crc32(0L, Z_NULL, 0);
    // NB: zlib takes at most a uInt at a time.
    while (!bytes.is_empty()) {
        auto chunk = bytes.slice(0, min(bytes.size(), static_cast<size_t>(1u << 30)));
        crc = ::crc32(crc, chunk.data(), static_cast<uInt>(chunk.size()));
        bytes = bytes.slice(chunk.size());
    }
    return static_cast<u32>(crc);
}

}
