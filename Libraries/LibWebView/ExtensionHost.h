/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <LibDatabase/Forward.h>
#include <LibURL/Forward.h>
#include <LibURL/Origin.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

class ExtensionActions;
struct ExtensionPermissions;
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
    void did_finish_event(WebContentPage const&, u64 dispatch_id, Optional<String> response_json);

    // Dispatches an event to the contexts of an extension in the frames of a view that target picks, as
    // {"contextType", "frameId"}. Calls on_complete with the first response a listener gave, once every frame's
    // listeners have run.
    void dispatch_event(ViewImplementation&, String const& extension_id, JsonObject const& target, StringView event, JsonArray const& arguments, Function<void(Optional<String>)> on_complete);

    // Dispatches an event to every context of an extension, in every view.
    void broadcast_event(String const& extension_id, StringView event, JsonArray const& arguments);

    // What happens to tabs and their frames, which the events of the tabs and webNavigation APIs tell extensions of.
    void navigable_did_commit(CanonicalNavigable const&);
    void navigable_will_be_removed(CanonicalNavigable const&);
    void navigable_did_navigate_within_document(CanonicalNavigable const&, URL::URL const& previous_url);
    void view_did_finish_load(ViewImplementation&);
    void view_did_change_title(ViewImplementation&);
    void view_did_change_pinned(ViewImplementation&);
    // The UI reports the order of its tab strips after each change, which tells of the tabs moved since.
    void tab_order_did_change(ReadonlySpan<u64> previous_order, ReadonlySpan<u64> order);
    // The UI reports the tabs and windows the user may reopen, for the sessions API.
    void recently_closed_did_change(StringView entries_json);
    // The UI reports what happens to the history, for the events of the history API.
    struct HistoryVisit {
        String url;
        String title;
        i64 last_visit_time_ms { 0 };
        i64 visit_count { 0 };
        i64 typed_count { 0 };
    };
    void history_visit_was_recorded(HistoryVisit const&);
    // The URLs whose entries were removed, or nothing when the whole history was.
    void history_entries_were_removed(Optional<Vector<String>> const& urls);
    void view_will_be_destroyed(u64 view_id);
    void active_view_did_change(Optional<u64> view_id);

    // The favicon of a page, as PNG no larger than size pixels on either side: what a tab showing the page shows, or
    // else what the history keeps.
    Optional<ByteBuffer> favicon_png_for_page(URL::URL const&, int size);

    // The toolbar buttons of extensions, as JSON for the UI.
    String serialized_actions();
    // The user clicked the toolbar button of an extension while a tab showed: the button opens its popup, or else the
    // extension is told of the click.
    // https://developer.chrome.com/docs/extensions/reference/api/action#event-onClicked
    void action_clicked(String const& extension_id, u64 view_id);

    // Whether the activeTab permission lets an extension at a URL shown in a tab: the user invoked the extension on the
    // tab, which still shows a page of the origin it showed then.
    // https://developer.chrome.com/docs/extensions/develop/concepts/activeTab
    bool has_active_tab_access(String const& extension_id, u64 view_id, URL::URL const&) const;

    // Takes back optional permissions and host permissions the user granted an extension, as permissions.remove does,
    // and tells the extension with permissions.onRemoved. Required ones, and ones it was not granted, are left alone.
    void revoke_granted_permissions(String const& extension_id, ExtensionPermissions const&);

    // Forgets what the host keeps of an extension that was turned off or removed. What it stored stays on disk.
    void extension_did_unload(String const& extension_id);

    // Called as the application tears down, after which no event is dispatched.
    void stop_dispatching_events() { m_dispatches_events = false; }

