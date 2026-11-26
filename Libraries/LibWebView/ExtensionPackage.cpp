/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Endian.h>
#include <AK/HashTable.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/MemoryStream.h>
#include <AK/StringBuilder.h>
#include <LibCompress/Deflate.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibCrypto/PK/PublicKey.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/ExtensionPackage.h>
#include <LibWebView/Extensions.h>

namespace WebView {

// Bounds on what a package may hold, so that a malformed or hostile one cannot fill the disk or the memory.
static constexpr size_t maximum_package_size = 512 * MiB;
static constexpr size_t maximum_unpacked_size = 1 * GiB;

// Reads little-endian numbers and byte runs, failing instead of reading past the end.
class ByteReader {
public:
    explicit ByteReader(ReadonlyBytes bytes)
        : m_bytes(bytes)
    {
    }

    size_t position() const { return m_position; }
    size_t remaining() const { return m_bytes.size() - m_position; }

    template<typename T>
    ErrorOr<T> read()
    {
        auto bytes = TRY(read_bytes(sizeof(T)));
        T value;
        __builtin_memcpy(&value, bytes.data(), sizeof(T));
        return AK::convert_between_host_and_little_endian(value);
    }

    ErrorOr<ReadonlyBytes> read_bytes(size_t count)
    {
        if (count > remaining())
            return Error::from_string_literal("The package is cut short");
        auto bytes = m_bytes.slice(m_position, count);
        m_position += count;
        return bytes;
    }

private:
    ReadonlyBytes m_bytes;
    size_t m_position { 0 };
};

String extension_id_for_public_key(ReadonlyBytes public_key)
{
    // https://chromium.googlesource.com/chromium/src/+/main/components/crx_file/id_util.cc
    auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, public_key);
    StringBuilder builder;
    for (auto byte : digest.bytes().slice(0, 16)) {
        builder.append(static_cast<char>('a' + (byte >> 4)));
        builder.append(static_cast<char>('a' + (byte & 0xf)));
    }
    return MUST(builder.to_string());
}

// The few parts of the protocol buffer encoding that a CRX3 header uses: https://protobuf.dev/programming-guides/encoding/
struct ProtobufField {
    u64 number { 0 };
    ReadonlyBytes bytes;
};

