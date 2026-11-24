/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/Extensions/ContentScripts.h>

using Web::Extensions::match_pattern_matches;

static URL::URL url(StringView string)
{
    auto result = URL::Parser::basic_parse(string);
    EXPECT(result.has_value());
    return result.release_value();
}

TEST_CASE(all_urls)
{
    EXPECT(match_pattern_matches("<all_urls>"sv, url("https://example.com/"sv)));
    EXPECT(match_pattern_matches("<all_urls>"sv, url("http://example.com:8080/a?b"sv)));
    EXPECT(match_pattern_matches("<all_urls>"sv, url("file:///Users/someone/page.html"sv)));
    EXPECT(!match_pattern_matches("<all_urls>"sv, url("about:blank"sv)));
    EXPECT(!match_pattern_matches("<all_urls>"sv, url("chrome-extension://abc/page.html"sv)));
}

TEST_CASE(scheme)
{
    EXPECT(match_pattern_matches("*://example.com/*"sv, url("http://example.com/"sv)));
    EXPECT(match_pattern_matches("*://example.com/*"sv, url("https://example.com/"sv)));
    EXPECT(!match_pattern_matches("*://example.com/*"sv, url("file:///example.com/"sv)));
    EXPECT(match_pattern_matches("https://example.com/*"sv, url("https://example.com/"sv)));
    EXPECT(!match_pattern_matches("https://example.com/*"sv, url("http://example.com/"sv)));
}

TEST_CASE(host)
{
    EXPECT(match_pattern_matches("https://*/*"sv, url("https://anything.test/"sv)));
    EXPECT(match_pattern_matches("https://*.example.com/*"sv, url("https://example.com/"sv)));
    EXPECT(match_pattern_matches("https://*.example.com/*"sv, url("https://a.b.example.com/"sv)));
    EXPECT(!match_pattern_matches("https://*.example.com/*"sv, url("https://notexample.com/"sv)));
    EXPECT(!match_pattern_matches("https://example.com/*"sv, url("https://www.example.com/"sv)));
    EXPECT(match_pattern_matches("https://EXAMPLE.com/*"sv, url("https://example.com/"sv)));
}

TEST_CASE(port)
{
    EXPECT(match_pattern_matches("http://localhost/*"sv, url("http://localhost:8000/"sv)));
    EXPECT(match_pattern_matches("http://localhost:8000/*"sv, url("http://localhost:8000/"sv)));
    EXPECT(!match_pattern_matches("http://localhost:8000/*"sv, url("http://localhost:9000/"sv)));
    EXPECT(match_pattern_matches("http://localhost:80/*"sv, url("http://localhost/"sv)));
}

TEST_CASE(path)
{
    EXPECT(match_pattern_matches("https://example.com/foo*"sv, url("https://example.com/foobar"sv)));
    EXPECT(!match_pattern_matches("https://example.com/foo*"sv, url("https://example.com/bar"sv)));
    EXPECT(match_pattern_matches("https://example.com/*/c"sv, url("https://example.com/a/b/c"sv)));
    EXPECT(match_pattern_matches("https://example.com/*?q=1"sv, url("https://example.com/x?q=1"sv)));
    EXPECT(!match_pattern_matches("https://example.com/"sv, url("https://example.com/x"sv)));
    EXPECT(match_pattern_matches("file:///*"sv, url("file:///tmp/a.html"sv)));
    EXPECT(match_pattern_matches("file:///"sv, url("file:///"sv)));
    EXPECT(!match_pattern_matches("file:///"sv, url("file:///tmp/"sv)));
}

TEST_CASE(malformed)
{
    EXPECT(!match_pattern_matches(""sv, url("https://example.com/"sv)));
    EXPECT(!match_pattern_matches("example.com"sv, url("https://example.com/"sv)));
    EXPECT(!match_pattern_matches("https://example.com"sv, url("https://example.com/"sv)));
}
