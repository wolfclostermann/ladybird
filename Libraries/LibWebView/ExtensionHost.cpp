/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebView/Application.h>
#include <LibWebCommon/Extensions/FrameId.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/ExtensionHost.h>
#include <LibWebView/Extensions.h>
#include <LibWebView/HistoryStore.h>
#include <LibDatabase/Database.h>
#include <LibCore/File.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

// Like Chromium, every tab is in the one window until windows are known to the extension APIs.
static constexpr i64 window_id = 1;

// The views that show tabs, in the order the extension APIs index them. Views without a window, such as those of
// background pages, are not tabs.
static Vector<ViewImplementation&> tab_views()
{
    Vector<ViewImplementation&> views;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (!Application::the().is_extension_background_view(view) && !view.owner_view_id().has_value())
            views.append(view);
        return IterationDecision::Continue;
    });
    return views;
}

static Optional<ViewImplementation&> tab_view_with_id(i64 tab_id)
{
    for (auto& view : tab_views()) {
        if (static_cast<i64>(view.view_id()) == tab_id)
            return view;
    }
    return {};
}

static bool is_tab_view(ViewImplementation const& view)
{
    return !Application::the().is_extension_background_view(view) && !view.owner_view_id().has_value();
}

static bool view_is_active(ViewImplementation& view)
{
    // The application knows which view is active when a UI shows it. Otherwise, the visible views are.
    if (auto active_view = Application::the().active_web_view(); active_view.has_value())
        return &*active_view == &view;
    return view.has_traversable() && view.traversable().system_visibility_state() == Web::HTML::VisibilityState::Visible;
}

// The tab that a call without a tab ID acts on: the active one, as Chromium picks in the current window.
static Optional<ViewImplementation&> active_tab_view()
{
    Optional<ViewImplementation&> first_tab;
    for (auto& view : tab_views()) {
        if (view_is_active(view))
            return view;
        if (!first_tab.has_value())
            first_tab = view;
    }
    return first_tab;
}

static Optional<ViewImplementation&> tab_view_for_argument(JsonValue const& tab_id)
{
    if (tab_id.is_null())
        return active_tab_view();
    if (auto id = tab_id.get_integer<i64>(); id.has_value())
        return tab_view_with_id(*id);
    return {};
}

static u64 milliseconds_since_epoch()
{
    return static_cast<u64>(UnixDateTime::now().milliseconds_since_epoch());
}

static u64 frame_id_of(CanonicalNavigable const& navigable)
{
    return Web::Extensions::frame_id_for_navigable(navigable.id(), navigable.is_top_level_traversable());
}

// The pages that host the documents of a view's frames, or only of the frame with frame_id.
static Vector<NonnullRefPtr<WebContentPage>> pages_hosting_frames_of(ViewImplementation& view, Optional<u64> frame_id)
{
    Vector<NonnullRefPtr<WebContentPage>> pages;
    if (!view.has_traversable())
        return pages;
    view.traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        if (frame_id.has_value() && frame_id_of(navigable) != *frame_id)
            return IterationDecision::Continue;
        if (!navigable.active_session_history_entry())
            return IterationDecision::Continue;
        auto const& host = navigable.active_document().host();
        if (host && host->is_open() && !any_of(pages, [&](auto const& page) { return page.ptr() == host.ptr(); }))
            pages.append(*host);
        return IterationDecision::Continue;
    });
    // The view's own page hosts the top-level document, even before it has a session history entry.
    if (pages.is_empty() && (!frame_id.has_value() || *frame_id == 0))
        pages.append(view.page());
    return pages;
}

// https://developer.chrome.com/docs/extensions/reference/api/webNavigation#method-getAllFrames
static JsonObject frame_details_for(CanonicalNavigable const& navigable)
{
    JsonObject frame;
    frame.set("frameId"sv, frame_id_of(navigable));
    frame.set("parentFrameId"sv, navigable.parent() ? static_cast<i64>(frame_id_of(*navigable.parent())) : -1);
    frame.set("url"sv, navigable.active_session_history_entry() ? navigable.active_document_url().serialize() : "about:blank"_string);
    frame.set("errorOccurred"sv, false);
    frame.set("frameType"sv, navigable.parent() ? "sub_frame"sv : "outermost_frame"sv);
    frame.set("documentLifecycle"sv, "active"sv);
    return frame;
}

JsonObject tab_object_for(ViewImplementation& view, size_t index)
{
    auto active = view_is_active(view);

    JsonObject tab;
    tab.set("id"sv, static_cast<i64>(view.view_id()));
    tab.set("index"sv, static_cast<i64>(index));
    tab.set("windowId"sv, window_id);
    tab.set("active"sv, active);
    tab.set("highlighted"sv, active);
    tab.set("selected"sv, active);
    tab.set("pinned"sv, false);
    tab.set("audible"sv, false);
    tab.set("discarded"sv, false);
    tab.set("autoDiscardable"sv, true);
    tab.set("frozen"sv, false);
    tab.set("groupId"sv, -1);
    tab.set("incognito"sv, view.is_private() == IsPrivate::Yes);
    tab.set("url"sv, view.url().serialize());
    tab.set("title"sv, view.title().to_well_formed_utf8());
    tab.set("status"sv, view.is_loading() ? "loading"sv : "complete"sv);
    tab.set("mutedInfo"sv, JsonObject {});
    return tab;
}

static Optional<JsonObject> tab_object_with_id(i64 tab_id)
{
    auto views = tab_views();
    for (size_t i = 0; i < views.size(); ++i) {
        if (static_cast<i64>(views[i].view_id()) == tab_id)
            return tab_object_for(views[i], i);
    }
    return {};
}

static JsonObject window_object(bool with_tabs)
{
    JsonObject window;
    window.set("id"sv, window_id);
    window.set("focused"sv, true);
    window.set("top"sv, 0);
    window.set("left"sv, 0);
    window.set("alwaysOnTop"sv, false);
    window.set("incognito"sv, false);
    window.set("type"sv, "normal"sv);
    window.set("state"sv, "normal"sv);
    if (with_tabs) {
        JsonArray tabs;
        auto views = tab_views();
        for (size_t i = 0; i < views.size(); ++i)
            tabs.must_append(tab_object_for(views[i], i));
        window.set("tabs"sv, move(tabs));
    }
    return window;
}

static ExtensionHost::CallResult result(JsonValue const& value)
{
    return { .is_error = false, .payload = value.serialized() };
}

static ExtensionHost::CallResult undefined_result()
{
    return { .is_error = false, .payload = {} };
}

static ExtensionHost::CallResult error(StringView message)
{
    return { .is_error = true, .payload = MUST(String::from_utf8(message)) };
}