static ErrorOr<u64> read_varint(ByteReader& reader)
{
    u64 value = 0;
    for (size_t shift = 0; shift < 64; shift += 7) {
        auto byte = TRY(reader.read<u8>());
        value |= static_cast<u64>(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            return value;
    }
    return Error::from_string_literal("The package's header has an overlong number");
}

// Calls the callback for each length-delimited field, skipping the others.
template<typename Callback>
static ErrorOr<void> for_each_bytes_field(ReadonlyBytes message, Callback callback)
{
    ByteReader reader { message };
    while (reader.remaining() > 0) {
        auto key = TRY(read_varint(reader));
        auto number = key >> 3;
        switch (key & 7) {
        case 0:
            (void)TRY(read_varint(reader));
            break;
        case 1:
            (void)TRY(reader.read_bytes(8));
            break;
        case 2: {
            auto length = TRY(read_varint(reader));
            if (length > reader.remaining())
                return Error::from_string_literal("The package's header is cut short");
            TRY(callback(ProtobufField { number, TRY(reader.read_bytes(length)) }));
            break;
        }
        case 5:
            (void)TRY(reader.read_bytes(4));
            break;
        default:
            return Error::from_string_literal("The package's header is malformed");
        }
    }
    return {};
}

ErrorOr<CrxContents> read_crx(ReadonlyBytes file)
{
    // https://chromium.googlesource.com/chromium/src/+/main/components/crx_file/crx3.proto
    // A CRX3 file is "Cr24", the version (3), the header's size, the header (a CrxFileHeader), and the zip archive.
    ByteReader reader { file };
    auto magic = TRY(reader.read_bytes(4));
    if (magic != "Cr24"sv.bytes())
        return Error::from_string_literal("Not a .crx file");
    auto version = TRY(reader.read<u32>());
    if (version == 2)
        return Error::from_string_literal("The .crx file has the old CRX2 format, which is no longer supported");
    if (version != 3)
        return Error::from_string_literal("The .crx file has an unknown format");
    auto header_size = TRY(reader.read<u32>());
    auto header = TRY(reader.read_bytes(header_size));
    auto archive = file.slice(reader.position());

    struct Proof {
        ReadonlyBytes public_key;
        ReadonlyBytes signature;
    };
    Vector<Proof> proofs;
    Optional<ReadonlyBytes> signed_header_data;

    TRY(for_each_bytes_field(header, [&](ProtobufField const& field) -> ErrorOr<void> {
        // CrxFileHeader: sha256_with_rsa = 2, sha256_with_ecdsa = 3, signed_header_data = 10000. The key itself says
        // which algorithm it is for.
        if (field.number == 2 || field.number == 3) {
            // AsymmetricKeyProof: public_key = 1, signature = 2.
            Proof proof;
            TRY(for_each_bytes_field(field.bytes, [&](ProtobufField const& proof_field) -> ErrorOr<void> {
                if (proof_field.number == 1)
                    proof.public_key = proof_field.bytes;
                else if (proof_field.number == 2)
                    proof.signature = proof_field.bytes;
                return {};
            }));
            proofs.append(proof);
        } else if (field.number == 10000) {
            signed_header_data = field.bytes;
        }
        return {};
    }));

    if (!signed_header_data.has_value())
        return Error::from_string_literal("The .crx file names no extension ID");

    // SignedData: crx_id = 1, the first 16 bytes of the SHA-256 digest of the developer's key.
    Optional<ReadonlyBytes> crx_id;
    TRY(for_each_bytes_field(*signed_header_data, [&](ProtobufField const& field) -> ErrorOr<void> {
        if (field.number == 1)
            crx_id = field.bytes;
        return {};
    }));
    if (!crx_id.has_value() || crx_id->size() != 16)
        return Error::from_string_literal("The .crx file names no extension ID");

    // What is signed: "CRX3 SignedData\0", the size of the signed header data, the signed header data, and the archive.
    static constexpr auto signature_context = "CRX3 SignedData\0"sv;
    ByteBuffer signed_message;
    u32 signed_header_size = AK::convert_between_host_and_little_endian(static_cast<u32>(signed_header_data->size()));
    TRY(signed_message.try_append(signature_context.bytes()));
    TRY(signed_message.try_append(&signed_header_size, sizeof(signed_header_size)));
    TRY(signed_message.try_append(*signed_header_data));
    TRY(signed_message.try_append(archive));

    // Like Chromium, every signature must check out, and one must be the developer's, whose key gives the ID.
    Optional<ReadonlyBytes> developer_key;
    for (auto const& proof : proofs) {
        auto verified = TRY(Crypto::PK::verify_with_subject_public_key_info(proof.public_key, Crypto::Hash::HashKind::SHA256, signed_message, proof.signature));
        if (!verified)
            return Error::from_string_literal("The .crx file's signature is not valid");

        auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, proof.public_key);
        if (digest.bytes().slice(0, 16) == *crx_id)
            developer_key = proof.public_key;
    }
    if (!developer_key.has_value())
        return Error::from_string_literal("The .crx file is not signed with the key of its extension ID");

    return CrxContents { .public_key = TRY(ByteBuffer::copy(*developer_key)), .archive = archive };
}

// https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT
struct ZipEntry {
    ByteString name;
    u16 flags { 0 };
    u16 method { 0 };
    u32 crc32 { 0 };
    u32 compressed_size { 0 };
    u32 uncompressed_size { 0 };
    u32 local_header_offset { 0 };
};

static constexpr u32 end_of_central_directory_signature = 0x06054b50;
static constexpr u32 central_directory_header_signature = 0x02014b50;
static constexpr u32 local_file_header_signature = 0x04034b50;
static constexpr size_t end_of_central_directory_size = 22;

