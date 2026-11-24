/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/JsonObject.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/JsonArray.h>
#include <LibWebView/Export.h>

// WebExtensions, loaded from directories that hold them unpacked: https://developer.chrome.com/docs/extensions
namespace WebView {

struct LoadedExtension {
    // Like Chromium, an unpacked extension's ID derives from the path of its directory, so it stays the same across
    // runs.
    String id;
    String name;
    String version;
    ByteString directory;
    JsonObject manifest;

    // The content scripts, with the sources of their files, as the WebContent processes take them.
    JsonArray content_scripts;
};

WEBVIEW_API ErrorOr<LoadedExtension> load_unpacked_extension(StringView directory);

// The extensions as JSON, for a WebContent process.
WEBVIEW_API String serialize_extensions_for_web_content(Vector<LoadedExtension> const&);

// JSON with the comments that Chromium accepts in a manifest taken out.
WEBVIEW_API String strip_json_comments(StringView);

}