void ExtensionHost::handle_api_call(WebContentPage& page, String const& extension_id, String const& context_json, String const& api, String const& arguments_json, u64 call_id)
{
    auto complete = [page = NonnullRefPtr { page }, call_id](CallResult result) {
        page->async_complete_extension_api_call(call_id, result.is_error, result.payload);
    };

    // FIXME: Check that the page hosts a context of the extension, rather than trusting the renderer that it does.
    auto const* extension = Application::the().extension_with_id(extension_id);
    if (!extension) {
        complete(error("No such extension"sv));
        return;
    }

    Context context;
    if (auto parsed = JsonParser::parse(context_json); !parsed.is_error() && parsed.value().is_object()) {
        auto const& object = parsed.value().as_object();
        context.type = object.get_string("contextType"sv).value_or({});
        context.frame_id = object.get_integer<u64>("frameId"sv).value_or(0);
        context.url = object.get_string("url"sv).value_or({});
    }

    JsonArray arguments;
    if (auto parsed = JsonParser::parse(arguments_json); !parsed.is_error() && parsed.value().is_array())
        arguments = parsed.value().as_array();

    call(page, *extension, context, api, arguments, move(complete));
}

void ExtensionHost::call(WebContentPage& page, LoadedExtension const& extension, Context const& context, StringView api, JsonArray const& arguments, Completion complete)
{
    auto argument = [&](size_t index) -> JsonValue {
        return index < arguments.size() ? arguments.at(index) : JsonValue {};
    };

    if (api == "runtime.sendMessage"sv)
        return runtime_send_message(page, extension, context, arguments, move(complete));
    if (api == "tabs.sendMessage"sv)
        return tabs_send_message(extension, arguments, move(complete));

    if (api.starts_with("storage."sv)) {
        auto parts = api.split_view('.');
        if (parts.size() == 3)
            return storage_call(extension, parts[1], parts[2], arguments, move(complete));
    }

    if (api == "runtime.getPlatformInfo"sv) {
        JsonObject info;
#if defined(AK_OS_MACOS)
        info.set("os"sv, "mac"sv);
#elif defined(AK_OS_WINDOWS)
        info.set("os"sv, "win"sv);
#else
        info.set("os"sv, "linux"sv);
#endif
#if ARCH(AARCH64)
        info.set("arch"sv, "arm64"sv);
#else
        info.set("arch"sv, "x86-64"sv);
#endif
        return complete(result(info));
    }

    if (api == "tabs.query"sv) {
        auto query = argument(0).is_object() ? argument(0).as_object() : JsonObject {};
        JsonArray tabs;
        auto views = tab_views();
        for (size_t i = 0; i < views.size(); ++i) {
            auto& view = views[i];
            if (auto active = query.get_bool("active"sv); active.has_value() && *active != view_is_active(view))
                continue;
            if (auto id = query.get_integer<i64>("windowId"sv); id.has_value() && *id >= 0 && *id != window_id)
                continue;

            if (auto url = query.get("url"sv); url.has_value()) {
                Vector<String> patterns;
                if (url->is_string())
                    patterns.append(url->as_string());
                else if (url->is_array())
                    url->as_array().for_each([&](auto const& value) { if (value.is_string()) patterns.append(value.as_string()); });
                auto view_url = view.url();
                if (!any_of(patterns, [&](auto const& pattern) { return Web::Extensions::match_pattern_matches(pattern, view_url); }))
                    continue;
            }

            tabs.must_append(tab_object_for(view, i));
        }
        return complete(result(tabs));
    }

    if (api == "tabs.get"sv) {
        auto tab = argument(0).get_integer<i64>().has_value() ? tab_object_with_id(*argument(0).get_integer<i64>()) : Optional<JsonObject> {};
        if (!tab.has_value())
            return complete(error(MUST(String::formatted("No tab with id: {}", argument(0)))));
        return complete(result(*tab));
    }

    if (api == "tabs.getCurrent"sv) {
        // Only an extension page shown in a tab is in one.
        if (Application::the().is_extension_background_view(page.view()))
            return complete(undefined_result());
        auto tab = tab_object_with_id(static_cast<i64>(page.view().view_id()));
        return complete(tab.has_value() ? result(*tab) : undefined_result());
    }

    if (api == "webNavigation.getAllFrames"sv || api == "webNavigation.getFrame"sv) {
        auto details = argument(0).is_object() ? argument(0).as_object() : JsonObject {};
        auto view = tab_view_with_id(details.get_integer<i64>("tabId"sv).value_or(-1));
        if (!view.has_value() || !view->has_traversable())
            return complete(api == "webNavigation.getFrame"sv ? result(JsonValue {}) : error("No tab with that id"sv));

        auto wanted_frame_id = details.get_integer<u64>("frameId"sv);
        JsonArray frames;
        view->traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
            if (api == "webNavigation.getFrame"sv && frame_id_of(navigable) != wanted_frame_id.value_or(0))
                return IterationDecision::Continue;
            frames.must_append(frame_details_for(navigable));
            return IterationDecision::Continue;
        });
        if (api == "webNavigation.getFrame"sv)
            return complete(result(frames.is_empty() ? JsonValue {} : frames.at(0)));
        return complete(result(frames));
    }


    if (api == "windows.getCurrent"sv || api == "windows.getLastFocused"sv || api == "windows.get"sv) {
        auto options = argument(api == "windows.get"sv ? 1 : 0);
        auto populate = options.is_object() && options.as_object().get_bool("populate"sv).value_or(false);
        return complete(result(window_object(populate)));
    }

    if (api == "windows.getAll"sv) {
        auto options = argument(0);
        auto populate = options.is_object() && options.as_object().get_bool("populate"sv).value_or(false);
        JsonArray windows;
        windows.must_append(window_object(populate));
        return complete(result(windows));
    }

    // FIXME: Show the toolbar button of an extension, with what these set.
    if (api.starts_with("action."sv) || api.starts_with("browserAction."sv)) {
        auto function = api.substring_view(api.find('.').value() + 1);
        if (function.starts_with("set"sv) || function.is_one_of("enable"sv, "disable"sv))
            return complete(undefined_result());
    }

    if (api.starts_with("tabs."sv))
        return tabs_call(page, extension, api.substring_view(5), arguments, move(complete));
    if (api.starts_with("scripting."sv))
        return scripting_call(page, extension, api.substring_view(10), arguments, move(complete));

    if (api.starts_with("history."sv))
        return history_call(api.substring_view(8), arguments, move(complete));
    if (api.starts_with("bookmarks."sv))
        return bookmarks_call(api.substring_view(10), arguments, move(complete));

    if (api == "windows.update"sv)
        return complete(result(window_object(false)));

    if (api == "permissions.contains"sv)
        return complete(result(true));

    complete(error(MUST(String::formatted("chrome.{} is not implemented yet", api))));
}