static ErrorOr<Vector<ZipEntry>> read_zip_central_directory(ReadonlyBytes archive)
{
    // The end of central directory record is last, followed only by a comment of at most 64 KiB.
    if (archive.size() < end_of_central_directory_size)
        return Error::from_string_literal("Not a zip archive");
    Optional<size_t> end_offset;
    auto earliest = archive.size() > end_of_central_directory_size + 0xffff ? archive.size() - end_of_central_directory_size - 0xffff : 0;
    for (size_t offset = archive.size() - end_of_central_directory_size + 1; offset-- > earliest;) {
        ByteReader probe { archive.slice(offset) };
        if (MUST(probe.read<u32>()) == end_of_central_directory_signature) {
            end_offset = offset;
            break;
        }
    }
    if (!end_offset.has_value())
        return Error::from_string_literal("Not a zip archive");

    ByteReader end { archive.slice(*end_offset) };
    (void)TRY(end.read<u32>());
    auto disk_number = TRY(end.read<u16>());
    auto central_directory_disk = TRY(end.read<u16>());
    auto entries_on_this_disk = TRY(end.read<u16>());
    auto entry_count = TRY(end.read<u16>());
    auto central_directory_size = TRY(end.read<u32>());
    auto central_directory_offset = TRY(end.read<u32>());

    if (disk_number != 0 || central_directory_disk != 0 || entries_on_this_disk != entry_count)
        return Error::from_string_literal("Zip archives split across files are not supported");
    if (entry_count == 0xffff || central_directory_size == 0xffffffff || central_directory_offset == 0xffffffff)
        return Error::from_string_literal("ZIP64 archives are not supported");
    if (static_cast<u64>(central_directory_offset) + central_directory_size > *end_offset)
        return Error::from_string_literal("The zip archive is malformed");

    Vector<ZipEntry> entries;
    ByteReader reader { archive.slice(central_directory_offset, central_directory_size) };
    for (size_t i = 0; i < entry_count; ++i) {
        if (TRY(reader.read<u32>()) != central_directory_header_signature)
            return Error::from_string_literal("The zip archive is malformed");
        ZipEntry entry;
        (void)TRY(reader.read<u16>()); // Version made by.
        (void)TRY(reader.read<u16>()); // Version needed to extract.
        entry.flags = TRY(reader.read<u16>());
        entry.method = TRY(reader.read<u16>());
        (void)TRY(reader.read<u32>()); // Modification time and date.
        entry.crc32 = TRY(reader.read<u32>());
        entry.compressed_size = TRY(reader.read<u32>());
        entry.uncompressed_size = TRY(reader.read<u32>());
        auto name_length = TRY(reader.read<u16>());
        auto extra_length = TRY(reader.read<u16>());
        auto comment_length = TRY(reader.read<u16>());
        (void)TRY(reader.read<u16>()); // Disk number start.
        (void)TRY(reader.read<u16>()); // Internal file attributes.
        (void)TRY(reader.read<u32>()); // External file attributes.
        entry.local_header_offset = TRY(reader.read<u32>());
        entry.name = ByteString { TRY(reader.read_bytes(name_length)) };
        (void)TRY(reader.read_bytes(extra_length));
        (void)TRY(reader.read_bytes(comment_length));

        if (entry.compressed_size == 0xffffffff || entry.uncompressed_size == 0xffffffff || entry.local_header_offset == 0xffffffff)
            return Error::from_string_literal("ZIP64 archives are not supported");
        entries.append(move(entry));
    }
    return entries;
}

// Whether a name in an archive is a relative path that stays within the folder it is unpacked into.
static bool is_safe_entry_name(StringView name)
{
    if (name.is_empty() || name.starts_with('/') || name.contains('\\') || name.contains('\0'))
        return false;
    auto path = name.ends_with('/') ? name.substring_view(0, name.length() - 1) : name;
    bool is_safe = true;
    path.for_each_split_view('/', SplitBehavior::KeepEmpty, [&](StringView component) {
        if (component.is_empty() || component == "."sv || component == ".."sv)
            is_safe = false;
    });
    return is_safe;
}

