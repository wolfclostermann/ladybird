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
#include <AK/JsonArray.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibIPC/File.h>
#include <LibURL/Forward.h>
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

// Reads a text file of an extension, at a path relative to its directory, which the path may not leave.
WEBVIEW_API ErrorOr<String> read_extension_text_file(LoadedExtension const&, StringView relative_path);

// The page that hosts an extension's background scripts, if it has any. For background scripts or a service worker,
// it is generated, at this path.
inline constexpr StringView generated_background_page_path = "_generated_background_page.html"sv;
WEBVIEW_API Optional<String> background_page_path(LoadedExtension const&);

enum class ExtensionFileRequester : u8 {
    // The extension's own pages may read any of its files.
    ExtensionPage,
    // Other pages may read only the files it makes web accessible.
    WebPage,
};

// The path at which an extension with the "favicon" permission finds the favicons of pages, as
// _favicon/?pageUrl=<url>&size=<pixels>.
// https://developer.chrome.com/docs/extensions/how-to/ui/favicons
inline constexpr StringView favicon_path = "_favicon"sv;

// https://developer.chrome.com/docs/extensions/develop/concepts/declare-permissions
WEBVIEW_API bool extension_has_permission(LoadedExtension const&, StringView permission);

enum class HostAccess : u8 {
    // The hosts that the manifest's host permissions name, which scripting needs.
    Explicit,
    // Those, and the hosts its content scripts match, which let it see the URLs and titles of tabs.
    IncludingContentScripts,
};
// Whether an extension may access a URL: its own, or one its host permissions match.
// https://developer.chrome.com/docs/extensions/develop/concepts/declare-permissions#host-permissions
WEBVIEW_API bool extension_has_host_access(LoadedExtension const&, URL::URL const&, HostAccess);
// Whether an extension may see the URL and title of a tab that shows a URL: with the "tabs" permission or host access.
WEBVIEW_API bool extension_may_see_tab_with_url(LoadedExtension const&, URL::URL const&);
// Whether web pages may load the file at the path, relative to the extension's root.
WEBVIEW_API bool is_web_accessible(LoadedExtension const&, StringView path);

// A file a renderer can read the bytes from.
WEBVIEW_API ErrorOr<IPC::File> file_with_contents(ReadonlyBytes);

// Opens the file that a chrome-extension: URL's path names, for a renderer.
WEBVIEW_API ErrorOr<IPC::File> open_extension_file(LoadedExtension const&, StringView url_path, ExtensionFileRequester);

// The extensions as JSON, for a WebContent process.
WEBVIEW_API String serialize_extensions_for_web_content(Vector<LoadedExtension> const&);

// JSON with the comments that Chromium accepts in a manifest taken out.
WEBVIEW_API String strip_json_comments(StringView);

}