JsonObject ExtensionHost::sender_for(WebContentPage& page, LoadedExtension const& extension, Context const& context) const
{
    JsonObject sender;
    sender.set("id"sv, extension.id);
    sender.set("url"sv, context.url);
    if (auto url = URL::Parser::basic_parse(context.url); url.has_value())
        sender.set("origin"sv, url->origin().serialize());

    // A content script, or an extension page shown in a tab, is in that tab.
    if (!Application::the().is_extension_background_view(page.view())) {
        auto views = tab_views();
        for (size_t i = 0; i < views.size(); ++i) {
            if (&views[i] == &page.view()) {
                sender.set("tab"sv, tab_object_for(views[i], i));
                sender.set("frameId"sv, context.frame_id);
                break;
            }
        }
    }
    return sender;
}

// https://developer.chrome.com/docs/extensions/reference/api/runtime#method-sendMessage
void ExtensionHost::runtime_send_message(WebContentPage& page, LoadedExtension const& extension, Context const& context, JsonArray const& arguments, Completion complete)
{
    // The message goes to the extension's pages. Only its background page is known to receive messages so far.
    // FIXME: Send it to the extension's other pages too, such as one shown in a tab.
    auto* background_view = Application::the().extension_background_view(extension.id);
    if (!background_view || background_view == &page.view()) {
        complete(error("Could not establish connection. Receiving end does not exist."sv));
        return;
    }

    JsonArray event_arguments;
    event_arguments.must_append(arguments.is_empty() ? JsonValue {} : arguments.at(0));
    event_arguments.must_append(sender_for(page, extension, context));

    auto send = [this, background_view, extension_id = extension.id, event_arguments = move(event_arguments), complete = move(complete)]() mutable {
        JsonObject target;
        target.set("contextType"sv, "extension_page"sv);
        dispatch_event(*background_view, extension_id, target, "runtime.onMessage"sv, event_arguments, [complete = move(complete)](Optional<String> response) mutable {
            complete({ .is_error = false, .payload = response.value_or({}) });
        });
    };
    if (!m_loaded_background_pages.contains(extension.id)) {
        m_waiting_for_background_page.ensure(extension.id).append(move(send));
        return;
    }
    send();
}

// https://developer.chrome.com/docs/extensions/reference/api/runtime#event-onInstalled
// https://developer.chrome.com/docs/extensions/reference/api/runtime#event-onStartup
void ExtensionHost::background_page_did_load(LoadedExtension const& extension)
{
    if (m_loaded_background_pages.contains(extension.id))
        return;
    m_loaded_background_pages.set(extension.id);

    // An extension is installed when the profile first runs it, and updated when it runs with another version.
    auto path = LexicalPath::join(Application::profile().paths().data, "Extensions"sv, extension.id, "installed.json"sv).string();
    Optional<String> previous_version;
    if (auto installed = read_json_file(path); !installed.is_error()) {
        if (auto version = installed.value().get_string("version"sv); version.has_value())
            previous_version = *version;
    }

    JsonArray arguments;
    StringView event = "runtime.onStartup"sv;
    if (previous_version != extension.version) {
        JsonObject details;
        details.set("reason"sv, previous_version.has_value() ? "update"sv : "install"sv);
        if (previous_version.has_value())
            details.set("previousVersion"sv, *previous_version);
        arguments.must_append(move(details));
        event = "runtime.onInstalled"sv;

        JsonObject installed;
        installed.set("version"sv, extension.version);
        if (auto result = write_json_file(path, installed); result.is_error())
            warnln("Unable to note the installation of extension {}: {}", extension.name, result.error());
    }

    if (auto* view = Application::the().extension_background_view(extension.id)) {
        JsonObject target;
        target.set("contextType"sv, "extension_page"sv);
        dispatch_event(*view, extension.id, target, event, arguments, [](auto) { });
    }

    // The messages that waited go once the listeners of the event have run, as the event was dispatched first.
    for (auto& send : m_waiting_for_background_page.take(extension.id).value_or({}))
        send();
}

// https://developer.chrome.com/docs/extensions/reference/api/tabs#method-sendMessage
void ExtensionHost::tabs_send_message(LoadedExtension const& extension, JsonArray const& arguments, Completion complete)
{
    auto tab_id = arguments.is_empty() ? Optional<i64> {} : arguments.at(0).get_integer<i64>();
    auto view = tab_id.has_value() ? tab_view_with_id(*tab_id) : Optional<ViewImplementation&> {};
    if (!view.has_value()) {
        complete(error("Could not establish connection. Receiving end does not exist."sv));
        return;
    }

    JsonObject sender;
    sender.set("id"sv, extension.id);
    if (auto path = background_page_path(extension); path.has_value())
        sender.set("url"sv, MUST(String::formatted("chrome-extension://{}/{}", extension.id, *path)));
    sender.set("origin"sv, MUST(String::formatted("chrome-extension://{}", extension.id)));

    JsonArray event_arguments;
    event_arguments.must_append(arguments.size() > 1 ? arguments.at(1) : JsonValue {});
    event_arguments.must_append(move(sender));

    JsonObject target;
    target.set("contextType"sv, "content_script"sv);
    if (arguments.size() > 2 && arguments.at(2).is_object()) {
        if (auto frame_id = arguments.at(2).as_object().get_integer<u64>("frameId"sv); frame_id.has_value())
            target.set("frameId"sv, *frame_id);
    }

    dispatch_event(*view, extension.id, target, "runtime.onMessage"sv, event_arguments, [complete = move(complete)](Optional<String> response) mutable {
        complete({ .is_error = false, .payload = response.value_or({}) });
    });
}

JsonObject& ExtensionHost::storage_area(LoadedExtension const& extension, StringView area)
{
    auto& storage = m_storage.ensure(extension.id, [&] {
        ExtensionStorage storage;
        auto path = LexicalPath::join(Application::profile().paths().data, "Extensions"sv, extension.id, "storage.json"sv).string();
        if (auto saved = read_json_file(path); !saved.is_error()) {
            if (auto local = saved.value().get_object("local"sv); local.has_value())
                storage.local = *local;
            if (auto sync = saved.value().get_object("sync"sv); sync.has_value())
                storage.sync = *sync;
        }
        return storage;
    });

    if (area == "local"sv)
        return storage.local;
    if (area == "sync"sv)
        return storage.sync;
    if (area == "managed"sv)
        return storage.managed;
    return storage.session;
}

void ExtensionHost::save_storage(LoadedExtension const& extension)
{
    auto storage = m_storage.get(extension.id);
    if (!storage.has_value())
        return;

    // Like Firefox, the sync area stays on this device.
    JsonObject saved;
    saved.set("local"sv, storage->local);
    saved.set("sync"sv, storage->sync);

    auto path = LexicalPath::join(Application::profile().paths().data, "Extensions"sv, extension.id, "storage.json"sv).string();
    if (auto result = write_json_file(path, saved); result.is_error())
        warnln("Unable to save the storage of extension {}: {}", extension.name, result.error());
}