static ErrorOr<ByteBuffer> inflate_at_most(ReadonlyBytes compressed, size_t expected_size)
{
    auto stream = TRY(Compress::DeflateDecompressor::create(make<FixedMemoryStream>(compressed)));
    auto output = TRY(ByteBuffer::create_uninitialized(expected_size));
    size_t size = 0;
    while (!stream->is_eof()) {
        // One byte more than expected shows that the file is larger than the archive says.
        u8 extra_byte = 0;
        auto destination = size < expected_size ? output.bytes().slice(size) : Bytes { &extra_byte, 1 };
        auto read = TRY(stream->read_some(destination));
        if (read.is_empty() && stream->is_eof())
            break;
        if (size >= expected_size && !read.is_empty())
            return Error::from_string_literal("A file in the zip archive is larger than the archive says");
        size += read.size();
    }
    if (size != expected_size)
        return Error::from_string_literal("A file in the zip archive is smaller than the archive says");
    return output;
}

static ErrorOr<ByteBuffer> read_zip_entry(ReadonlyBytes archive, ZipEntry const& entry)
{
    if (entry.flags & 1)
        return Error::from_string_literal("Encrypted zip archives are not supported");

    ByteReader reader { archive };
    TRY(reader.read_bytes(entry.local_header_offset));
    if (TRY(reader.read<u32>()) != local_file_header_signature)
        return Error::from_string_literal("The zip archive is malformed");
    // The local header repeats the central directory's details, but may leave the sizes to a trailing data descriptor,
    // so only its name and extra field's lengths are used.
    TRY(reader.read_bytes(22));
    auto name_length = TRY(reader.read<u16>());
    auto extra_length = TRY(reader.read<u16>());
    TRY(reader.read_bytes(name_length + extra_length));
    auto compressed = TRY(reader.read_bytes(entry.compressed_size));

    ByteBuffer contents;
    switch (entry.method) {
    case 0:
        if (entry.compressed_size != entry.uncompressed_size)
            return Error::from_string_literal("The zip archive is malformed");
        contents = TRY(ByteBuffer::copy(compressed));
        break;
    case 8:
        contents = TRY(inflate_at_most(compressed, entry.uncompressed_size));
        break;
    default:
        return Error::from_string_literal("The zip archive uses a compression method that is not supported");
    }

    if (Compress::crc32(contents) != entry.crc32)
        return Error::from_string_literal("A file in the zip archive is damaged");
    return contents;
}

ErrorOr<void> extract_zip(ReadonlyBytes archive, StringView directory)
{
    auto entries = TRY(read_zip_central_directory(archive));

    // macOS's archiver adds resource forks in a folder of their own, which are no part of the extension.
    entries.remove_all_matching([](ZipEntry const& entry) { return entry.name.starts_with("__MACOSX/"sv); });

    u64 unpacked_size = 0;
    HashTable<ByteString> names;
    for (auto const& entry : entries) {
        if (!is_safe_entry_name(entry.name))
            return Error::from_string_literal("The zip archive holds a file with a path that is not allowed");
        unpacked_size += entry.uncompressed_size;
        if (unpacked_size > maximum_unpacked_size)
            return Error::from_string_literal("The zip archive is too large");
        names.set(entry.name);
    }

    // An archive of the extension's folder, rather than of its files, has them all in that one folder.
    StringView prefix;
    if (!names.contains("manifest.json"sv) && !entries.is_empty()) {
        auto first_folder = entries.first().name.find('/').map([&](size_t slash) { return entries.first().name.substring_view(0, slash + 1); });
        if (first_folder.has_value()
            && all_of(entries, [&](ZipEntry const& entry) { return entry.name.starts_with(*first_folder); })
            && names.contains(ByteString::formatted("{}manifest.json", *first_folder))) {
            prefix = *first_folder;
        }
    }

    for (auto const& entry : entries) {
        auto relative_name = entry.name.substring_view(prefix.length());
        if (relative_name.is_empty())
            continue;
        auto path = LexicalPath::join(directory, relative_name);

        if (relative_name.ends_with('/')) {
            TRY(Core::Directory::create(path, Core::Directory::CreateDirectories::Yes));
            continue;
        }

        auto contents = TRY(read_zip_entry(archive, entry));
        TRY(Core::Directory::create(path.parent(), Core::Directory::CreateDirectories::Yes));
        // NB: Only regular files are made, so an archive cannot plant a link that a later file would be written through.
        auto file = TRY(Core::File::open(path.string(), Core::File::OpenMode::Write | Core::File::OpenMode::MustBeNew));
        TRY(file->write_until_depleted(contents));
    }
    return {};
}

