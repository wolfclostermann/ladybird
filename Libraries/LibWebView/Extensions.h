/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibIPC/File.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/UIEvents/KeyCode.h>
#include <LibWebView/Export.h>

// WebExtensions, loaded from directories that hold them unpacked: https://developer.chrome.com/docs/extensions
namespace WebView {

struct LoadedExtension {
    // Like Chromium, an unpacked extension's ID derives from the manifest's "key" if it has one, else from the path of
    // its directory, so it stays the same across runs.
    String id;
    String name;
    String version;
    ByteString directory;
    JsonObject manifest;

    // The content scripts, with the sources of their files, as the WebContent processes take them.
    JsonArray content_scripts;

    // The optional permissions and host permissions the user granted, with permissions.request.
    // https://developer.chrome.com/docs/extensions/reference/api/permissions
    Vector<String> granted_permissions;
    Vector<String> granted_origins;
};

// An extension that could not be loaded, and why.
struct ExtensionLoadError {
    ByteString directory;
    String error;
};

// Loads the extension in a directory. Its ID is id_without_key, if given, when the manifest has no "key".
WEBVIEW_API ErrorOr<LoadedExtension> load_unpacked_extension(StringView directory, Optional<String> const& id_without_key = {});

// Reads a text file of an extension, at a path relative to its directory, which the path may not leave.
WEBVIEW_API ErrorOr<String> read_extension_text_file(LoadedExtension const&, StringView relative_path);

// Reads any file of an extension, at a path relative to its directory, which the path may not leave.
WEBVIEW_API ErrorOr<ByteBuffer> read_extension_file(LoadedExtension const&, StringView relative_path);

// Of icons keyed by their sizes, as manifests list them, the path of the one that best shows at a size, in pixels.
WEBVIEW_API Optional<String> best_icon_path(JsonObject const& icons, int size);

// The path of the icon that best shows an extension at a size, in pixels: one of its own, or else its toolbar
// button's.
WEBVIEW_API Optional<String> icon_path(LoadedExtension const&, int size);

// The manifest's description of the extension's toolbar button, if it has one: "action", or "browser_action" in
// Manifest V2. https://developer.chrome.com/docs/extensions/reference/api/action#manifest
WEBVIEW_API Optional<JsonObject const&> action_of(LoadedExtension const&);

// The path of the icon of the extension's toolbar button at a size: the button's own, or else the extension's.
WEBVIEW_API Optional<String> action_icon_path(LoadedExtension const&, int size);

// The path of an extension's options page, if it has one.
WEBVIEW_API Optional<String> options_page_path(LoadedExtension const&);

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

// What an extension's manifest asks for, as permissions.getAll gives them: API permissions, and the match patterns of
// host permissions. Manifest V2 names both in "permissions", and Manifest V3 the hosts in "host_permissions".
struct ExtensionPermissions {
    Vector<String> permissions;
    Vector<String> origins;
};
WEBVIEW_API ExtensionPermissions required_permissions_of(LoadedExtension const&);
// What it may ask the user for later, with permissions.request.
WEBVIEW_API ExtensionPermissions optional_permissions_of(LoadedExtension const&);
// What the user is told a permission or host permission gives an extension, much as Chromium words its warnings.
WEBVIEW_API String describe_extension_permission(StringView permission);
WEBVIEW_API String describe_extension_origin(StringView origin);

// Whether a held match pattern covers one an extension asks about: the same pattern, one for every URL, or one that
// matches every URL the other pattern names.
// FIXME: Chromium compares the patterns' schemes, hosts and paths, so that https://*.example.com/* also covers
//        https://*.www.example.com/*. Here a pattern with a wildcard in its host is covered only by itself or by a
//        pattern for every URL.
WEBVIEW_API bool match_pattern_covers(StringView held, StringView wanted);

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
// Whether a web page may load the file at the path, relative to the extension's root. Manifest V3 names the sites and
// extensions that may load each file, so it takes the origin of what made the request, if it is known.
WEBVIEW_API bool is_web_accessible(LoadedExtension const&, StringView path, Optional<URL::Origin> const& requesting_origin);

// A file a renderer can read the bytes from.
WEBVIEW_API ErrorOr<IPC::File> file_with_contents(ReadonlyBytes);

// Opens the file that a chrome-extension: URL's path names, for a renderer.
WEBVIEW_API ErrorOr<IPC::File> open_extension_file(LoadedExtension const&, StringView url_path, ExtensionFileRequester, Optional<URL::Origin> const& requesting_origin);

// The extensions as JSON, for a WebContent process.
WEBVIEW_API String serialize_extensions_for_web_content(Vector<LoadedExtension> const&);

// JSON with the comments that Chromium accepts in a manifest taken out.
WEBVIEW_API String strip_json_comments(StringView);

// Keyboard shortcuts for the commands of extensions: https://developer.chrome.com/docs/extensions/reference/api/commands
// Manifests suggest different keys for different platforms, and some modifiers mean different keys on macOS.
enum class ShortcutPlatform : u8 {
    MacOS,
    Windows,
    Linux,
};
WEBVIEW_API ShortcutPlatform current_shortcut_platform();

struct ExtensionShortcut {
    Web::UIEvents::KeyCode key { Web::UIEvents::KeyCode::Key_Invalid };
    // The keys held with it, of Mod_Ctrl, Mod_Alt, Mod_Shift and Mod_Super (Command, on macOS), as key events name them.
    Web::UIEvents::KeyModifier modifiers { Web::UIEvents::KeyModifier::Mod_None };