// https://developer.chrome.com/docs/extensions/reference/api/storage
void ExtensionHost::storage_call(LoadedExtension const& extension, StringView area, StringView function, JsonArray const& arguments, Completion complete)
{
    if (!area.is_one_of("local"sv, "sync"sv, "session"sv, "managed"sv)) {
        complete(error("No such storage area"sv));
        return;
    }

    auto& items = storage_area(extension, area);
    auto argument = arguments.is_empty() ? JsonValue {} : arguments.at(0);

    if (function == "get"sv) {
        JsonObject values;
        if (argument.is_null()) {
            values = items;
        } else if (argument.is_string()) {
            if (auto value = items.get(argument.as_string()); value.has_value())
                values.set(argument.as_string(), *value);
        } else if (argument.is_array()) {
            argument.as_array().for_each([&](auto const& key) {
                if (!key.is_string())
                    return;
                if (auto value = items.get(key.as_string()); value.has_value())
                    values.set(key.as_string(), *value);
            });
        } else if (argument.is_object()) {
            // An object names the keys with their defaults.
            argument.as_object().for_each_member([&](auto const& key, auto const& default_value) {
                auto value = items.get(key);
                values.set(key, value.has_value() ? *value : default_value);
            });
        }
        complete(result(values));
        return;
    }

    if (function == "getKeys"sv) {
        JsonArray keys;
        items.for_each_member([&](auto const& key, auto const&) { keys.must_append(key); });
        complete(result(keys));
        return;
    }

    if (function == "getBytesInUse"sv) {
        complete(result(static_cast<i64>(items.serialized().byte_count())));
        return;
    }

    if (function == "setAccessLevel"sv) {
        complete(undefined_result());
        return;
    }

    if (area == "managed"sv) {
        complete(error("The managed storage area cannot be changed"sv));
        return;
    }

    // Each change names a key with its old value and new value, either absent where the key was or is unset.
    JsonObject changes;
    auto note_change = [&](String const& key, Optional<JsonValue> old_value, Optional<JsonValue> new_value) {
        JsonObject change;
        if (old_value.has_value())
            change.set("oldValue"sv, *old_value);
        if (new_value.has_value())
            change.set("newValue"sv, *new_value);
        changes.set(key, move(change));
    };

    if (function == "set"sv) {
        if (!argument.is_object()) {
            complete(error("storage.set takes an object"sv));
            return;
        }
        argument.as_object().for_each_member([&](auto const& key, auto const& value) {
            auto old_value = items.get(key).copy();
            if (old_value.has_value() && old_value->serialized() == value.serialized())
                return;
            items.set(key, value);
            note_change(key, move(old_value), value);
        });
    } else if (function == "remove"sv) {
        Vector<String> keys;
        if (argument.is_string())
            keys.append(argument.as_string());
        else if (argument.is_array())
            argument.as_array().for_each([&](auto const& key) { if (key.is_string()) keys.append(key.as_string()); });
        for (auto const& key : keys) {
            auto old_value = items.get(key).copy();
            if (!old_value.has_value())
                continue;
            items.remove(key);
            note_change(key, move(old_value), {});
        }
    } else if (function == "clear"sv) {
        items.for_each_member([&](auto const& key, auto const& value) { note_change(key, value, {}); });
        items = {};
    } else {
        complete(error(MUST(String::formatted("chrome.storage.{}.{} is not implemented yet", area, function))));
        return;
    }

    if (area != "session"sv)
        save_storage(extension);
    complete(undefined_result());

    if (changes.size() > 0) {
        JsonArray arguments_with_area;
        arguments_with_area.must_append(changes);
        arguments_with_area.must_append(area);
        broadcast_event(extension.id, "storage.onChanged"sv, arguments_with_area);

        JsonArray area_arguments;
        area_arguments.must_append(changes);
        broadcast_event(extension.id, MUST(String::formatted("storage.{}.onChanged", area)), area_arguments);
    }
}

void ExtensionHost::dispatch_event_to_page(WebContentPage& page, String const& extension_id, JsonObject const& target, StringView event, JsonArray const& arguments, Function<void(Optional<String>)> on_complete)
{
    auto dispatch_id = ++m_next_dispatch_id;
    m_pending_dispatches.set(dispatch_id, move(on_complete));
    page.async_dispatch_extension_event(extension_id, target.serialized(), MUST(String::from_utf8(event)), arguments.serialized(), dispatch_id);
}

void ExtensionHost::dispatch_event(ViewImplementation& view, String const& extension_id, JsonObject const& target, StringView event, JsonArray const& arguments, Function<void(Optional<String>)> on_complete)
{
    auto pages = pages_hosting_frames_of(view, target.get_integer<u64>("frameId"sv));
    if (pages.is_empty()) {
        on_complete({});
        return;
    }

    // The first response wins, as in Chromium; the others are dropped.
    struct State : RefCounted<State> {
        Function<void(Optional<String>)> on_complete;
        Optional<String> response;
        size_t remaining { 0 };
    };
    auto state = make_ref_counted<State>();
    state->on_complete = move(on_complete);
    state->remaining = pages.size();

    for (auto& page : pages) {
        dispatch_event_to_page(page, extension_id, target, event, arguments, [state](Optional<String> response) {
            if (!state->response.has_value() && response.has_value())
                state->response = move(response);
            if (--state->remaining == 0 && state->on_complete) {
                auto on_complete = move(state->on_complete);
                on_complete(move(state->response));
            }
        });
    }
}

void ExtensionHost::did_finish_event(u64 dispatch_id, Optional<String> response_json)
{
    if (auto on_complete = m_pending_dispatches.take(dispatch_id); on_complete.has_value())
        (*on_complete)(move(response_json));
}

void ExtensionHost::broadcast_event(String const& extension_id, StringView event, JsonArray const& arguments)
{
    Vector<ViewImplementation&> views;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        views.append(view);
        return IterationDecision::Continue;
    });
    for (auto& view : views)
        dispatch_event(view, extension_id, {}, event, arguments, [](auto) { });
}

// The tab that a page shows, for a call from a content script or an extension page in a tab.
static Optional<ViewImplementation&> tab_view_of_page(WebContentPage& page)
{
    if (!is_tab_view(page.view()))
        return {};
    return page.view();
}

static Optional<URL::URL> url_for_extension(LoadedExtension const& extension, StringView url)
{
    // A relative URL is relative to the extension's root.
    auto base = URL::Parser::basic_parse(MUST(String::formatted("chrome-extension://{}/", extension.id)));
    return URL::Parser::basic_parse(url, base.value());
}

