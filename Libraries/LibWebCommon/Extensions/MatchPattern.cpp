/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/MatchPattern.h>

namespace Web::Extensions {

// Whether text matches glob, in which "*" matches any run of characters, including none.
static bool glob_matches(StringView glob, StringView text)
{
    size_t glob_index = 0;
    size_t text_index = 0;
    Optional<size_t> star_index;
    size_t text_index_at_star = 0;

    while (text_index < text.length()) {
        if (glob_index < glob.length() && glob[glob_index] == '*') {
            star_index = glob_index++;
            text_index_at_star = text_index;
        } else if (glob_index < glob.length() && glob[glob_index] == text[text_index]) {
            ++glob_index;
            ++text_index;
        } else if (star_index.has_value()) {
            glob_index = *star_index + 1;
            text_index = ++text_index_at_star;
        } else {
            return false;
        }
    }

    while (glob_index < glob.length() && glob[glob_index] == '*')
        ++glob_index;
    return glob_index == glob.length();
}

// https://developer.chrome.com/docs/extensions/develop/concepts/match-patterns
bool match_pattern_matches(StringView pattern, URL::URL const& url)
{
    auto scheme = url.scheme();

    if (pattern == "<all_urls>"sv)
        return scheme.is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv, "ftp"sv, "file"sv, "data"sv);

    auto scheme_separator = pattern.find("://"sv);
    if (!scheme_separator.has_value())
        return false;

    auto pattern_scheme = pattern.substring_view(0, *scheme_separator);
    auto rest = pattern.substring_view(*scheme_separator + 3);

    if (pattern_scheme == "*"sv) {
        if (!scheme.is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv))
            return false;
    } else if (pattern_scheme != scheme) {
        return false;
    }

    // A pattern has a path, if only "/".
    auto path_start = rest.find('/');
    if (!path_start.has_value())
        return false;
    auto pattern_host = rest.substring_view(0, *path_start);
    auto pattern_path = rest.substring_view(*path_start);

    if (scheme == "file"sv) {
        if (!pattern_host.is_empty() && pattern_host != "*"sv)
            return false;
    } else {
        // A host may name a port, which the URL then has to be on. Without one, any port matches.
        Optional<StringView> pattern_port;
        if (auto port_separator = pattern_host.find_last(':'); port_separator.has_value() && !pattern_host.ends_with(']')) {
            pattern_port = pattern_host.substring_view(*port_separator + 1);
            pattern_host = pattern_host.substring_view(0, *port_separator);
        }

        auto host = url.serialized_host();
        if (pattern_host == "*"sv) {
            // Any host matches.
        } else if (pattern_host.starts_with("*."sv)) {
            auto domain = pattern_host.substring_view(2);
            if (!host.equals_ignoring_ascii_case(domain) && !(host.length() > domain.length() && host.ends_with(domain, CaseSensitivity::CaseInsensitive) && host[host.length() - domain.length() - 1] == '.'))
                return false;
        } else if (!host.equals_ignoring_ascii_case(pattern_host)) {
            return false;
        }

        if (pattern_port.has_value() && *pattern_port != "*"sv) {
            if (String::number(url.port_or_default()) != *pattern_port)
                return false;
        }
    }

    // The path of a pattern is matched against the path of the URL with its query.
    StringBuilder path;
    path.append(url.serialize_path());
    if (auto query = url.query(); query.has_value()) {
        path.append('?');
        path.append(*query);
    }
    return glob_matches(pattern_path, path.string_view());
}

}
