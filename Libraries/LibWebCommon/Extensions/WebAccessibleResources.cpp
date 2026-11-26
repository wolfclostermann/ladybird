/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/String.h>
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebCommon/Extensions/WebAccessibleResources.h>

namespace Web::Extensions {

static Vector<String> strings_of(JsonObject const& object, StringView key)
{
    Vector<String> strings;
    if (auto array = object.get_array(key); array.has_value()) {
        for (auto const& value : array->values()) {
            if (value.is_string())
                strings.append(value.as_string());
        }
    }
    return strings;
}

// Whether an entry of Manifest V3's web_accessible_resources lets what made a request, of an origin, use its resources:
// its "matches" name the sites that may, and its "extension_ids" the extensions, or "*" for all of them.
static bool entry_allows(JsonObject const& entry, Optional<URL::Origin> const& requesting_origin)
{
    // NB: An opaque origin, such as a sandboxed frame's, has no site to match, so only the extension's own pages, which
    //     are not asked, may then load the resources. Chromium goes by the origin the opaque one came from.
    if (!requesting_origin.has_value() || requesting_origin->is_opaque())
        return false;

    if (requesting_origin->scheme() == "chrome-extension"sv) {
        auto id = requesting_origin->host().serialize();
        return any_of(strings_of(entry, "extension_ids"sv), [&](auto const& allowed) { return allowed == "*"sv || allowed == id; });
    }

    // Match patterns of web_accessible_resources have the path "/*", so they are matched against the origin's root.
    auto url = URL::Parser::basic_parse(MUST(String::formatted("{}/", requesting_origin->serialize())));
    if (!url.has_value())
        return false;
    return any_of(strings_of(entry, "matches"sv), [&](auto const& pattern) { return match_pattern_matches(pattern, *url); });
}

bool is_web_accessible_resource(JsonObject const& manifest, StringView path, Optional<URL::Origin> const& requesting_origin)
{
    auto resources = manifest.get_array("web_accessible_resources"sv);
    if (!resources.has_value())
        return false;

    auto matches = [&](StringView glob) {
        return path.matches(glob.trim("/"sv, TrimMode::Left), CaseSensitivity::CaseSensitive);
    };

    for (auto const& entry : resources->values()) {
        // Manifest V2 lists the resources themselves, which every page may use.
        if (entry.is_string() && matches(entry.as_string()))
            return true;
        // Manifest V3 lists them with the sites and extensions that may use them.
        // FIXME: Support "use_dynamic_url", which serves the resources at a URL that changes each session.
        if (!entry.is_object())
            continue;
        auto const& object = entry.as_object();
        if (any_of(strings_of(object, "resources"sv), [&](auto const& glob) { return matches(glob); })
            && entry_allows(object, requesting_origin))
            return true;
    }
    return false;
}

}