// https://developer.chrome.com/docs/extensions/reference/api/tabs
void ExtensionHost::tabs_call(WebContentPage& page, LoadedExtension const& extension, StringView function, JsonArray const& arguments, Completion complete)
{
    auto argument = [&](size_t index) -> JsonValue {
        return index < arguments.size() ? arguments.at(index) : JsonValue {};
    };
    auto tab_result = [&](ViewImplementation& view) {
        auto tab = tab_object_with_id(static_cast<i64>(view.view_id()));
        return tab.has_value() ? result(*tab) : undefined_result();
    };
    auto no_tab = [&](JsonValue const& tab_id) {
        return error(MUST(String::formatted("No tab with id: {}", tab_id)));
    };

    if (function == "create"sv) {
        auto properties = argument(0).is_object() ? argument(0).as_object() : JsonObject {};
        Optional<URL::URL> url;
        if (auto url_string = properties.get_string("url"sv); url_string.has_value()) {
            url = url_for_extension(extension, *url_string);
            if (!url.has_value())
                return complete(error(MUST(String::formatted("Invalid url: \"{}\"", *url_string))));
        }

        // The new tab goes with the tab it was opened from, as Chromium places it next to that tab.
        Optional<ViewImplementation&> source_view;
        if (auto opener = properties.get_integer<i64>("openerTabId"sv); opener.has_value())
            source_view = tab_view_with_id(*opener);
        if (!source_view.has_value())
            source_view = tab_view_of_page(page);
        if (!source_view.has_value())
            source_view = active_tab_view();

        auto activate = properties.get_bool("active"sv).value_or(true) ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No;
        auto view = Application::the().open_blank_new_tab(activate, source_view.has_value() ? &*source_view : nullptr);
        if (!view.has_value())
            return complete(error("Tabs cannot be created here"sv));

        if (url.has_value())
            view->load_from_browser_ui(*url);

        auto tab = tab_object_with_id(static_cast<i64>(view->view_id())).value_or(tab_object_for(*view, 0));
        if (url.has_value())
            tab.set("pendingUrl"sv, url->serialize());
        return complete(result(tab));
    }

    // The functions that take a tab ID first default to the active tab without one.
    auto tab_id = argument(0);
    auto rest = [&](size_t index) { return argument(tab_id.is_object() ? index - 1 : index); };
    if (tab_id.is_object() && function.is_one_of("update"sv, "reload"sv))
        tab_id = JsonValue {};

    if (function == "remove"sv) {
        Vector<ViewImplementation*> views;
        auto add = [&](JsonValue const& id) {
            auto view = tab_view_for_argument(id);
            if (view.has_value())
                views.append(&*view);
            return view.has_value();
        };
        if (tab_id.is_array()) {
            for (auto const& id : tab_id.as_array().values()) {
                if (!add(id))
                    return complete(no_tab(id));
            }
        } else if (!add(tab_id)) {
            return complete(no_tab(tab_id));
        }
        for (auto* view : views)
            view->request_close();
        return complete(undefined_result());
    }

    auto view = tab_view_for_argument(tab_id);
    if (!view.has_value())
        return complete(no_tab(tab_id));

    if (function == "update"sv) {
        auto properties = rest(1).is_object() ? rest(1).as_object() : JsonObject {};
        if (auto url_string = properties.get_string("url"sv); url_string.has_value()) {
            auto url = url_for_extension(extension, *url_string);
            if (!url.has_value())
                return complete(error(MUST(String::formatted("Invalid url: \"{}\"", *url_string))));
            view->load_from_browser_ui(*url);
        }
        if (properties.get_bool("active"sv).value_or(false) || properties.get_bool("highlighted"sv).value_or(false)) {
            if (view->on_activate_tab)
                view->on_activate_tab();
        }
        if (auto muted = properties.get_bool("muted"sv); muted.has_value())
            view->set_page_mute_state(*muted ? Web::HTML::MuteState::Muted : Web::HTML::MuteState::Unmuted);
        // FIXME: Pin tabs, once the UI can be told to.
        return complete(tab_result(*view));
    }

    if (function == "reload"sv) {
        auto properties = rest(1).is_object() ? rest(1).as_object() : JsonObject {};
        view->reload(properties.get_bool("bypassCache"sv).value_or(false) ? BypassCache::Yes : BypassCache::No);
        return complete(undefined_result());
    }

    if (function == "duplicate"sv) {
        auto url = view->url();
        auto new_view = Application::the().open_blank_new_tab(Web::HTML::ActivateTab::Yes, &*view);
        if (!new_view.has_value())
            return complete(error("Tabs cannot be created here"sv));
        new_view->load_from_browser_ui(url);
        return complete(tab_result(*new_view));
    }

    if (function == "getZoom"sv)
        return complete(result(view->zoom_level()));

    if (function == "setZoom"sv) {
        // A zoom factor of 0 resets the tab to the default.
        auto factor = rest(1).get_double_with_precision_loss().value_or(0);
        if (factor <= 0)
            view->reset_zoom();
        else
            view->set_zoom(factor);
        return complete(undefined_result());
    }

    if (function == "goBack"sv || function == "goForward"sv) {
        view->traverse_the_history_by_delta(function == "goBack"sv ? -1 : 1);
        return complete(undefined_result());
    }

    complete(error(MUST(String::formatted("chrome.tabs.{} is not implemented yet", function))));
}

