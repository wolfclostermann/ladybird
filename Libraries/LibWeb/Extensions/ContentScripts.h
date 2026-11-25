/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibURL/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Extensions/MatchPattern.h>

// Content scripts of WebExtensions: https://developer.chrome.com/docs/extensions/develop/concepts/content-scripts
//
// The UI process loads the extensions, and gives each WebContent process the content scripts they declare, with the
// sources of their files. This process runs a content script in an isolated world of each document it matches: a
// realm of its own, whose global object is another wrapper of the document's Window. The page and the extension share
// the DOM, but not their JavaScript objects.
namespace Web::Extensions {

enum class RunAt : u8 {
    DocumentStart,
    DocumentEnd,
    DocumentIdle,
};

struct ScriptFile {
    String path;
    Utf16String source;
};

struct ContentScript {
    Vector<String> matches;
    Vector<String> exclude_matches;
    Vector<ScriptFile> js;
    Utf16String css;
    RunAt run_at { RunAt::DocumentIdle };
    bool all_frames { false };
    bool match_about_blank { false };
};

struct Extension {
    String id;
    // The manifest, as JSON with its comments taken out, for chrome.runtime.getManifest().
    String manifest_json;
    Vector<ContentScript> content_scripts;
};

// Replaces the extensions of this process with those of a JSON array that the UI process serialized.
WEB_API ErrorOr<void> set_extensions_from_json(StringView);
WEB_API bool has_extensions();

// Whether the script running in vm is an extension's, in an isolated world. As in Chromium, the page's Content Security
// Policy does not govern what such a script does to the DOM: the styles it adds and the HTML it sets.
WEB_API bool is_running_in_an_isolated_world(JS::VM&);

// Whether a page's Content Security Policy leaves loads of url alone. As in Chromium and Firefox, a page's policy does
// not govern the resources of extensions, which inject their own frames and styles into it. Which of its resources an
// extension lets pages load is up to the extension.
WEB_API bool url_bypasses_content_security_policy(URL::URL const&);

// Runs the content scripts of document that are to run at run_at, and those of the earlier points that have not run yet.
void run_content_scripts(DOM::Document&, RunAt);

// The CSS of the content scripts that match document, and that extensions inserted into it.
Utf16String style_sheet_for_document(DOM::Document const&);

// Runs code, or inserts or removes CSS, in the frames of page that target_json picks, for chrome.scripting. target_json
// is {"allFrames": bool, "frameIds": [n, ...]}, picking only the top-level frame without either. details_json is
// {"kind": "script", "world": "ISOLATED" or "MAIN", "sources": [{"path", "source"}, ...]} or {"kind": "insert_css" or
// "remove_css", "css": "..."}. Calls on_complete with the JSON array of each frame's {"frameId", "result"}.
WEB_API void inject_into_frames(Page&, String const& extension_id, String const& target_json, String const& details_json, Function<void(String)> on_complete);

}