    bool operator==(ExtensionShortcut const&) const = default;
};

// Whether an extension may use a shortcut on a platform. Like Chromium, a shortcut is a letter, a digit, or one of a few
// other keys, held with Ctrl or Alt (or Command or Control, on macOS), and perhaps with Shift. Ctrl+Alt is not allowed
// outside macOS, where it types characters on some keyboards.
WEBVIEW_API bool is_valid_extension_shortcut(ExtensionShortcut, ShortcutPlatform);
// A shortcut as a manifest's suggested_key writes it, such as "Ctrl+Shift+Y". On macOS, "Ctrl" and "Command" mean the
// Command key, and "MacCtrl" the Control key.
WEBVIEW_API Optional<ExtensionShortcut> parse_manifest_shortcut(StringView, ShortcutPlatform);
// A shortcut the user chose, as the profile keeps it, naming the keys held: "Ctrl", "Alt", "Shift" and "Command".
WEBVIEW_API Optional<ExtensionShortcut> parse_stored_shortcut(StringView, ShortcutPlatform);
WEBVIEW_API String stored_shortcut_text(ExtensionShortcut);
// A shortcut as the user reads it, as Chromium shows it: "⌥⇧Y" on macOS, "Alt+Shift+Y" elsewhere.
WEBVIEW_API String display_shortcut_text(ExtensionShortcut, ShortcutPlatform);

// The commands that act as a click on the extension's toolbar button, in Manifest V3 and in Manifest V2.
WEBVIEW_API bool is_execute_action_command(StringView name);

struct ExtensionCommand {
    String name;
    String description;
    // The shortcut the manifest suggests on this platform, if it suggests one Ladybird can use. As in Chromium, an
    // extension may suggest no more than four.
    Optional<ExtensionShortcut> suggested_shortcut;
};
// The commands an extension's manifest declares. An extension with a toolbar button also has the command that clicks
// it, as in Chromium, which lets the user give it a shortcut.
WEBVIEW_API Vector<ExtensionCommand> commands_of(LoadedExtension const&, ShortcutPlatform);

// The shortcuts the user chose, by extension ID and then by command name, as stored_shortcut_text gives them. An empty
// one means the user took the command's shortcut away.
using ExtensionShortcutChoices = HashMap<String, HashMap<String, String>>;

struct AssignedExtensionShortcut {
    String extension_id;
    String command;
    ExtensionShortcut shortcut;
};
// The shortcuts that run the commands of the extensions: the ones the user chose, then the ones the manifests suggest
// that no other command has, in the order the extensions come.
WEBVIEW_API Vector<AssignedExtensionShortcut> assign_extension_shortcuts(ReadonlySpan<LoadedExtension>, ExtensionShortcutChoices const&, ShortcutPlatform);

struct TabMove {
    u64 view_id { 0 };
    size_t from_index { 0 };
    size_t to_index { 0 };
};

// The moves, one after another, that took the tabs from one order to the other, as tabs.onMoved tells of them. Both
// orders hold the same tabs. The tabs in requested_moves are taken to have moved.
WEBVIEW_API Vector<TabMove> tab_moves_between(ReadonlySpan<u64> before, ReadonlySpan<u64> after, HashTable<u64> const& requested_moves);

}