// https://developer.chrome.com/docs/extensions/reference/api/scripting
void ExtensionHost::scripting_call(WebContentPage&, LoadedExtension const& extension, StringView function, JsonArray const& arguments, Completion complete)
{
    if (!function.is_one_of("executeScript"sv, "insertCSS"sv, "removeCSS"sv)) {
        complete(error(MUST(String::formatted("chrome.scripting.{} is not implemented yet", function))));
        return;
    }

    auto injection = arguments.is_empty() || !arguments.at(0).is_object() ? JsonObject {} : arguments.at(0).as_object();
    auto target = injection.get_object("target"sv).value_or({});
    auto tab_id = target.get_integer<i64>("tabId"sv);
    auto view = tab_id.has_value() ? tab_view_with_id(*tab_id) : Optional<ViewImplementation&> {};
    if (!view.has_value()) {
        complete(error(MUST(String::formatted("No tab with id: {}", tab_id.value_or(-1)))));
        return;
    }

    JsonObject frame_target;
    frame_target.set("allFrames"sv, target.get_bool("allFrames"sv).value_or(false));
    if (auto frame_ids = target.get_array("frameIds"sv); frame_ids.has_value())
        frame_target.set("frameIds"sv, *frame_ids);

    auto files = injection.get_array("files"sv);
    JsonObject details;

    if (function == "executeScript"sv) {
        details.set("kind"sv, "script"sv);
        details.set("world"sv, injection.get_string("world"sv).value_or("ISOLATED"_string));

        JsonArray sources;
        if (files.has_value()) {
            for (auto const& file : files->values()) {
                if (!file.is_string())
                    continue;
                auto source = read_extension_text_file(extension, file.as_string());
                if (source.is_error()) {
                    complete(error(MUST(String::formatted("Could not load file: '{}'.", file.as_string()))));
                    return;
                }
                JsonObject entry;
                entry.set("path"sv, file.as_string());
                entry.set("source"sv, source.release_value());
                sources.must_append(move(entry));
            }
        } else if (auto func = injection.get_string("func"sv); func.has_value()) {
            // The runtime sends the source of the function, which runs with the arguments given.
            auto args = injection.get_array("args"sv).value_or({});
            JsonObject entry;
            entry.set("path"sv, "_injected_function.js"sv);
            entry.set("source"sv, MUST(String::formatted("({}).apply(null, {});", *func, args.serialized())));
            sources.must_append(move(entry));
        } else {
            complete(error("Exactly one of 'func' and 'files' must be specified"sv));
            return;
        }
        details.set("sources"sv, move(sources));
    } else {
        details.set("kind"sv, function == "insertCSS"sv ? "insert_css"sv : "remove_css"sv);

        StringBuilder css;
        if (auto code = injection.get_string("css"sv); code.has_value()) {
            css.append(*code);
        } else if (files.has_value()) {
            for (auto const& file : files->values()) {
                if (!file.is_string())
                    continue;
                auto source = read_extension_text_file(extension, file.as_string());
                if (source.is_error()) {
                    complete(error(MUST(String::formatted("Could not load file: '{}'.", file.as_string()))));
                    return;
                }
                css.append(source.value());
                css.append('\n');
            }
        } else {
            complete(error("Exactly one of 'css' and 'files' must be specified"sv));
            return;
        }
        details.set("css"sv, MUST(css.to_string()));
    }

    auto returns_results = function == "executeScript"sv;
    inject(*view, extension, frame_target, details, [complete = move(complete), returns_results](JsonArray results) mutable {
        complete(returns_results ? result(results) : undefined_result());
    });
}

void ExtensionHost::inject(ViewImplementation& view, LoadedExtension const& extension, JsonObject const& target, JsonObject const& details, Function<void(JsonArray)> on_complete)
{
    // A frame is hosted by one page, but the frames of a tab may be hosted by several.
    Optional<u64> frame_id;
    if (auto frame_ids = target.get_array("frameIds"sv); frame_ids.has_value() && frame_ids->size() == 1)
        frame_id = frame_ids->at(0).get_integer<u64>();
    auto pages = pages_hosting_frames_of(view, target.get_array("frameIds"sv).has_value() ? frame_id : Optional<u64> { 0 });
    if (target.get_bool("allFrames"sv).value_or(false) || (target.get_array("frameIds"sv).has_value() && !frame_id.has_value()))
        pages = pages_hosting_frames_of(view, {});
    if (pages.is_empty()) {
        on_complete({});
        return;
    }

    struct State : RefCounted<State> {
        Function<void(JsonArray)> on_complete;
        JsonArray results;
        size_t remaining { 0 };
    };
    auto state = make_ref_counted<State>();
    state->on_complete = move(on_complete);
    state->remaining = pages.size();

    for (auto& page : pages) {
        auto dispatch_id = ++m_next_dispatch_id;
        m_pending_dispatches.set(dispatch_id, [state](Optional<String> results_json) {
            if (results_json.has_value()) {
                if (auto results = JsonParser::parse(*results_json); !results.is_error() && results.value().is_array()) {
                    results.value().as_array().for_each([&](auto const& result) { state->results.must_append(result); });
                }
            }
            if (--state->remaining == 0) {
                // The top-level frame comes first, as in Chromium.
                auto frame_id_of_result = [](JsonValue const& result) { return result.as_object().get_u64("frameId"sv).value_or(0); };
                quick_sort(state->results.values(), [&](JsonValue const& a, JsonValue const& b) {
                    return frame_id_of_result(a) < frame_id_of_result(b);
                });
                state->on_complete(move(state->results));
            }
        });
        page->async_inject_into_extension_frames(extension.id, target.serialized(), details.serialized(), dispatch_id);
    }
}

bool ExtensionHost::dispatches_events() const
{
    return m_dispatches_events && !Application::the().extensions().is_empty();
}

// The events of the tabs and webNavigation APIs go to the pages of extensions, not to their content scripts.
void ExtensionHost::broadcast_to_extension_pages(StringView event, JsonArray const& arguments)
{
    JsonObject target;
    target.set("contextType"sv, "extension_page"sv);

    for (auto const& extension : Application::the().extensions()) {
        Vector<ViewImplementation&> views;
        ViewImplementation::for_each_view([&](ViewImplementation& view) {
            if (Application::the().extension_background_view(extension.id) == &view || (view.url().scheme() == "chrome-extension"sv && view.url().serialized_host() == extension.id))
                views.append(view);
            return IterationDecision::Continue;
        });
        for (auto& view : views)
            dispatch_event(view, extension.id, target, event, arguments, [](auto) { });
    }
}

void ExtensionHost::tab_did_update(ViewImplementation& view, JsonObject change_info)
{
    auto tab = tab_object_with_id(static_cast<i64>(view.view_id()));
    if (!tab.has_value())
        return;

    JsonArray arguments;
    arguments.must_append(static_cast<i64>(view.view_id()));
    arguments.must_append(move(change_info));
    arguments.must_append(tab.release_value());
    broadcast_to_extension_pages("tabs.onUpdated"sv, arguments);
}

// https://developer.chrome.com/docs/extensions/reference/api/webNavigation#event-onCommitted
void ExtensionHost::navigable_did_commit(CanonicalNavigable const& navigable)
{
    if (!dispatches_events())
        return;
    auto view = navigable.top_level_traversable().view();
    if (!view.has_value() || !is_tab_view(*view))
        return;
    auto tab_id = static_cast<i64>(view->view_id());

    if (navigable.is_top_level_traversable() && !m_announced_tabs.contains(view->view_id())) {
        m_announced_tabs.set(view->view_id());
        if (auto tab = tab_object_with_id(tab_id); tab.has_value()) {
            JsonArray arguments;
            arguments.must_append(tab.release_value());
            broadcast_to_extension_pages("tabs.onCreated"sv, arguments);
        }
    }

    auto details = frame_details_for(navigable);
    details.set("tabId"sv, tab_id);
    details.set("processId"sv, -1);
    details.set("timeStamp"sv, milliseconds_since_epoch());
    details.set("transitionType"sv, "link"sv);
    details.set("transitionQualifiers"sv, JsonArray {});
    JsonArray arguments;
    arguments.must_append(move(details));
    broadcast_to_extension_pages("webNavigation.onCommitted"sv, arguments);

    if (navigable.is_top_level_traversable()) {
        JsonObject change_info;
        change_info.set("status"sv, "loading"sv);
        change_info.set("url"sv, navigable.active_document_url().serialize());
        tab_did_update(*view, move(change_info));
    }
}

