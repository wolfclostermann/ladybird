/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <LibCompress/Deflate.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibWebView/ExtensionPackage.h>
#include <LibWebView/Extensions.h>

static WebView::LoadedExtension extension_with_manifest(StringView manifest)
{
    return WebView::LoadedExtension {
        .id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"_string,
        .name = "Test"_string,
        .version = "1"_string,
        .directory = "/nonexistent"sv,
        .manifest = JsonParser::parse(manifest).release_value().as_object(),
        .content_scripts = {},
        .granted_permissions = {},
        .granted_origins = {},
    };
}

static Optional<URL::Origin> origin_of(StringView url)
{
    return URL::Parser::basic_parse(url)->origin();
}

TEST_CASE(manifest_v2_resources_are_accessible_to_every_page)
{
    auto extension = extension_with_manifest(R"({ "manifest_version": 2, "web_accessible_resources": ["images/*.png", "/page.html"] })"sv);

    EXPECT(WebView::is_web_accessible(extension, "images/icon.png"sv, origin_of("https://example.com/"sv)));
    EXPECT(WebView::is_web_accessible(extension, "page.html"sv, {}));
    EXPECT(!WebView::is_web_accessible(extension, "secret.html"sv, origin_of("https://example.com/"sv)));
}

TEST_CASE(manifest_v3_resources_are_accessible_to_the_sites_they_match)
{
    auto extension = extension_with_manifest(R"({ "manifest_version": 3, "web_accessible_resources": [
        { "resources": ["frame.html"], "matches": ["https://*.example.com/*"] },
        { "resources": ["everywhere.css"], "matches": ["<all_urls>"] }
    ] })"sv);

    EXPECT(WebView::is_web_accessible(extension, "frame.html"sv, origin_of("https://www.example.com/page"sv)));
    EXPECT(WebView::is_web_accessible(extension, "frame.html"sv, origin_of("https://example.com/"sv)));
    EXPECT(!WebView::is_web_accessible(extension, "frame.html"sv, origin_of("https://example.org/"sv)));
    EXPECT(!WebView::is_web_accessible(extension, "frame.html"sv, origin_of("http://www.example.com/"sv)));
    EXPECT(!WebView::is_web_accessible(extension, "frame.html"sv, {}));

    EXPECT(WebView::is_web_accessible(extension, "everywhere.css"sv, origin_of("http://127.0.0.1:8000/"sv)));
    EXPECT(!WebView::is_web_accessible(extension, "everywhere.css"sv, URL::Origin::create_opaque()));
    EXPECT(!WebView::is_web_accessible(extension, "secret.html"sv, origin_of("https://www.example.com/"sv)));
}

TEST_CASE(manifest_v3_resources_are_accessible_to_the_extensions_they_name)
{
    auto extension = extension_with_manifest(R"({ "manifest_version": 3, "web_accessible_resources": [
        { "resources": ["for-one.js"], "extension_ids": ["bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"] },
        { "resources": ["for-all.js"], "extension_ids": ["*"] },
        { "resources": ["for-pages.js"], "matches": ["<all_urls>"] }
    ] })"sv);

    auto named = origin_of("chrome-extension://bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb/"sv);
    auto other = origin_of("chrome-extension://cccccccccccccccccccccccccccccccc/"sv);

    EXPECT(WebView::is_web_accessible(extension, "for-one.js"sv, named));
    EXPECT(!WebView::is_web_accessible(extension, "for-one.js"sv, other));
    EXPECT(WebView::is_web_accessible(extension, "for-all.js"sv, other));
    EXPECT(!WebView::is_web_accessible(extension, "for-pages.js"sv, other));
    EXPECT(!WebView::is_web_accessible(extension, "for-all.js"sv, origin_of("https://example.com/"sv)));
}

namespace {

// An extension's directory, ext/, with a file outside it, secret.txt, beside it.
class ExtensionDirectory {
public:
    ExtensionDirectory()
        : m_root(MUST(FileSystem::real_path(m_directory.path().string())))
    {
        MUST(Core::System::mkdir(directory(), 0700));
        MUST(Core::System::mkdir(join("ext/sub"sv), 0700));
        write_file("ext/page.html"sv, "page"sv);
        write_file("ext/sub/script.js"sv, "script"sv);
        write_file("secret.txt"sv, "secret"sv);
    }

    ByteString directory() const { return join("ext"sv); }
    ByteString join(StringView name) const { return LexicalPath::join(m_root, name).string(); }

    void write_file(StringView name, StringView content) const
    {
        auto file = MUST(Core::File::open(join(name), Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(content));
    }

    WebView::LoadedExtension extension(StringView manifest = "{}"sv) const
    {
        auto extension = extension_with_manifest(manifest);
        extension.directory = directory();
        return extension;
    }

private:
    Test::TemporaryDirectory m_directory { "TestExtensions"sv };
    ByteString m_root;
};

}

static ByteString read_file_of(WebView::LoadedExtension const& extension, StringView url_path, WebView::ExtensionFileRequester requester = WebView::ExtensionFileRequester::ExtensionPage)
{
    auto file = WebView::open_extension_file(extension, url_path, requester, origin_of("https://example.com/"sv));
    if (file.is_error())
        return {};
    auto stream = MUST(Core::File::adopt_fd(file.value().take_fd(), Core::File::OpenMode::Read));
    auto bytes = MUST(stream->read_until_eof());
    return ByteString { bytes.bytes() };
}

static void expect_refused(WebView::LoadedExtension const& extension, StringView url_path, WebView::ExtensionFileRequester requester = WebView::ExtensionFileRequester::ExtensionPage)
{
    if (!WebView::open_extension_file(extension, url_path, requester, origin_of("https://example.com/"sv)).is_error())
        FAIL(ByteString::formatted("{} was opened", url_path));
}

TEST_CASE(extension_files_open)
{
    ExtensionDirectory directory;
    auto extension = directory.extension();

    EXPECT_EQ(read_file_of(extension, "/page.html"sv), "page"sv);
    EXPECT_EQ(read_file_of(extension, "/sub/script.js"sv), "script"sv);
    EXPECT_EQ(read_file_of(extension, "/sub/../page.html"sv), "page"sv);
    EXPECT_EQ(read_file_of(extension, "/%70age.html"sv), "page"sv);
}

TEST_CASE(paths_leading_out_of_the_extension_are_refused)
{
    ExtensionDirectory directory;
    auto extension = directory.extension();

    // A URL's path has its dot segments resolved, but not those it spells with percent-encoded characters.
    for (auto path : {
             "/../secret.txt"sv,
             "/sub/../../secret.txt"sv,
             "/%2e%2e/secret.txt"sv,
             "/%2E%2E/secret.txt"sv,
             "/%2e%2e%2fsecret.txt"sv,
             "/..%2fsecret.txt"sv,
             "/sub/%2e%2e/%2e%2e/secret.txt"sv,
             "/sub%2f..%2f..%2fsecret.txt"sv,
             "/..%5csecret.txt"sv,
         }) {
        expect_refused(extension, path);
    }

    // An absolute path in the URL's path stays under the extension's directory.
    auto absolute = ByteString::formatted("/{}", directory.join("secret.txt"sv));
    expect_refused(extension, absolute);
    expect_refused(extension, ByteString::formatted("//{}", directory.join("secret.txt"sv)));
}

TEST_CASE(links_leading_out_of_the_extension_are_refused)
{
    ExtensionDirectory directory;
    auto extension = directory.extension();

    MUST(Core::System::symlink(directory.join("secret.txt"sv), directory.join("ext/link.txt"sv)));
    MUST(Core::System::symlink(directory.join(""sv), directory.join("ext/outside"sv)));
    MUST(Core::System::symlink(directory.join("ext/page.html"sv), directory.join("ext/inside.html"sv)));

    expect_refused(extension, "/link.txt"sv);
    expect_refused(extension, "/outside/secret.txt"sv);
    EXPECT_EQ(read_file_of(extension, "/inside.html"sv), "page"sv);
}

TEST_CASE(web_accessible_globs_do_not_lead_out_of_the_extension)
{
    ExtensionDirectory directory;
    auto extension = directory.extension(R"({ "manifest_version": 3, "web_accessible_resources": [
        { "resources": ["*"], "matches": ["<all_urls>"] }
    ] })"sv);

    EXPECT_EQ(read_file_of(extension, "/page.html"sv, WebView::ExtensionFileRequester::WebPage), "page"sv);
    expect_refused(extension, "/%2e%2e/secret.txt"sv, WebView::ExtensionFileRequester::WebPage);
    expect_refused(extension, "/..%2fsecret.txt"sv, WebView::ExtensionFileRequester::WebPage);

    // Web pages may not read the page the browser generates for the extension's background scripts.
    expect_refused(extension, ByteString::formatted("/{}", WebView::generated_background_page_path), WebView::ExtensionFileRequester::WebPage);
}

TEST_CASE(web_pages_read_only_web_accessible_files)
{
    ExtensionDirectory directory;
    auto extension = directory.extension(R"({ "manifest_version": 3, "web_accessible_resources": [
        { "resources": ["sub/*"], "matches": ["<all_urls>"] }
    ] })"sv);

    EXPECT_EQ(read_file_of(extension, "/sub/script.js"sv, WebView::ExtensionFileRequester::WebPage), "script"sv);
    expect_refused(extension, "/page.html"sv, WebView::ExtensionFileRequester::WebPage);
    expect_refused(extension, "/sub/../page.html"sv, WebView::ExtensionFileRequester::WebPage);
    expect_refused(extension, "/sub/%2e%2e/page.html"sv, WebView::ExtensionFileRequester::WebPage);
    expect_refused(extension, "/sub/..%2fpage.html"sv, WebView::ExtensionFileRequester::WebPage);
    expect_refused(extension, "/sub%2f..%2fpage.html"sv, WebView::ExtensionFileRequester::WebPage);
    EXPECT_EQ(read_file_of(extension, "/page.html/..%2fsub/script.js"sv, WebView::ExtensionFileRequester::WebPage), "script"sv);
}

// A CRX3 file Chromium packed (--pack-extension) from a folder whose only file is this manifest.json:
// { "manifest_version": 3, "name": "Tiny", "version": "1" }
static constexpr u8 tiny_crx[] = {
    0x43, 0x72, 0x32, 0x34, 0x03, 0x00, 0x00, 0x00, 0x45, 0x02, 0x00, 0x00, 0x12, 0xac, 0x04, 0x0a, 0xa6, 0x02, 0x30,
    0x82, 0x01, 0x22, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00, 0x03,
    0x82, 0x01, 0x0f, 0x00, 0x30, 0x82, 0x01, 0x0a, 0x02, 0x82, 0x01, 0x01, 0x00, 0xa7, 0xf0, 0xba, 0x4f, 0xf6, 0xd9,
    0x7a, 0x85, 0xb5, 0xe1, 0x5c, 0x79, 0x17, 0x54, 0xec, 0xb4, 0x1e, 0x0f, 0xde, 0xb6, 0xbf, 0x4c, 0x15, 0x33, 0x07,
    0x07, 0x8d, 0x74, 0x3d, 0xc2, 0x74, 0xe4, 0x41, 0x38, 0x3e, 0xa1, 0xd1, 0xd5, 0x57, 0xe3, 0xe8, 0xa6, 0x6e, 0x82,
    0xc8, 0xe3, 0xc3, 0xab, 0x2c, 0x7d, 0x26, 0x7b, 0xf4, 0x47, 0x21, 0x9d, 0x98, 0x37, 0x43, 0x15, 0x61, 0x0d, 0x80,
    0x8e, 0xbc, 0x79, 0x63, 0x9b, 0xd6, 0xdf, 0x2e, 0x5b, 0xee, 0xb9, 0x9e, 0x80, 0xd2, 0x59, 0xe4, 0x69, 0x22, 0x6f,
    0x0f, 0xd8, 0x80, 0xb4, 0x6f, 0x61, 0x93, 0x2a, 0x47, 0xc5, 0xa0, 0xb3, 0x61, 0x5d, 0xe4, 0xb2, 0xe1, 0xf0, 0xd4,
    0x82, 0x1c, 0xe4, 0x01, 0x12, 0x21, 0x39, 0x49, 0xb6, 0x9a, 0xb2, 0x0d, 0x83, 0x0c, 0x78, 0x6e, 0x19, 0xea, 0x47,
    0x9a, 0x65, 0xb0, 0x36, 0xff, 0x1c, 0x67, 0x3d, 0x01, 0xd8, 0xb3, 0x72, 0xef, 0xb9, 0x48, 0xd8, 0x81, 0xca, 0x9f,
    0xc8, 0x55, 0x0f, 0xf3, 0xea, 0x03, 0x93, 0x67, 0x68, 0x32, 0x57, 0x3b, 0xb4, 0x67, 0xe7, 0xbe, 0xdf, 0xf7, 0x7a,
    0x61, 0x49, 0x23, 0xde, 0x0f, 0x5a, 0x68, 0x92, 0x59, 0xe7, 0x22, 0xa6, 0xec, 0x7b, 0x96, 0xdf, 0x4d, 0x1d, 0xe2,
    0x2a, 0xc6, 0xe2, 0x68, 0xcd, 0x4a, 0x07, 0x81, 0xd2, 0x92, 0x51, 0x4a, 0x2f, 0xdd, 0xa5, 0x1e, 0x24, 0x6b, 0x76,
    0x5f, 0x3a, 0xfd, 0x7b, 0x1e, 0x59, 0xe2, 0x2b, 0xd8, 0xec, 0xca, 0xad, 0x7b, 0x93, 0xfc, 0x3b, 0xba, 0x71, 0xd6,
    0x67, 0x47, 0x83, 0x80, 0x26, 0x8c, 0xee, 0x71, 0xf3, 0x0e, 0xee, 0x03, 0xb9, 0x16, 0xb7, 0xf1, 0xb0, 0x4c, 0xef,
    0x43, 0x38, 0x80, 0x37, 0xe3, 0xa6, 0xc9, 0x63, 0xe3, 0x35, 0xf7, 0x66, 0xde, 0x47, 0xd5, 0xb9, 0x0c, 0x0a, 0xba,
    0xb8, 0xcc, 0xa9, 0x02, 0x03, 0x01, 0x00, 0x01, 0x12, 0x80, 0x02, 0x8f, 0x4d, 0x2b, 0xf5, 0xb1, 0xd1, 0xdc, 0x24,
    0x05, 0x3d, 0xd7, 0xcf, 0xa9, 0x21, 0xde, 0xcd, 0xf5, 0x74, 0x9a, 0xe0, 0x55, 0x25, 0x9f, 0x50, 0x50, 0x69, 0xa2,
    0x16, 0xc1, 0xbc, 0x2c, 0x78, 0x99, 0x29, 0x66, 0xfd, 0x75, 0xc4, 0x12, 0x7e, 0xae, 0x30, 0x40, 0x60, 0x65, 0xda,
    0xbc, 0x7f, 0x48, 0xe4, 0x4a, 0xe0, 0xdb, 0xb3, 0xb5, 0xd3, 0xf3, 0x23, 0x8b, 0x3b, 0x04, 0xc9, 0x99, 0x88, 0x6d,
    0x1e, 0x76, 0xc8, 0xea, 0x6f, 0x8c, 0xb8, 0xd3, 0xf0, 0xb9, 0x3f, 0x0b, 0xa2, 0x21, 0x42, 0x45, 0xa2, 0x5a, 0x40,
    0x22, 0x3c, 0xa7, 0x15, 0x47, 0xd6, 0x36, 0xb5, 0xcd, 0x51, 0x97, 0xe8, 0x75, 0x48, 0x68, 0x82, 0xf8, 0xa0, 0xa1,
    0x82, 0xb5, 0x35, 0x10, 0x53, 0x73, 0xc3, 0xb4, 0x46, 0x94, 0xb2, 0x26, 0x92, 0x79, 0xfd, 0x06, 0x17, 0xb1, 0xd3,
    0x7b, 0x02, 0xaa, 0xdf, 0x70, 0xdc, 0x28, 0xe9, 0xbe, 0x79, 0x87, 0xb1, 0x05, 0xd0, 0xa4, 0x0a, 0xf1, 0xf1, 0x04,
    0xba, 0xb1, 0x2a, 0x5b, 0x11, 0xfb, 0x57, 0xb6, 0xa3, 0x40, 0x35, 0x4f, 0xc3, 0x66, 0x81, 0x2b, 0x58, 0xc9, 0x0e,
    0x50, 0xf9, 0xb0, 0xd1, 0x37, 0x48, 0x5e, 0x98, 0x85, 0x68, 0x45, 0xc0, 0x29, 0xfc, 0x16, 0xd1, 0xeb, 0x8e, 0xf2,
    0xa8, 0xb2, 0x83, 0x0d, 0x63, 0xc7, 0xa3, 0x55, 0x18, 0x88, 0x24, 0x93, 0xd5, 0x0c, 0x70, 0xb5, 0x9f, 0xf3, 0x11,
    0xbf, 0x94, 0x84, 0x46, 0xb6, 0x58, 0x27, 0x0b, 0x78, 0xb3, 0xbe, 0x8d, 0x11, 0x60, 0xe4, 0xf0, 0xf9, 0xf8, 0xce,
    0x3f, 0xa9, 0xe1, 0x79, 0x14, 0xb6, 0xbd, 0xa9, 0x4b, 0xfd, 0x59, 0x04, 0x04, 0x86, 0xa8, 0x7e, 0x25, 0x7e, 0xa8,
    0xce, 0xc7, 0xfd, 0x2f, 0x7f, 0xf7, 0x9b, 0x25, 0x15, 0x9f, 0xca, 0x26, 0x6d, 0x4b, 0xde, 0xda, 0xbc, 0x70, 0xa8,
    0xb2, 0x82, 0xf1, 0x04, 0x12, 0x0a, 0x10, 0xd2, 0x86, 0xf6, 0xa0, 0xa2, 0xeb, 0x6e, 0x5a, 0x39, 0x51, 0x2e, 0x6e,
    0x95, 0xd5, 0xa6, 0x2a, 0x50, 0x4b, 0x03, 0x04, 0x2d, 0x00, 0x00, 0x08, 0x08, 0x00, 0x37, 0x77, 0x7a, 0x5d, 0x24,
    0x29, 0x94, 0xb7, 0x34, 0x00, 0x00, 0x00, 0x3a, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x14, 0x00, 0x6d, 0x61, 0x6e, 0x69,
    0x66, 0x65, 0x73, 0x74, 0x2e, 0x6a, 0x73, 0x6f, 0x6e, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xab, 0x56, 0x50, 0xca, 0x4d, 0xcc, 0xcb, 0x4c, 0x4b,
    0x2d, 0x2e, 0x89, 0x2f, 0x4b, 0x2d, 0x2a, 0xce, 0xcc, 0xcf, 0x53, 0xb2, 0x52, 0x30, 0xd6, 0x51, 0x50, 0xca, 0x4b,
    0xcc, 0x4d, 0x55, 0xb2, 0x52, 0x50, 0x0a, 0xc9, 0xcc, 0xab, 0x54, 0xd2, 0x51, 0x50, 0x42, 0xc8, 0x2a, 0x19, 0x2a,
    0x29, 0xd4, 0x72, 0x01, 0x00, 0x50, 0x4b, 0x01, 0x02, 0x00, 0x00, 0x14, 0x00, 0x00, 0x08, 0x08, 0x00, 0x37, 0x77,
    0x7a, 0x5d, 0x24, 0x29, 0x94, 0xb7, 0x34, 0x00, 0x00, 0x00, 0x3a, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6d, 0x61, 0x6e, 0x69, 0x66, 0x65,
    0x73, 0x74, 0x2e, 0x6a, 0x73, 0x6f, 0x6e, 0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x3b, 0x00, 0x00, 0x00, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00
};
static constexpr auto tiny_crx_id = "ncigpgkakcolgofkdjfbcogojfnfkgck"sv;

struct ZipFile {
    StringView name;
    StringView contents;
};

// Builds a zip archive of stored (uncompressed) files.
static ByteBuffer zip_of(Vector<ZipFile> const& files)
{
    ByteBuffer archive;
    ByteBuffer central_directory;
    auto append_u16 = [](ByteBuffer& buffer, u16 value) { buffer.append(&value, sizeof(value)); };
    auto append_u32 = [](ByteBuffer& buffer, u32 value) { buffer.append(&value, sizeof(value)); };

    for (auto const& [name, contents] : files) {
        auto offset = static_cast<u32>(archive.size());
        auto crc = Compress::crc32(contents.bytes());
        auto size = static_cast<u32>(contents.length());

        append_u32(archive, 0x04034b50);
        for (u16 field : { 20, 0, 0, 0, 0 })
            append_u16(archive, field); // Version, flags, method, time, date.
        append_u32(archive, crc);
        append_u32(archive, size);
        append_u32(archive, size);
        append_u16(archive, static_cast<u16>(name.length()));
        append_u16(archive, 0);
        archive.append(name.bytes());
        archive.append(contents.bytes());

        append_u32(central_directory, 0x02014b50);
        for (u16 field : { 20, 20, 0, 0, 0, 0 })
            append_u16(central_directory, field); // Versions, flags, method, time, date.
        append_u32(central_directory, crc);
        append_u32(central_directory, size);
        append_u32(central_directory, size);
        append_u16(central_directory, static_cast<u16>(name.length()));
        for (u16 field : { 0, 0, 0, 0 })
            append_u16(central_directory, field); // Extra and comment lengths, disk, internal attributes.
        append_u32(central_directory, 0);
        append_u32(central_directory, offset);
        central_directory.append(name.bytes());
    }

    auto central_directory_offset = static_cast<u32>(archive.size());
    archive.append(central_directory);
    append_u32(archive, 0x06054b50);
    append_u16(archive, 0);
    append_u16(archive, 0);
    append_u16(archive, static_cast<u16>(files.size()));
    append_u16(archive, static_cast<u16>(files.size()));
    append_u32(archive, static_cast<u32>(central_directory.size()));
    append_u32(archive, central_directory_offset);
    append_u16(archive, 0);
    return archive;
}

static ByteString contents_of(LexicalPath const& path)
{
    auto file = MUST(Core::File::open(path.string(), Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    return ByteString { bytes.bytes() };
}

static void write_file(LexicalPath const& path, ReadonlyBytes contents)
{
    auto file = MUST(Core::File::open(path.string(), Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(contents));
}

TEST_CASE(crx_files_from_chromium_are_read_and_their_signatures_checked)
{
    auto crx = MUST(WebView::read_crx({ tiny_crx, sizeof(tiny_crx) }));
    EXPECT_EQ(WebView::extension_id_for_public_key(crx.public_key), tiny_crx_id);

    Test::TemporaryDirectory directory { "TestExtensions"sv };
    MUST(WebView::extract_zip(crx.archive, directory.path().string()));
    EXPECT(contents_of(directory.path().append("manifest.json"sv)).contains("\"Tiny\""sv));
}

TEST_CASE(crx_files_that_were_changed_are_refused)
{
    // A byte of the archive, which the signature covers.
    auto changed = MUST(ByteBuffer::copy(ReadonlyBytes { tiny_crx, sizeof(tiny_crx) }));
    changed[changed.size() - 30] ^= 1;
    EXPECT(WebView::read_crx(changed).is_error());

    // The old CRX2 format, and files that are cut short.
    auto crx2 = MUST(ByteBuffer::copy(ReadonlyBytes { tiny_crx, sizeof(tiny_crx) }));
    crx2[4] = 2;
    EXPECT(WebView::read_crx(crx2).is_error());
    EXPECT(WebView::read_crx(ReadonlyBytes { tiny_crx, 100 }).is_error());
    EXPECT(WebView::read_crx("Cr24"sv.bytes()).is_error());
}

TEST_CASE(unpacked_crx_files_keep_their_extension_id)
{
    Test::TemporaryDirectory directory { "TestExtensions"sv };
    auto package = directory.path().append("tiny.crx"sv);
    write_file(package, { tiny_crx, sizeof(tiny_crx) });

    auto packages = directory.path().append("Packed"sv).string();
    auto unpacked = MUST(WebView::unpack_extension_package(package.string(), packages));
    auto extension = MUST(WebView::load_unpacked_extension(unpacked));
    EXPECT_EQ(extension.id, tiny_crx_id);
    EXPECT_EQ(LexicalPath { unpacked }.basename(), tiny_crx_id);

    // Unpacking it again replaces the files, and leaves nothing else behind.
    EXPECT_EQ(MUST(WebView::unpack_extension_package(package.string(), packages)), unpacked);
    size_t entries = 0;
    MUST(Core::Directory::for_each_entry(packages, Core::DirIterator::SkipParentAndBaseDir, [&](auto const&, auto const&) {
        ++entries;
        return IterationDecision::Continue;
    }));
    EXPECT_EQ(entries, 1u);
}

TEST_CASE(zip_files_are_unpacked)
{
    Test::TemporaryDirectory directory { "TestExtensions"sv };
    auto package = directory.path().append("ext.zip"sv);
    auto archive = zip_of({
        { "manifest.json"sv, R"({ "manifest_version": 3, "name": "Zipped", "version": "2" })"sv },
        { "scripts/"sv, ""sv },
        { "scripts/cs.js"sv, "console.log(1);"sv },
    });
    write_file(package, archive);

    auto packages = directory.path().append("Packed"sv).string();
    auto unpacked = MUST(WebView::unpack_extension_package(package.string(), packages));
    auto extension = MUST(WebView::load_unpacked_extension(unpacked));
    EXPECT_EQ(extension.name, "Zipped"sv);
    EXPECT_EQ(contents_of(LexicalPath { unpacked }.append("scripts/cs.js"sv)), "console.log(1);"sv);

    // The same archive keeps its ID when installed again.
    EXPECT_EQ(MUST(WebView::load_unpacked_extension(MUST(WebView::unpack_extension_package(package.string(), packages)))).id, extension.id);
}

TEST_CASE(zip_files_of_an_extension_folder_are_unpacked_without_the_folder)
{
    Test::TemporaryDirectory directory { "TestExtensions"sv };
    auto archive = zip_of({
        { "ext-1.0/manifest.json"sv, R"({ "manifest_version": 3, "name": "Foldered", "version": "1" })"sv },
        { "ext-1.0/cs.js"sv, "1"sv },
        { "__MACOSX/ext-1.0/._cs.js"sv, "resource fork"sv },
    });
    MUST(WebView::extract_zip(archive, directory.path().string()));
    EXPECT(FileSystem::exists(directory.path().append("manifest.json"sv).string()));
    EXPECT(FileSystem::exists(directory.path().append("cs.js"sv).string()));
    EXPECT(!FileSystem::exists(directory.path().append("__MACOSX"sv).string()));
}

TEST_CASE(zip_files_may_not_write_outside_their_folder)
{
    for (auto name : { "../escape.txt"sv, "/tmp/escape.txt"sv, "sub/../../escape.txt"sv, "sub\\..\\..\\escape.txt"sv, "./manifest.json"sv }) {
        Test::TemporaryDirectory directory { "TestExtensions"sv };
        auto target = directory.path().append("target"sv);
        MUST(Core::System::mkdir(target.string(), 0755));
        auto archive = zip_of({ { "manifest.json"sv, "{}"sv }, { name, "escaped"sv } });
        EXPECT(WebView::extract_zip(archive, target.string()).is_error());
        EXPECT(!FileSystem::exists(directory.path().append("escape.txt"sv).string()));
    }
}

TEST_CASE(damaged_packages_are_refused)
{
    Test::TemporaryDirectory directory { "TestExtensions"sv };

    // A file whose checksum does not match.
    auto archive = zip_of({ { "manifest.json"sv, "{}"sv } });
    archive[30 + "manifest.json"sv.length()] = '[';
    EXPECT(WebView::extract_zip(archive, directory.path().string()).is_error());

    // Neither a zip archive nor a .crx file.
    auto package = directory.path().append("ext.zip"sv);
    write_file(package, "not an archive"sv.bytes());
    EXPECT(WebView::unpack_extension_package(package.string(), directory.path().append("Packed"sv).string()).is_error());

    // An archive without a manifest leaves nothing behind.
    auto no_manifest = zip_of({ { "readme.txt"sv, "hi"sv } });
    write_file(package, no_manifest);
    auto packages = directory.path().append("Packed"sv);
    EXPECT(WebView::unpack_extension_package(package.string(), packages.string()).is_error());
    size_t entries = 0;
    MUST(Core::Directory::for_each_entry(packages.string(), Core::DirIterator::SkipParentAndBaseDir, [&](auto const&, auto const&) {
        ++entries;
        return IterationDecision::Continue;
    }));
    EXPECT_EQ(entries, 0u);
}

TEST_CASE(manifest_keys_give_the_extension_id)
{
    Test::TemporaryDirectory directory { "TestExtensions"sv };
    auto crx = MUST(WebView::read_crx({ tiny_crx, sizeof(tiny_crx) }));
    auto manifest = ByteString::formatted(R"({{ "manifest_version": 3, "name": "Keyed", "version": "1", "key": "{}" }})", MUST(encode_base64(crx.public_key)));
    write_file(directory.path().append("manifest.json"sv), manifest.bytes());
    EXPECT_EQ(MUST(WebView::load_unpacked_extension(directory.path().string())).id, tiny_crx_id);

    write_file(directory.path().append("manifest.json"sv), R"({ "manifest_version": 3, "name": "Keyed", "version": "1", "key": "%%%" })"sv.bytes());
    EXPECT(WebView::load_unpacked_extension(directory.path().string()).is_error());
}

TEST_CASE(manifests_name_required_and_optional_permissions)
{
    auto extension = extension_with_manifest(R"~~~({"manifest_version": 3,
        "permissions": ["storage", "tabs"], "host_permissions": ["https://example.com/*"],
        "optional_permissions": ["history"], "optional_host_permissions": ["https://*.example.org/*"]})~~~"sv);
    auto required = WebView::required_permissions_of(extension);
    EXPECT_EQ(required.permissions, (Vector<String> { "storage"_string, "tabs"_string }));
    EXPECT_EQ(required.origins, (Vector<String> { "https://example.com/*"_string }));
    auto optional = WebView::optional_permissions_of(extension);
    EXPECT_EQ(optional.permissions, (Vector<String> { "history"_string }));
    EXPECT_EQ(optional.origins, (Vector<String> { "https://*.example.org/*"_string }));

    // Manifest V2 names hosts among the permissions.
    auto v2 = extension_with_manifest(R"~~~({"manifest_version": 2,
        "permissions": ["storage", "<all_urls>"], "optional_permissions": ["bookmarks", "https://example.net/*"]})~~~"sv);
    EXPECT_EQ(WebView::required_permissions_of(v2).permissions, (Vector<String> { "storage"_string }));
    EXPECT_EQ(WebView::required_permissions_of(v2).origins, (Vector<String> { "<all_urls>"_string }));
    EXPECT_EQ(WebView::optional_permissions_of(v2).permissions, (Vector<String> { "bookmarks"_string }));
    EXPECT_EQ(WebView::optional_permissions_of(v2).origins, (Vector<String> { "https://example.net/*"_string }));
}

TEST_CASE(granted_optional_permissions_are_held)
{
    auto extension = extension_with_manifest(R"~~~({"manifest_version": 3,
        "optional_permissions": ["history"], "optional_host_permissions": ["https://example.org/*"]})~~~"sv);
    auto url = URL::Parser::basic_parse("https://example.org/page"sv).release_value();

    EXPECT(!WebView::extension_has_permission(extension, "history"sv));
    EXPECT(!WebView::extension_has_host_access(extension, url, WebView::HostAccess::Explicit));

    extension.granted_permissions.append("history"_string);
    extension.granted_origins.append("https://example.org/*"_string);
    EXPECT(WebView::extension_has_permission(extension, "history"sv));
    EXPECT(WebView::extension_has_host_access(extension, url, WebView::HostAccess::Explicit));
    EXPECT(WebView::extension_may_see_tab_with_url(extension, url));
    EXPECT(!WebView::extension_has_host_access(extension, URL::Parser::basic_parse("https://example.com/"sv).release_value(), WebView::HostAccess::Explicit));
}

TEST_CASE(match_patterns_cover_narrower_ones)
{
    EXPECT(WebView::match_pattern_covers("https://example.com/*"sv, "https://example.com/*"sv));
    EXPECT(WebView::match_pattern_covers("<all_urls>"sv, "https://example.com/*"sv));
    EXPECT(WebView::match_pattern_covers("*://*/*"sv, "https://*.example.com/*"sv));
    EXPECT(WebView::match_pattern_covers("https://*.example.org/*"sv, "https://www.example.org/*"sv));
    EXPECT(WebView::match_pattern_covers("https://example.com/*"sv, "https://example.com/docs/*"sv));

    EXPECT(!WebView::match_pattern_covers("https://example.com/*"sv, "https://example.net/*"sv));
    EXPECT(!WebView::match_pattern_covers("https://example.com/docs/*"sv, "https://example.com/*"sv));
    EXPECT(!WebView::match_pattern_covers("https://www.example.org/*"sv, "https://*.example.org/*"sv));
    EXPECT(!WebView::match_pattern_covers("*://*/*"sv, "file:///*"sv));
}
