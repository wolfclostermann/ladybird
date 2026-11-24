/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebView/Application.h>
#include <LibWebCommon/Extensions/FrameId.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/ExtensionHost.h>
#include <LibWebView/Extensions.h>
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

static bool view_is_active(ViewImplementation& view)
{
    return view.traversable().system_visibility_state() == Web::HTML::VisibilityState::Visible;
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
    tab.set("status"sv, "complete"sv);
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

    if (api == "tabs.getZoom"sv)
        return complete(result(1.0));

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

    JsonObject target;
    target.set("contextType"sv, "extension_page"sv);
    dispatch_event(*background_view, extension.id, target, "runtime.onMessage"sv, event_arguments, [complete = move(complete)](Optional<String> response) mutable {
        complete({ .is_error = false, .payload = response.value_or({}) });
    });
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

}