private:
    // Who may call an extension's APIs, as the UI process tells from the documents a page hosts rather than from what
    // the page says of itself.
    enum class Caller : u8 {
        // A document of the extension's own origin, such as its background page or a frame of it in a tab.
        ExtensionPage,
        // A content script of the extension, in a web page it matches or was injected into.
        ContentScript,
    };
    Optional<Caller> caller_for(WebContentPage const&, LoadedExtension const&);
    bool page_matches_content_scripts_of(WebContentPage const&, LoadedExtension const&) const;
    bool content_script_may_call(LoadedExtension const&, StringView api) const;

    struct Context {
        String type;
        u64 frame_id { 0 };
        String url;
    };

    void call(WebContentPage&, LoadedExtension const&, Context const&, StringView api, JsonArray const& arguments, Completion);

    void runtime_send_message(WebContentPage&, LoadedExtension const&, Context const&, JsonArray const& arguments, Completion);
    void tabs_send_message(LoadedExtension const&, JsonArray const& arguments, Completion);

    // https://developer.chrome.com/docs/extensions/develop/concepts/messaging#connect
    // One end of a port: the contexts of a type in a view, in one frame or in all of them but one.
    struct PortEnd {
        u64 view_id { 0 };
        String context_type;
        Optional<u64> frame_id;
        Optional<u64> excluded_frame_id;

        bool contains(u64 view_id, StringView context_type, u64 frame_id) const;
        bool operator==(PortEnd const&) const = default;
    };
    // As in Chromium, a port connects its opener to every frame that took it in a listener of runtime.onConnect.
    struct Port {
        String extension_id;
        String name;
        PortEnd opener;
        // The contexts the port was offered to until they answer, then the frames that took it.
        Vector<PortEnd> receivers;
        bool receivers_were_told { false };
        bool receivers_are_known { false };
        // Until the receivers are known, the opener's messages wait.
        Vector<JsonValue> waiting_messages;
    };
    void port_call(WebContentPage&, LoadedExtension const&, Context const&, StringView api, JsonArray const& arguments, Completion);
    void connect_port(String const& port_id, JsonObject sender);
    static JsonObject target_for_port_end(PortEnd const&);
    void send_to_port_end(String const& extension_id, PortEnd const&, StringView event, JsonArray const& arguments, Function<void(Optional<String>)> on_complete = [](auto) { });
    // Closes the port, telling the ends that are not the one that closed it, with the error if there is one.
    void close_port(String const& port_id, Optional<PortEnd> closed_by, Optional<StringView> error = {});
    void close_ports_matching(Function<bool(Port const&, PortEnd const&)> end_is_gone);
    void storage_call(LoadedExtension const&, StringView area, StringView function, JsonArray const& arguments, Completion);

    void tabs_call(WebContentPage&, LoadedExtension const&, StringView function, JsonArray const& arguments, Completion);
    void scripting_call(WebContentPage&, LoadedExtension const&, StringView function, JsonArray const& arguments, Completion);
    void sessions_call(LoadedExtension const&, StringView function, JsonArray const& arguments, Completion);
    void history_call(StringView function, JsonArray const& arguments, Completion);
    void search_query(WebContentPage&, JsonArray const& arguments, Completion);
    void bookmarks_call(StringView function, JsonArray const& arguments, Completion);
    void permissions_call(LoadedExtension const&, StringView function, JsonArray const& arguments, Completion);
    CallResult open_action_popup(LoadedExtension const&, JsonValue const& options);
    HistoryStore* history_store();

    void inject(ViewImplementation&, LoadedExtension const&, JsonObject const& target, JsonObject const& details, Function<void(JsonArray)> on_complete);

    bool dispatches_events() const;
    void background_page_did_load(LoadedExtension const&);
    void broadcast_to_extension_pages(StringView event, JsonArray const& arguments);
    void tab_did_update(ViewImplementation&, JsonObject change_info);
    void grant_active_tab(LoadedExtension const&, ViewImplementation&);

    JsonObject sender_for(WebContentPage&, LoadedExtension const&, Context const&) const;
    JsonObject& storage_area(LoadedExtension const&, StringView area);
    void save_storage(LoadedExtension const&);

    void dispatch_event_to_page(WebContentPage&, String const& extension_id, JsonObject const& target, StringView event, JsonArray const& arguments, Function<void(Optional<String>)> on_complete);
    struct ViewResponse {
        u64 view_id { 0 };
        String response;
    };
    // The response that wins: the first that isn't empty, else an empty one, from a context that listened but didn't
    // respond.
    static Optional<String> winning_response(Vector<ViewResponse> const&);
    // Dispatches the event to the views, with the target that target_for gives for each, and collects the responses,
    // in the order they came.
    void dispatch_event_to_views(Vector<ViewImplementation&>, String const& extension_id, Function<JsonObject(ViewImplementation&)> target_for, StringView event, JsonArray const& arguments, Function<void(Vector<ViewResponse>)> on_complete);

    struct PendingDispatch {
        // NB: Only compared with the page that answers, never dereferenced.
        WebContentPage const* page { nullptr };
        Function<void(Optional<String>)> on_complete;
    };
    HashMap<u64, PendingDispatch> m_pending_dispatches;

    // The pages into which an extension injected scripts in its isolated world, whose content scripts may then call
    // its APIs, as in Chromium.
    HashMap<String, Vector<NonnullRefPtr<WebContentPage>>> m_pages_with_injected_scripts;

    // The extensions whose session storage their content scripts may use, as storage.session.setAccessLevel allows.
    HashTable<String> m_session_storage_open_to_content_scripts;
    u64 m_next_dispatch_id { 0 };
    bool m_dispatches_events { true };

    // The tabs that the activeTab permission lets each extension at, by extension ID and then view ID, with the origin
    // each tab showed when the user invoked the extension.
    HashMap<String, HashMap<u64, URL::Origin>> m_active_tab_grants;

    // The tabs that extensions were told of, which they are told are gone once they are.
    HashTable<u64> m_announced_tabs;
    // The tabs that tabs.move or tabs.update asked the UI to move, until it reports the new order.
    HashTable<u64> m_requested_tab_moves;
    // What the UI last reported of the tabs and windows the user may reopen, most recently closed first.
    Optional<JsonArray> m_recently_closed;
    // The open ports, by their IDs.
    HashMap<String, Port> m_ports;
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

    NonnullOwnPtr<ExtensionActions> m_actions;

    // NB: The UI keeps the history, in a database this reads too.
    RefPtr<Database::Database> m_history_database;
    OwnPtr<HistoryStore> m_history_store;
    bool m_tried_opening_history { false };
};

// The tab object of a view, as the tabs API describes one: https://developer.chrome.com/docs/extensions/reference/api/tabs#type-Tab
WEBVIEW_API JsonObject tab_object_for(ViewImplementation&, size_t index);

}