// https://developer.chrome.com/docs/extensions/reference/api/webNavigation#event-onHistoryStateUpdated
// https://developer.chrome.com/docs/extensions/reference/api/webNavigation#event-onReferenceFragmentUpdated
void ExtensionHost::navigable_did_navigate_within_document(CanonicalNavigable const& navigable, URL::URL const& previous_url)
{
    if (!dispatches_events())
        return;
    auto view = navigable.top_level_traversable().view();
    if (!view.has_value() || !is_tab_view(*view))
        return;

    auto const& url = navigable.active_document_url();
    // FIXME: Chromium tells a fragment navigation from history.pushState(), even to a URL that differs only in its
    //        fragment. The canonical side is not told which navigation this was, so it goes by the URLs.
    auto only_fragment_changed = url.equals(previous_url, URL::ExcludeFragment::Yes);

    auto details = frame_details_for(navigable);
    details.set("tabId"sv, static_cast<i64>(view->view_id()));
    details.set("processId"sv, -1);
    details.set("timeStamp"sv, milliseconds_since_epoch());
    details.set("transitionType"sv, "link"sv);
    details.set("transitionQualifiers"sv, JsonArray {});
    JsonArray arguments;
    arguments.must_append(move(details));
    broadcast_to_extension_pages(only_fragment_changed ? "webNavigation.onReferenceFragmentUpdated"sv : "webNavigation.onHistoryStateUpdated"sv, arguments);

    if (navigable.is_top_level_traversable()) {
        JsonObject change_info;
        change_info.set("url"sv, url.serialize());
        tab_did_update(*view, move(change_info));
    }
}

// https://developer.chrome.com/docs/extensions/reference/api/tabs#event-onUpdated
void ExtensionHost::view_did_finish_load(ViewImplementation& view)
{
    if (m_dispatches_events && Application::the().is_extension_background_view(view)) {
        for (auto const& extension : Application::the().extensions()) {
            if (Application::the().extension_background_view(extension.id) == &view)
                background_page_did_load(extension);
        }
        return;
    }

    if (!dispatches_events() || !is_tab_view(view))
        return;

    if (view.has_traversable()) {
        auto details = frame_details_for(view.traversable());
        details.set("tabId"sv, static_cast<i64>(view.view_id()));
        details.set("processId"sv, -1);
        details.set("timeStamp"sv, milliseconds_since_epoch());
        JsonArray arguments;
        arguments.must_append(move(details));
        broadcast_to_extension_pages("webNavigation.onCompleted"sv, arguments);
    }

    JsonObject change_info;
    change_info.set("status"sv, "complete"sv);
    tab_did_update(view, move(change_info));
}

void ExtensionHost::view_did_change_title(ViewImplementation& view)
{
    if (!dispatches_events() || !is_tab_view(view) || !m_announced_tabs.contains(view.view_id()))
        return;
    JsonObject change_info;
    change_info.set("title"sv, view.title().to_well_formed_utf8());
    tab_did_update(view, move(change_info));
}

// https://developer.chrome.com/docs/extensions/reference/api/tabs#event-onRemoved
void ExtensionHost::view_will_be_destroyed(u64 view_id)
{
    if (!m_announced_tabs.remove(view_id) || !dispatches_events())
        return;

    JsonObject remove_info;
    remove_info.set("windowId"sv, window_id);
    remove_info.set("isWindowClosing"sv, false);
    JsonArray arguments;
    arguments.must_append(static_cast<i64>(view_id));
    arguments.must_append(move(remove_info));
    broadcast_to_extension_pages("tabs.onRemoved"sv, arguments);
}

// https://developer.chrome.com/docs/extensions/reference/api/tabs#event-onActivated
void ExtensionHost::active_view_did_change(Optional<u64> view_id)
{
    if (m_active_view_id == view_id)
        return;
    m_active_view_id = view_id;
    if (!view_id.has_value() || !dispatches_events())
        return;
    auto view = tab_view_with_id(static_cast<i64>(*view_id));
    if (!view.has_value())
        return;

    JsonObject active_info;
    active_info.set("tabId"sv, static_cast<i64>(*view_id));
    active_info.set("windowId"sv, window_id);
    JsonArray arguments;
    arguments.must_append(move(active_info));
    broadcast_to_extension_pages("tabs.onActivated"sv, arguments);
}

ExtensionHost::ExtensionHost() = default;
ExtensionHost::~ExtensionHost() = default;

HistoryStore* ExtensionHost::history_store()
{
    if (m_tried_opening_history)
        return m_history_store.ptr();
    m_tried_opening_history = true;

    if (Application::browser_options().disable_sql_database == DisableSQLDatabase::Yes)
        return nullptr;

    auto database = Database::Database::create(Application::profile().paths().data, "History"sv);
    if (database.is_error()) {
        warnln("Extensions: Unable to open the history database: {}", database.error());
        return nullptr;
    }
    auto store = HistoryStore::create(*database.value());
    if (store.is_error()) {
        warnln("Extensions: Unable to read the history: {}", store.error());
        return nullptr;
    }
    m_history_database = database.release_value();
    m_history_store = store.release_value();
    return m_history_store.ptr();
}

// https://developer.chrome.com/docs/extensions/reference/api/history
void ExtensionHost::history_call(StringView function, JsonArray const& arguments, Completion complete)
{
    if (function != "search"sv) {
        complete(error(MUST(String::formatted("chrome.history.{} is not implemented yet", function))));
        return;
    }

    auto query = arguments.is_empty() || !arguments.at(0).is_object() ? JsonObject {} : arguments.at(0).as_object();
    auto text = query.get_string("text"sv).value_or({});
    // Without a start time, Chromium searches the last 24 hours.
    auto start_time = query.get_double_with_precision_loss("startTime"sv).value_or(static_cast<double>(milliseconds_since_epoch()) - 24 * 60 * 60 * 1000);
    auto end_time = query.get_double_with_precision_loss("endTime"sv);
    auto max_results = query.get_integer<i64>("maxResults"sv).value_or(100);

    JsonArray items;
    auto* store = history_store();
    if (store && max_results > 0) {
        // The entries come most recent first, so those before the start time end the search.
        auto entries = store->list_entries(text, 0, end_time.has_value() ? 1000 : static_cast<size_t>(max_results));
        for (auto const& entry : entries) {
            auto visited = static_cast<double>(entry.last_visited_time.milliseconds_since_epoch());
            if (end_time.has_value() && visited > *end_time)
                continue;
            if (visited < start_time || items.size() >= static_cast<size_t>(max_results))
                break;
            JsonObject item;
            item.set("id"sv, entry.url);
            item.set("url"sv, entry.url);
            item.set("title"sv, entry.title.value_or({}));
            item.set("lastVisitTime"sv, visited);
            item.set("visitCount"sv, entry.visit_count);
            item.set("typedCount"sv, entry.direct_visit_count);
            items.must_append(move(item));
        }
    }
    complete(result(items));
}

