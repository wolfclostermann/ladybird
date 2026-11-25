/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <LibDatabase/Forward.h>
#include <LibURL/Forward.h>
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
    ExtensionHost();
    ~ExtensionHost();

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

    // What happens to tabs and their frames, which the events of the tabs and webNavigation APIs tell extensions of.
    void navigable_did_commit(CanonicalNavigable const&);
    void navigable_did_navigate_within_document(CanonicalNavigable const&, URL::URL const& previous_url);
    void view_did_finish_load(ViewImplementation&);
    void view_did_change_title(ViewImplementation&);
    void view_will_be_destroyed(u64 view_id);
    void active_view_did_change(Optional<u64> view_id);

    // Called as the application tears down, after which no event is dispatched.
    void stop_dispatching_events() { m_dispatches_events = false; }

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

    void tabs_call(WebContentPage&, LoadedExtension const&, StringView function, JsonArray const& arguments, Completion);
    void scripting_call(WebContentPage&, LoadedExtension const&, StringView function, JsonArray const& arguments, Completion);
    void history_call(StringView function, JsonArray const& arguments, Completion);
    void search_query(WebContentPage&, JsonArray const& arguments, Completion);
    void bookmarks_call(StringView function, JsonArray const& arguments, Completion);
    HistoryStore* history_store();

    void inject(ViewImplementation&, LoadedExtension const&, JsonObject const& target, JsonObject const& details, Function<void(JsonArray)> on_complete);

    bool dispatches_events() const;
    void background_page_did_load(LoadedExtension const&);
    void broadcast_to_extension_pages(StringView event, JsonArray const& arguments);
    void tab_did_update(ViewImplementation&, JsonObject change_info);

    JsonObject sender_for(WebContentPage&, LoadedExtension const&, Context const&) const;
    JsonObject& storage_area(LoadedExtension const&, StringView area);
    void save_storage(LoadedExtension const&);

    void dispatch_event_to_page(WebContentPage&, String const& extension_id, JsonObject const& target, StringView event, JsonArray const& arguments, Function<void(Optional<String>)> on_complete);

    HashMap<u64, Function<void(Optional<String>)>> m_pending_dispatches;
    u64 m_next_dispatch_id { 0 };
    bool m_dispatches_events { true };

    // The tabs that extensions were told of, which they are told are gone once they are.
    HashTable<u64> m_announced_tabs;
    Optional<u64> m_active_view_id;

    // Like Chromium, which holds messages for a service worker until it starts, messages wait for a background page
    // until it has loaded.
    HashTable<String> m_loaded_background_pages;
    HashMap<String, Vector<Function<void()>>> m_waiting_for_background_page;

    struct ExtensionStorage {
        JsonObject local;
        JsonObject sync;
        JsonObject session;
        JsonObject managed;
    };
    HashMap<String, ExtensionStorage> m_storage;

    // NB: The UI keeps the history, in a database this reads too.
    RefPtr<Database::Database> m_history_database;
    OwnPtr<HistoryStore> m_history_store;
    bool m_tried_opening_history { false };
};

// The tab object of a view, as the tabs API describes one: https://developer.chrome.com/docs/extensions/reference/api/tabs#type-Tab
WEBVIEW_API JsonObject tab_object_for(ViewImplementation&, size_t index);

}