// Writes the key into the unpacked manifest, as Chromium does, so that the folder loads with the packed extension's ID.
static ErrorOr<void> write_key_into_manifest(StringView directory, ReadonlyBytes public_key)
{
    auto manifest_path = LexicalPath::join(directory, "manifest.json"sv).string();
    auto manifest_file = TRY(Core::File::open(manifest_path, Core::File::OpenMode::Read));
    auto manifest_source = TRY(String::from_utf8(TRY(manifest_file->read_until_eof())));
    auto manifest = TRY(JsonParser::parse(strip_json_comments(manifest_source)));
    if (!manifest.is_object())
        return Error::from_string_literal("manifest.json is not a JSON object");

    manifest.as_object().set("key"sv, TRY(encode_base64(public_key)));
    auto serialized = manifest.serialized();
    auto output = TRY(Core::File::open(manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(output->write_until_depleted(serialized.bytes()));
    return {};
}

ErrorOr<ByteString> unpack_extension_package(StringView package_path, StringView packages_directory)
{
    auto real_package_path = TRY(FileSystem::real_path(package_path));
    auto file = TRY(Core::File::open(real_package_path, Core::File::OpenMode::Read));
    auto size = TRY(file->size());
    if (size > maximum_package_size)
        return Error::from_string_literal("The file is too large");
    auto contents = TRY(file->read_until_eof());

    ReadonlyBytes archive = contents;
    Optional<CrxContents> crx;
    String folder_name;
    if (contents.bytes().starts_with("Cr24"sv.bytes())) {
        crx = TRY(read_crx(contents));
        archive = crx->archive;
        folder_name = extension_id_for_public_key(crx->public_key);
    } else if (contents.bytes().starts_with("PK"sv.bytes())) {
        // A zip archive has no key, so its folder (and so its ID) derives from the archive's path, which installing
        // the same archive again keeps.
        folder_name = extension_id_for_public_key(real_package_path.bytes());
    } else {
        return Error::from_string_literal("Not a .crx or .zip file");
    }

    TRY(Core::Directory::create(ByteString { packages_directory }, Core::Directory::CreateDirectories::Yes));
    auto destination = LexicalPath::join(packages_directory, folder_name).string();
    auto staging = LexicalPath::join(packages_directory, ByteString::formatted(".unpacking-{}", folder_name)).string();

    auto remove_staging = [&] {
        if (FileSystem::exists(staging))
            (void)FileSystem::remove(staging, FileSystem::RecursionMode::Allowed);
    };
    remove_staging();

    auto unpack = [&]() -> ErrorOr<void> {
        TRY(Core::System::mkdir(staging, 0755));
        TRY(extract_zip(archive, staging));
        if (crx.has_value())
            TRY(write_key_into_manifest(staging, crx->public_key));
        // What was unpacked before is replaced only by files that load as an extension.
        (void)TRY(load_unpacked_extension(staging));
        return {};
    };
    if (auto result = unpack(); result.is_error()) {
        remove_staging();
        return result.release_error();
    }

    if (FileSystem::exists(destination))
        TRY(FileSystem::remove(destination, FileSystem::RecursionMode::Allowed));
    TRY(Core::System::rename(staging, destination));
    return TRY(FileSystem::real_path(destination));
}

}
