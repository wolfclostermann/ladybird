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

// https://developer.chrome.com/docs/extensions/develop/concepts/match-patterns
WEB_API bool match_pattern_matches(StringView pattern, URL::URL const&);

// Runs the content scripts of document that are to run at run_at, and those of the earlier points that have not run yet.
void run_content_scripts(DOM::Document&, RunAt);

// The CSS of the content scripts that match document.
Utf16String style_sheet_for_document(DOM::Document const&);

}
