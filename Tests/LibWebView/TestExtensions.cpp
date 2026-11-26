/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
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
