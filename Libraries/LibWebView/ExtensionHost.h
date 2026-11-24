/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct LoadedExtension;
class WebContentPage;

// Carries out the calls that extensions make to the extension APIs, and dispatches their events, in the UI process.
class WEBVIEW_API ExtensionHost {
    AK_MAKE_NONCOPYABLE(ExtensionHost);
    AK_MAKE_NONMOVABLE(ExtensionHost);

public:
    ExtensionHost() = default;

    struct CallResult {
        bool is_error { false };
        // The JSON of the result, or the message of the error. Empty for an undefined result.
        String payload;
    };
    using Completion = Function<void(CallResult)>;


    void handle_api_call(WebContentPage&, String const& extension_id, String const& context_json, String const& api, String const& arguments_json, u64 call_id);
    void did_finish_event(u64 dispatch_id, Optional<String> response_json);

    // Dispatches an event to the contexts of an extension in the frames of a view that target picks, as
    // {"contextType", "frameId"}. Calls on_complete with the first response a listener gave, once every frame's
    // listeners have run.
    void dispatch_event(ViewImplementation&, String const& extension_id, JsonObject const& target, StringView event, JsonArray const& arguments, Function<void(Optional<String>)> on_complete);

    // Dispatches an event to every context of an extension, in every view.
    void broadcast_event(String const& extension_id, StringView event, JsonArray const& arguments);

private:
    struct Context {
        String type;
        u64 frame_id { 0 };
        String url;
    };

    void call(WebContentPage&, LoadedExtension const&, Context const&, StringView api, JsonArray const& arguments, Completion);

    void runtime_send_message(WebContentPage&, LoadedExtension const&, Context const&, JsonArray const& arguments, Completion);
    void tabs_send_message(LoadedExtension const&, JsonArray const& arguments, Completion);
    void storage_call(LoadedExtension const&, StringView area, StringView function, JsonArray const& arguments, Completion);

    JsonObject sender_for(WebContentPage&, LoadedExtension const&, Context const&) const;
    JsonObject& storage_area(LoadedExtension const&, StringView area);
    void save_storage(LoadedExtension const&);

    void dispatch_event_to_page(WebContentPage&, String const& extension_id, JsonObject const& target, StringView event, JsonArray const& arguments, Function<void(Optional<String>)> on_complete);

    HashMap<u64, Function<void(Optional<String>)>> m_pending_dispatches;
    u64 m_next_dispatch_id { 0 };

    struct ExtensionStorage {
        JsonObject local;
        JsonObject sync;
        JsonObject session;
        JsonObject managed;
    };
    HashMap<String, ExtensionStorage> m_storage;
};

// The tab object of a view, as the tabs API describes one: https://developer.chrome.com/docs/extensions/reference/api/tabs#type-Tab
WEBVIEW_API JsonObject tab_object_for(ViewImplementation&, size_t index);

}
