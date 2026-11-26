/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/MaybeOwned.h>
#include <AK/Stream.h>
#include <LibCompress/GenericZlib.h>

namespace Compress {

// Raw deflate data (RFC 1951), without the zlib or gzip wrapping, as in zip archives.
class DeflateDecompressor final : public GenericZlibDecompressor {
public:
    static ErrorOr<NonnullOwnPtr<DeflateDecompressor>> create(MaybeOwned<Stream>);
    static ErrorOr<ByteBuffer> decompress_all(ReadonlyBytes);

private:
    DeflateDecompressor(AK::FixedArray<u8> buffer, MaybeOwned<Stream> stream, z_stream* zstream)
        : GenericZlibDecompressor(move(buffer), move(stream), zstream)
    {
    }
};

// The CRC-32 of zip archives and gzip data (ISO 3309).
u32 crc32(ReadonlyBytes);

}