// The bookmarks that the UI keeps, as the nodes of the bookmarks API. Like Chromium, the root holds the bookmarks
// bar, which holds the bookmarks, and the other bookmarks, which are none.
static JsonObject bookmark_tree()
{
    auto path = LexicalPath::join(Application::profile().paths().config, "Bookmarks.json"sv).string();
    JsonArray items;
    if (auto saved = read_json_file(path); !saved.is_error()) {
        if (auto saved_items = saved.value().get_array("items"sv); saved_items.has_value())
            items = *saved_items;
    }

    Function<JsonObject(JsonObject const&, StringView, size_t)> node_for = [&](JsonObject const& item, StringView parent_id, size_t index) {
        JsonObject node;
        node.set("id"sv, item.get_string("id"sv).value_or({}));
        node.set("parentId"sv, parent_id);
        node.set("index"sv, static_cast<i64>(index));
        node.set("title"sv, item.get_string("title"sv).value_or({}));
        if (auto date_added = item.get_integer<i64>("dateAdded"sv); date_added.has_value())
            node.set("dateAdded"sv, *date_added);
        if (item.get_string("type"sv).value_or({}) == "folder"sv) {
            JsonArray children;
            auto id = item.get_string("id"sv).value_or({});
            if (auto item_children = item.get_array("children"sv); item_children.has_value()) {
                for (size_t i = 0; i < item_children->size(); ++i) {
                    if (item_children->at(i).is_object())
                        children.must_append(node_for(item_children->at(i).as_object(), id, i));
                }
            }
            node.set("children"sv, move(children));
        } else {
            node.set("url"sv, item.get_string("url"sv).value_or({}));
        }
        return node;
    };

    auto folder = [](StringView id, StringView title, size_t index, JsonArray children) {
        JsonObject node;
        node.set("id"sv, id);
        node.set("parentId"sv, "0"sv);
        node.set("index"sv, static_cast<i64>(index));
        node.set("title"sv, title);
        node.set("children"sv, move(children));
        return node;
    };

    JsonArray bar_children;
    for (size_t i = 0; i < items.size(); ++i) {
        if (items.at(i).is_object())
            bar_children.must_append(node_for(items.at(i).as_object(), "1"sv, i));
    }

    JsonArray root_children;
    root_children.must_append(folder("1"sv, "Bookmarks bar"sv, 0, move(bar_children)));
    root_children.must_append(folder("2"sv, "Other bookmarks"sv, 1, {}));

    JsonObject root;
    root.set("id"sv, "0"sv);
    root.set("title"sv, ""sv);
    root.set("children"sv, move(root_children));
    return root;
}

static void for_each_bookmark_node(JsonObject const& node, Function<void(JsonObject const&)> const& callback)
{
    callback(node);
    if (auto children = node.get_array("children"sv); children.has_value()) {
        children->for_each([&](auto const& child) {
            if (child.is_object())
                for_each_bookmark_node(child.as_object(), callback);
        });
    }
}

static JsonObject without_children(JsonObject node)
{
    node.remove("children"sv);
    return node;
}

// https://developer.chrome.com/docs/extensions/reference/api/bookmarks
void ExtensionHost::bookmarks_call(StringView function, JsonArray const& arguments, Completion complete)
{
    auto argument = arguments.is_empty() ? JsonValue {} : arguments.at(0);
    auto tree = bookmark_tree();

    if (function == "getTree"sv) {
        JsonArray roots;
        roots.must_append(move(tree));
        return complete(result(roots));
    }

    auto ids_of = [&](JsonValue const& value) {
        Vector<String> ids;
        if (value.is_string())
            ids.append(value.as_string());
        else if (value.is_array())
            value.as_array().for_each([&](auto const& id) { if (id.is_string()) ids.append(id.as_string()); });
        return ids;
    };

    if (function == "get"sv || function == "getChildren"sv || function == "getSubTree"sv) {
        auto ids = ids_of(argument);
        JsonArray nodes;
        for_each_bookmark_node(tree, [&](JsonObject const& node) {
            auto id = node.get_string("id"sv).value_or({});
            if (!ids.contains_slow(id))
                return;
            if (function == "getChildren"sv) {
                if (auto children = node.get_array("children"sv); children.has_value())
                    children->for_each([&](auto const& child) { nodes.must_append(without_children(child.as_object())); });
            } else {
                nodes.must_append(function == "get"sv ? without_children(node) : node);
            }
        });
        if (nodes.is_empty() && function != "getChildren"sv)
            return complete(error("Can't find bookmark for id."sv));
        return complete(result(nodes));
    }

    if (function == "search"sv || function == "getRecent"sv) {
        // A search matches bookmarks whose URL or title has every word of the query.
        Vector<String> words;
        Optional<String> url;
        Optional<String> title;
        if (argument.is_string()) {
            for (auto word : argument.as_string().bytes_as_string_view().split_view(' '))
                words.append(MUST(String::from_utf8(word)));
        } else if (argument.is_object()) {
            if (auto query = argument.as_object().get_string("query"sv); query.has_value()) {
                for (auto word : query->bytes_as_string_view().split_view(' '))
                    words.append(MUST(String::from_utf8(word)));
            }
            if (auto value = argument.as_object().get_string("url"sv); value.has_value())
                url = *value;
            if (auto value = argument.as_object().get_string("title"sv); value.has_value())
                title = *value;
        }

        Vector<JsonObject> bookmarks;
        for_each_bookmark_node(tree, [&](JsonObject const& node) {
            auto node_url = node.get_string("url"sv);
            if (!node_url.has_value())
                return;
            auto node_title = node.get_string("title"sv).value_or({});
            if (url.has_value() && *url != *node_url)
                return;
            if (title.has_value() && *title != node_title)
                return;
            for (auto const& word : words) {
                if (!node_url->contains(word, CaseSensitivity::CaseInsensitive) && !node_title.contains(word, CaseSensitivity::CaseInsensitive))
                    return;
            }
            bookmarks.append(node);
        });

        if (function == "getRecent"sv) {
            quick_sort(bookmarks, [](auto const& a, auto const& b) {
                return a.get_double_with_precision_loss("dateAdded"sv).value_or(0) > b.get_double_with_precision_loss("dateAdded"sv).value_or(0);
            });
            auto count = argument.get_integer<i64>().value_or(0);
            if (count >= 0 && bookmarks.size() > static_cast<size_t>(count))
                bookmarks.shrink(static_cast<size_t>(count));
        }

        JsonArray nodes;
        for (auto& bookmark : bookmarks)
            nodes.must_append(move(bookmark));
        return complete(result(nodes));
    }

    // FIXME: Change bookmarks, which the UI keeps, through the UI.
    complete(error(MUST(String::formatted("chrome.bookmarks.{} is not implemented yet", function))));
}

}
