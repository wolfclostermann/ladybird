/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonParser.h>
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
