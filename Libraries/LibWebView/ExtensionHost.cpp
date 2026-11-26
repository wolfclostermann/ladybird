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
#include <LibWebCommon/StorageAPI/StorageKey.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/ExtensionActions.h>
#include <LibWebView/ExtensionHost.h>
#include <LibWebView/Extensions.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/SearchEngine.h>
#include <LibWebView/Settings.h>
#include <LibDatabase/Database.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibCore/File.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

// Like Chromium, every tab is in the one window until windows are known to the extension APIs.
static constexpr i64 window_id = 1;

static bool is_tab_view(ViewImplementation const&);

// The views that show tabs, in the order the extension APIs index them: the order of the UI's tab strips. Views the UI
// hasn't placed yet follow, oldest first. Views without a window, such as those of background pages, are not tabs.
static Vector<ViewImplementation&> tab_views()
{
    Vector<ViewImplementation&> views;
    auto tab_order = Application::the().tab_order();
    for (auto view_id : tab_order) {
        if (auto view = ViewImplementation::find_view_by_id(view_id); view.has_value() && is_tab_view(*view))
            views.append(*view);
    }

    Vector<ViewImplementation*> unplaced_views;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (is_tab_view(view) && !tab_order.contains_slow(view.view_id()))
            unplaced_views.append(&view);
        return IterationDecision::Continue;
    });
    quick_sort(unplaced_views, [](auto const* a, auto const* b) { return a->view_id() < b->view_id(); });
    for (auto* view : unplaced_views)
        views.append(*view);
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
    tab.set("pinned"sv, view.is_pinned());
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

// The permission that the functions of an API namespace need, if they need one. Those of tabs, windows, runtime and
// the like need none, though the tabs API leaves out what a tab shows unless the extension may see it.
// https://developer.chrome.com/docs/extensions/reference/permissions-list
static Optional<StringView> permission_needed_for(StringView api)
{
    auto name = api.substring_view(0, api.find('.').value_or(api.length()));
    if (name.is_one_of("storage"sv, "history"sv, "bookmarks"sv, "sessions"sv, "search"sv, "scripting"sv, "webNavigation"sv,
            "notifications"sv, "alarms"sv, "contextMenus"sv, "offscreen"sv))
        return name;
    return {};
}

// A tab object as an extension may see it: without the URL and title of a page it has no access to, as Chromium
// scrubs them.
// https://developer.chrome.com/docs/extensions/reference/api/tabs#type-Tab
static JsonObject tab_as_seen_by(LoadedExtension const& extension, JsonObject tab)
{
    auto tab_id = tab.get_integer<i64>("id"sv);
    auto may_see = [&](StringView key) {
        auto url = tab.get_string(key);
        if (!url.has_value())
            return true;
        auto parsed = URL::Parser::basic_parse(*url);
        if (!parsed.has_value())
            return false;
        if (extension_may_see_tab_with_url(extension, *parsed))
            return true;
        return tab_id.has_value() && *tab_id >= 0 && Application::the().extension_host().has_active_tab_access(extension.id, static_cast<u64>(*tab_id), *parsed);
    };
    if (!may_see("url"sv)) {
        tab.remove("url"sv);
        tab.remove("title"sv);
        tab.remove("favIconUrl"sv);
    }
    if (!may_see("pendingUrl"sv))
        tab.remove("pendingUrl"sv);
    return tab;
}

// Scrubs the tabs in what the tabs and windows APIs give: a tab, a window with its tabs, or an array of either.
static JsonValue tabs_as_seen_by(LoadedExtension const& extension, JsonValue const& value)
{
    if (value.is_array()) {
        JsonArray array;
        value.as_array().for_each([&](auto const& element) { array.must_append(tabs_as_seen_by(extension, element)); });
        return array;
    }
    if (!value.is_object())
        return value;
    auto const& object = value.as_object();
    if (object.has("index"sv) && object.has("windowId"sv))
        return tab_as_seen_by(extension, object);
    if (auto tabs = object.get_array("tabs"sv); tabs.has_value()) {
        auto window = object;
        window.set("tabs"sv, tabs_as_seen_by(extension, *tabs));
        return window;
    }
    return value;
}

// Whether an active document of the page could have run one of the extension's content scripts: whether the URL it
// was created with, or the URL its navigable shows now, is one the extension's content scripts match.
// NB: The WebContent process matches about:blank, srcdoc, data: and blob: documents by the URL of their creator, which
//     is in the same process, so those need no match of their own here.
bool ExtensionHost::page_matches_content_scripts_of(WebContentPage const& page, LoadedExtension const& extension) const
{
    auto url_is_matched = [&](URL::URL const& url) {
        for (auto const& content_script_value : extension.content_scripts.values()) {
            if (!content_script_value.is_object())
                continue;
            auto const& content_script = content_script_value.as_object();
            auto matches = [&](JsonValue const& pattern) { return pattern.is_string() && Web::Extensions::match_pattern_matches(pattern.as_string(), url); };
            auto patterns = content_script.get_array("matches"sv);
            auto excluded_patterns = content_script.get_array("exclude_matches"sv);
            if (patterns.has_value() && any_of(patterns->values(), matches) && !(excluded_patterns.has_value() && any_of(excluded_patterns->values(), matches)))
                return true;
        }
        return false;
    };

    bool matched = false;
    page.for_each_navigable_with_a_hosted_active_document([&](CanonicalNavigable const& navigable) {
        matched = url_is_matched(navigable.active_document().creation_url()) || url_is_matched(navigable.active_document_url());
        return matched ? IterationDecision::Break : IterationDecision::Continue;
    });
    return matched;
}

// Like Chromium, the UI process decides what a page may do with an extension by what the page hosts, so that a
// WebContent process which has been taken over cannot speak for an extension it doesn't host.
// NB: Without site isolation, an extension's frame shares a process with the web page around it, which can therefore
//     act as the extension.
Optional<ExtensionHost::Caller> ExtensionHost::caller_for(WebContentPage const& page, LoadedExtension const& extension)
{
    if (!page.is_open() || !page.has_traversable())
        return {};

    // NB: What a process says it hosts can't be trusted; which documents the UI process gave it can. Site isolation
    //     gives the extension's documents a process of their own.
    if (page.client().may_speak_for_extension(extension.id))
        return Caller::ExtensionPage;

    if (page_matches_content_scripts_of(page, extension))
        return Caller::ContentScript;

    if (auto pages = m_pages_with_injected_scripts.get(extension.id); pages.has_value()) {
        pages->remove_all_matching([](auto const& injected_page) { return !injected_page->is_open(); });
        if (any_of(*pages, [&](auto const& injected_page) { return injected_page.ptr() == &page; }))
            return Caller::ContentScript;
    }
    return {};
}

// https://developer.chrome.com/docs/extensions/develop/concepts/content-scripts#capabilities
// Content scripts may use storage, and runtime only to message the extension. Session storage is theirs only once the
// extension allows it.
bool ExtensionHost::content_script_may_call(LoadedExtension const& extension, StringView api) const
{
    if (api.is_one_of("runtime.sendMessage"sv, "runtime.connect"sv))
        return true;
    if (api.starts_with("storage."sv)) {
        if (api.ends_with(".setAccessLevel"sv))
            return false;
        if (api.starts_with("storage.session."sv))
            return m_session_storage_open_to_content_scripts.contains(extension.id);
        return true;
    }
    return false;
}

void ExtensionHost::handle_api_call(WebContentPage& page, String const& extension_id, String const& context_json, String const& api, String const& arguments_json, u64 call_id)
{
    auto complete = [page = NonnullRefPtr { page }, call_id](CallResult result) {
        page->async_complete_extension_api_call(call_id, result.is_error, result.payload);
    };

    auto const* extension = Application::the().extension_with_id(extension_id);
    if (!extension) {
        complete(error("No such extension"sv));
        return;
    }

    auto caller = caller_for(page, *extension);
    if (!caller.has_value()) {
        dbgln("Extension API: refused {} for extension {} from a page that hosts none of its contexts", api, extension_id);
        complete(error("This page has no context of the extension"sv));
        return;
    }

    if (auto permission = permission_needed_for(api); permission.has_value() && !extension_has_permission(*extension, *permission)) {
        complete(error(MUST(String::formatted("chrome.{} needs the \"{}\" permission", api, *permission))));
        return;
    }

    JsonArray arguments;
    if (auto parsed = JsonParser::parse(arguments_json); !parsed.is_error() && parsed.value().is_array())
        arguments = parsed.value().as_array();

    Context context;
    if (auto parsed = JsonParser::parse(context_json); !parsed.is_error() && parsed.value().is_object()) {
        auto const& object = parsed.value().as_object();
        context.type = object.get_string("contextType"sv).value_or({});
        context.frame_id = object.get_integer<u64>("frameId"sv).value_or(0);
        context.url = object.get_string("url"sv).value_or({});
    }

    // The background page opens session storage to content scripts as it starts, which those in a tab that loaded first
    // must wait for, as Chromium starts an extension's service worker before its tabs load.
    if (*caller == Caller::ContentScript && api.starts_with_bytes("storage.session."sv) && !content_script_may_call(*extension, api)
        && Application::the().extension_background_view(extension->id) && !m_loaded_background_pages.contains(extension->id)) {
        m_waiting_for_background_page.ensure(extension->id).append([this, page = NonnullRefPtr { page }, extension_id, context_json, api, arguments_json, call_id] {
            handle_api_call(*page, extension_id, context_json, api, arguments_json, call_id);
        });
        return;
    }

    if (*caller == Caller::ContentScript) {
        if (!content_script_may_call(*extension, api)) {
            dbgln("Extension API: refused {} for a content script of extension {}", api, extension_id);
            complete(error(MUST(String::formatted("chrome.{} is not available to content scripts", api))));
            return;
        }

        // A content script is what it is, in the frame it is in, whatever the page says. The frame must be one the page
        // hosts, and its URL is the one the UI process knows.
        context.type = "content_script"_string;
        Optional<CanonicalNavigable const&> frame;
        page.for_each_navigable_with_a_hosted_active_document([&](CanonicalNavigable const& navigable) {
            if (frame_id_of(navigable) != context.frame_id)
                return IterationDecision::Continue;
            frame = navigable;
            return IterationDecision::Break;
        });
        if (frame.has_value()) {
            context.url = frame->active_document_url().serialize();
        } else if (!context.url.is_one_of("about:blank"sv, "about:srcdoc"sv)) {
            // NB: The UI process doesn't know of a frame until its first navigation, so a frame that still shows its
            //     initial about:blank document, such as one a content script just created, is taken at its word.
            complete(error("The frame of the content script is gone"sv));
            return;
        }
    }

    call(page, *extension, context, api, arguments, move(complete));
}

// The URL against which a context's relative paths are resolved. A service worker's are relative to its script, which
// runs in a page made for it here.
static URL::URL base_url_for_paths(LoadedExtension const& extension, auto const& context)
{
    auto root = URL::Parser::basic_parse(MUST(String::formatted("chrome-extension://{}/", extension.id)));
    VERIFY(root.has_value());

    auto url = URL::Parser::basic_parse(context.url);
    if (!url.has_value())
        return *root;
    if (url->serialize_path() == MUST(String::formatted("/{}", generated_background_page_path))) {
        if (auto background = extension.manifest.get_object("background"sv); background.has_value()) {
            if (auto worker = background->get_string("service_worker"sv); worker.has_value()) {
                if (auto worker_url = URL::Parser::basic_parse(*worker, *root); worker_url.has_value())
                    return *worker_url;
            }
        }
    }
    return *url;
}

void ExtensionHost::call(WebContentPage& page, LoadedExtension const& extension, Context const& context, StringView api, JsonArray const& arguments, Completion complete)
{
    // What the tabs and windows APIs give leaves out what the extension may not see of the tabs.
    if (api.starts_with("tabs."sv) || api.starts_with("windows."sv)) {
        // NB: The extension may be turned off or removed before the call completes, so it is looked up again.
        complete = [extension_id = extension.id, complete = move(complete)](CallResult call_result) mutable {
            if (!call_result.is_error && !call_result.payload.is_empty()) {
                auto const* extension = Application::the().extension_with_id(extension_id);
                if (!extension) {
                    complete(error("The extension was turned off"sv));
                    return;
                }
                if (auto parsed = JsonParser::parse(call_result.payload); !parsed.is_error())
                    call_result.payload = tabs_as_seen_by(*extension, parsed.value()).serialized();
            }
            complete(move(call_result));
        };
    }

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
            if (auto pinned = query.get_bool("pinned"sv); pinned.has_value() && *pinned != view.is_pinned())
                continue;
            if (auto id = query.get_integer<i64>("windowId"sv); id.has_value() && *id >= 0 && *id != window_id)
                continue;

            if (auto url = query.get("url"sv); url.has_value()) {
                Vector<String> patterns;
                if (url->is_string())
                    patterns.append(url->as_string());
                else if (url->is_array())
                    url->as_array().for_each([&](auto const& value) { if (value.is_string()) patterns.append(value.as_string()); });
                // Only a tab whose URL the extension may see can match one.
                auto view_url = view.url();
                if (!extension_may_see_tab_with_url(extension, view_url) && !has_active_tab_access(extension.id, view.view_id(), view_url))
                    continue;
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

    // The size the page in a view would fit in, which only the popup of a toolbar button takes on, as in Chromium.
    // https://developer.chrome.com/docs/extensions/develop/ui/add-popup
    if (api == "_popup.setSize"sv) {
        auto& view = page.view();
        auto width = argument(0).get_double_with_precision_loss();
        auto height = argument(1).get_double_with_precision_loss();
        if (view.owner_view_id().has_value() && width.has_value() && height.has_value())
            Application::the().extension_popup_did_request_size(view.view_id(), { clamp(*width, 0.0, 10000.0), clamp(*height, 0.0, 10000.0) });
        return complete(undefined_result());
    }

    if (api.starts_with("action."sv) || api.starts_with("browserAction."sv)) {
        auto function = api.substring_view(api.find('.').value() + 1);
        if (function == "openPopup"sv)
            return complete(open_action_popup(extension, argument(0)));
        return complete(m_actions->call(extension, base_url_for_paths(extension, context), function, arguments));
    }

    if (api.starts_with("tabs."sv))
        return tabs_call(page, extension, api.substring_view(5), arguments, move(complete));
    if (api.starts_with("scripting."sv))
        return scripting_call(page, extension, api.substring_view(10), arguments, move(complete));

    if (api.starts_with("history."sv))
        return history_call(api.substring_view(8), arguments, move(complete));
    if (api.starts_with("bookmarks."sv))
        return bookmarks_call(api.substring_view(10), arguments, move(complete));

    if (api == "search.query"sv)
        return search_query(page, arguments, move(complete));

    // https://developer.chrome.com/docs/extensions/reference/api/sessions#method-restore
    if (api == "sessions.restore"sv) {
        // FIXME: Restore a session by its ID, once sessions.getRecentlyClosed gives out IDs, and resolve with the
        //        session that was restored.
        if (!arguments.is_empty() && !arguments.at(0).is_null())
            return complete(error("Restoring a session by its ID is not supported yet"sv));
        Application::the().reopen_recently_closed_tab();
        return complete(undefined_result());
    }

    if (api == "windows.update"sv)
        return complete(result(window_object(false)));

    // https://developer.chrome.com/docs/extensions/reference/api/permissions
    if (api.starts_with("permissions."sv))
        return permissions_call(extension, api.substring_view(12), arguments, move(complete));

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
                sender.set("tab"sv, tab_as_seen_by(extension, tab_object_for(views[i], i)));
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

    // NB: The message may wait for the background page, which is gone if the extension is turned off meanwhile.
    auto send = [this, extension_id = extension.id, event_arguments = move(event_arguments), complete = move(complete)]() mutable {
        auto* background_view = Application::the().extension_background_view(extension_id);
        if (!background_view) {
            complete(error("Could not establish connection. Receiving end does not exist."sv));
            return;
        }
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

    // https://developer.chrome.com/docs/extensions/reference/api/storage#method-StorageArea-setAccessLevel
    if (function == "setAccessLevel"sv) {
        if (area != "session"sv) {
            complete(error("Only the session storage area has an access level"sv));
            return;
        }
        auto access_level = argument.is_object() ? argument.as_object().get_string("accessLevel"sv) : Optional<String> {};
        if (access_level == "TRUSTED_AND_UNTRUSTED_CONTEXTS"sv)
            m_session_storage_open_to_content_scripts.set(extension.id);
        else if (access_level == "TRUSTED_CONTEXTS"sv)
            m_session_storage_open_to_content_scripts.remove(extension.id);
        else {
            complete(error("Unknown access level"sv));
            return;
        }
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
    m_pending_dispatches.set(dispatch_id, { .page = &page, .on_complete = move(on_complete) });
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

void ExtensionHost::did_finish_event(WebContentPage const& page, u64 dispatch_id, Optional<String> response_json)
{
    // Only the page that an event went to may answer it.
    auto pending = m_pending_dispatches.get(dispatch_id);
    if (!pending.has_value() || pending->page != &page)
        return;
    auto on_complete = move(m_pending_dispatches.take(dispatch_id)->on_complete);
    on_complete(move(response_json));
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

    if (function == "move"sv) {
        Vector<ViewImplementation*> moving_views;
        auto ids = tab_id.is_array() ? tab_id.as_array().values() : Vector<JsonValue> { tab_id };
        for (auto const& id : ids) {
            auto id_value = id.get_integer<i64>();
            auto view = id_value.has_value() ? tab_view_with_id(*id_value) : Optional<ViewImplementation&> {};
            if (!view.has_value())
                return complete(no_tab(id));
            moving_views.append(&*view);
        }

        auto properties = argument(1).is_object() ? argument(1).as_object() : JsonObject {};
        auto index = properties.get_integer<i64>("index"sv);
        if (!index.has_value())
            return complete(error("No index given to move tabs to"sv));

        // The tabs go to consecutive positions from the index, in the order given, where -1 is the end. The UI moves
        // them one after another, so each move names the tab whose place the tab takes once the earlier ones are done.
        Vector<ViewImplementation*> order;
        for (auto& view : tab_views())
            order.append(&view);
        Vector<size_t> new_indices;
        for (size_t i = 0; i < moving_views.size(); ++i) {
            auto* view = moving_views[i];
            auto old_index = order.find_first_index(view);
            VERIFY(old_index.has_value());

            auto new_index = order.size() - 1;
            if (*index >= 0)
                new_index = min(static_cast<size_t>(*index) + i, order.size() - 1);

            Optional<u64> target_view_id;
            if (new_index < order.size() - 1)
                target_view_id = order[new_index]->view_id();
            if (new_index != *old_index && view->on_move_tab) {
                m_requested_tab_moves.set(view->view_id());
                view->on_move_tab(target_view_id);
            }

            order.remove(*old_index);
            order.insert(new_index, view);
        }
        // tabs.onMoved fires once the UI reports the new order, as for the moves the user makes.

        // The UI hasn't moved the tabs yet, so the tabs report the places they are about to have.
        JsonArray tabs;
        for (auto* view : moving_views) {
            auto tab = tab_object_for(*view, order.find_first_index(view).value());
            tabs.must_append(move(tab));
        }
        if (tab_id.is_array())
            return complete(result(move(tabs)));
        return complete(result(tabs.at(0)));
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
        auto pinned = properties.get_bool("pinned"sv);
        if (pinned.has_value() && *pinned != view->is_pinned() && view->on_pin_tab) {
            // Pinning moves the tab to the end of the pinned tabs, and unpinning may move it to after them.
            m_requested_tab_moves.set(view->view_id());
            view->on_pin_tab(*pinned);
        }

        // The UI hasn't pinned or unpinned the tab yet, so the tab reports the state it is about to have.
        auto tab = tab_object_with_id(static_cast<i64>(view->view_id()));
        if (!tab.has_value())
            return complete(undefined_result());
        if (pinned.has_value() && view->on_pin_tab)
            tab->set("pinned"sv, *pinned);
        return complete(result(tab.release_value()));
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

// https://developer.chrome.com/docs/extensions/reference/api/search#method-query
void ExtensionHost::search_query(WebContentPage& page, JsonArray const& arguments, Completion complete)
{
    auto query_info = !arguments.is_empty() && arguments.at(0).is_object() ? arguments.at(0).as_object() : JsonObject {};
    auto text = query_info.get_string("text"sv);
    if (!text.has_value())
        return complete(error("No text given to search for"sv));

    auto const& engine = Application::settings().search_engine_settings().engine;
    if (!engine.has_value())
        return complete(error("No search engine is set"sv));
    auto url = URL::Parser::basic_parse(engine->format_search_query_for_navigation(*text));
    if (!url.has_value())
        return complete(error("The search engine gave an invalid URL"sv));

    auto disposition = query_info.get_string("disposition"sv);
    auto tab_id = query_info.get_integer<i64>("tabId"sv);
    if (disposition.has_value() && tab_id.has_value())
        return complete(error("Cannot set both 'disposition' and 'tabId'"sv));

    if (disposition == "NEW_TAB"sv) {
        Optional<ViewImplementation&> source_view = tab_view_of_page(page);
        if (!source_view.has_value())
            source_view = active_tab_view();
        Application::the().open_url_in_new_tab(*url, Web::HTML::ActivateTab::Yes, source_view.has_value() ? &*source_view : nullptr);
        return complete(undefined_result());
    }
    if (disposition == "NEW_WINDOW"sv) {
        Application::the().open_url_in_new_window(*url, IsPrivate::No);
        return complete(undefined_result());
    }

    // Otherwise the search is shown in the given tab, or the active one.
    auto view = tab_id.has_value() ? tab_view_with_id(*tab_id) : active_tab_view();
    if (!view.has_value())
        return complete(error(MUST(String::formatted("No tab with id: {}", tab_id.value_or(-1)))));
    view->load_from_browser_ui(*url);
    complete(undefined_result());
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

    // The extension needs host permissions for each frame it injects into, as in Chromium, or activeTab access to the
    // tab for the frames of the origin it was granted for. A frame showing about:blank or about:srcdoc is its parent's
    // to script.
    auto may_access = [&](URL::URL const& url) {
        return extension_has_host_access(extension, url, HostAccess::Explicit) || has_active_tab_access(extension.id, view->view_id(), url);
    };
    HashMap<u64, bool> frame_access;
    if (view->has_traversable()) {
        view->traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
            Optional<URL::URL> url;
            for (auto const* frame = &navigable; frame; frame = frame->parent()) {
                if (!frame->active_session_history_entry())
                    continue;
                auto const& frame_url = frame->active_document_url();
                if (frame_url.scheme() == "about"sv && frame_url.serialize_path().is_one_of("blank"sv, "srcdoc"sv) && frame->parent())
                    continue;
                url = frame_url;
                break;
            }
            frame_access.set(frame_id_of(navigable), url.has_value() && may_access(*url));
            return IterationDecision::Continue;
        });
    } else {
        frame_access.set(0, may_access(view->url()));
    }
    auto no_access = [&](u64 frame_id) {
        complete(error(MUST(String::formatted("Cannot access the contents of frame {} of tab {}. The extension's manifest must ask for permission to access its host.", frame_id, *tab_id))));
    };

    JsonObject frame_target;
    if (auto frame_ids = target.get_array("frameIds"sv); frame_ids.has_value()) {
        for (auto const& frame_id : frame_ids->values()) {
            auto id = frame_id.get_integer<u64>();
            if (!id.has_value() || !frame_access.get(*id).value_or(false))
                return no_access(id.value_or(0));
        }
        frame_target.set("frameIds"sv, *frame_ids);
    } else if (target.get_bool("allFrames"sv).value_or(false)) {
        // Frames the extension may not access are left out, but the tab's own document must be one it may.
        if (!frame_access.get(0).value_or(false))
            return no_access(0);
        JsonArray frame_ids;
        for (auto const& [frame_id, has_access] : frame_access) {
            if (has_access)
                frame_ids.must_append(frame_id);
        }
        frame_target.set("frameIds"sv, move(frame_ids));
    } else {
        if (!frame_access.get(0).value_or(false))
            return no_access(0);
        frame_target.set("allFrames"sv, false);
    }

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

    // Scripts injected into the isolated world are content scripts, which may then call the extension's APIs.
    if (details.get_string("kind"sv) == "script"sv && details.get_string("world"sv) != "MAIN"sv) {
        auto& injected_pages = m_pages_with_injected_scripts.ensure(extension.id);
        for (auto& page : pages) {
            if (!any_of(injected_pages, [&](auto const& injected_page) { return injected_page.ptr() == page.ptr(); }))
                injected_pages.append(page);
        }
    }

    for (auto& page : pages) {
        auto dispatch_id = ++m_next_dispatch_id;
        m_pending_dispatches.set(dispatch_id, { .page = page.ptr(), .on_complete = [state](Optional<String> results_json) {
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
        } });
        page->async_inject_into_extension_frames(extension.id, target.serialized(), details.serialized(), dispatch_id);
    }
}

bool ExtensionHost::dispatches_events() const
{
    return m_dispatches_events && !Application::the().extensions().is_empty();
}

// The events of the tabs and webNavigation APIs go to the pages of extensions, not to their content scripts.
// The arguments of an event of tabs or webNavigation as an extension may see them, or nothing if it may not see the
// event at all: the events of an API that needs a permission go only to extensions with it, and those of tabs leave out
// what the extension may not see of the tab.
static Optional<JsonArray> event_arguments_for(LoadedExtension const& extension, StringView event, JsonArray const& arguments)
{
    if (auto permission = permission_needed_for(event); permission.has_value() && !extension_has_permission(extension, *permission))
        return {};

    if (event == "tabs.onCreated"sv && arguments.size() == 1 && arguments.at(0).is_object()) {
        JsonArray scrubbed;
        scrubbed.must_append(tab_as_seen_by(extension, arguments.at(0).as_object()));
        return scrubbed;
    }

    // An update to what the extension may not see of a tab, and nothing else, is not told of.
    if (event == "tabs.onUpdated"sv && arguments.size() == 3 && arguments.at(1).is_object() && arguments.at(2).is_object()) {
        auto tab = tab_as_seen_by(extension, arguments.at(2).as_object());
        auto change_info = arguments.at(1).as_object();
        if (!tab.has("url"sv)) {
            change_info.remove("url"sv);
            change_info.remove("title"sv);
            change_info.remove("favIconUrl"sv);
            if (change_info.size() == 0)
                return {};
        }
        JsonArray scrubbed;
        scrubbed.must_append(arguments.at(0));
        scrubbed.must_append(move(change_info));
        scrubbed.must_append(move(tab));
        return scrubbed;
    }
    return arguments;
}

void ExtensionHost::broadcast_to_extension_pages(StringView event, JsonArray const& arguments)
{
    JsonObject target;
    target.set("contextType"sv, "extension_page"sv);

    for (auto const& extension : Application::the().extensions()) {
        auto extension_arguments = event_arguments_for(extension, event, arguments);
        if (!extension_arguments.has_value())
            continue;

        Vector<ViewImplementation&> views;
        ViewImplementation::for_each_view([&](ViewImplementation& view) {
            if (Application::the().extension_background_view(extension.id) == &view || (view.url().scheme() == "chrome-extension"sv && view.url().serialized_host() == extension.id))
                views.append(view);
            return IterationDecision::Continue;
        });
        for (auto& view : views)
            dispatch_event(view, extension.id, target, event, *extension_arguments, [](auto) { });
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
    auto view = navigable.top_level_traversable().view();
    if (!view.has_value() || !is_tab_view(*view))
        return;

    // As in Chromium, a tab navigating to another origin takes away the access activeTab gave to it.
    if (navigable.is_top_level_traversable()) {
        auto origin = navigable.active_document_url().origin();
        for (auto& [extension_id, grants] : m_active_tab_grants) {
            if (auto granted = grants.get(view->view_id()); granted.has_value() && !granted->is_same_origin(origin))
                grants.remove(view->view_id());
        }
    }

    if (!dispatches_events())
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

void ExtensionHost::view_did_change_pinned(ViewImplementation& view)
{
    if (!dispatches_events() || !is_tab_view(view) || !m_announced_tabs.contains(view.view_id()))
        return;
    JsonObject change_info;
    change_info.set("pinned"sv, view.is_pinned());
    tab_did_update(view, move(change_info));
}

// https://developer.chrome.com/docs/extensions/reference/api/tabs#event-onMoved
void ExtensionHost::tab_order_did_change(ReadonlySpan<u64> previous_order, ReadonlySpan<u64> order)
{
    auto requested_moves = move(m_requested_tab_moves);
    if (!dispatches_events())
        return;

    // Only the tabs in both orders count: tabs that opened or closed in between aren't moves, and onCreated and
    // onRemoved tell of them.
    auto is_tab = [&](u64 view_id) {
        auto view = ViewImplementation::find_view_by_id(view_id);
        return view.has_value() && is_tab_view(*view);
    };
    Vector<u64> before;
    for (auto view_id : previous_order) {
        if (order.contains_slow(view_id) && is_tab(view_id))
            before.append(view_id);
    }
    Vector<u64> after;
    for (auto view_id : order) {
        if (before.contains_slow(view_id))
            after.append(view_id);
    }
    if (before == after)
        return;

    auto tab_did_move = [&](u64 view_id, size_t from_index, size_t to_index) {
        JsonObject move_info;
        move_info.set("windowId"sv, window_id);
        move_info.set("fromIndex"sv, static_cast<i64>(from_index));
        move_info.set("toIndex"sv, static_cast<i64>(to_index));
        JsonArray arguments;
        arguments.must_append(static_cast<i64>(view_id));
        arguments.must_append(move(move_info));
        broadcast_to_extension_pages("tabs.onMoved"sv, arguments);
    };

    for (auto const& tab_move : tab_moves_between(before, after, requested_moves))
        tab_did_move(tab_move.view_id, tab_move.from_index, tab_move.to_index);
}

// https://developer.chrome.com/docs/extensions/reference/api/tabs#event-onRemoved
void ExtensionHost::view_will_be_destroyed(u64 view_id)
{
    m_actions->view_will_be_destroyed(view_id);
    for (auto& [extension_id, grants] : m_active_tab_grants)
        grants.remove(view_id);
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

ExtensionHost::ExtensionHost()
    : m_actions(make<ExtensionActions>())
{
    m_actions->on_change = [] {
        Application::the().extension_actions_changed();
    };
}
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

// Scales a bitmap down to fit in a square of the given size, keeping its aspect ratio, by averaging the pixels that
// each pixel of the result covers. A bitmap that fits already is left alone.
static ErrorOr<NonnullRefPtr<Gfx::Bitmap const>> fit_bitmap_in_square(Gfx::Bitmap const& bitmap, int size)
{
    auto width = bitmap.width();
    auto height = bitmap.height();
    if (width <= size && height <= size)
        return NonnullRefPtr<Gfx::Bitmap const> { bitmap };

    auto scale = static_cast<double>(size) / max(width, height);
    auto new_width = max(1, static_cast<int>(round(width * scale)));
    auto new_height = max(1, static_cast<int>(round(height * scale)));
    auto result = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Unpremultiplied, { new_width, new_height }));

    for (int y = 0; y < new_height; ++y) {
        auto y0 = y * height / new_height;
        auto y1 = max(y0 + 1, (y + 1) * height / new_height);
        for (int x = 0; x < new_width; ++x) {
            auto x0 = x * width / new_width;
            auto x1 = max(x0 + 1, (x + 1) * width / new_width);

            // The colours are weighted by their alpha, so that transparent pixels don't darken the edges.
            u64 red = 0, green = 0, blue = 0, alpha = 0, count = 0;
            for (int source_y = y0; source_y < y1; ++source_y) {
                for (int source_x = x0; source_x < x1; ++source_x) {
                    auto color = bitmap.get_pixel(source_x, source_y);
                    red += color.red() * color.alpha();
                    green += color.green() * color.alpha();
                    blue += color.blue() * color.alpha();
                    alpha += color.alpha();
                    ++count;
                }
            }
            if (alpha == 0) {
                result->set_pixel(x, y, Gfx::Color::Transparent);
                continue;
            }
            result->set_pixel(x, y, Gfx::Color(red / alpha, green / alpha, blue / alpha, alpha / count));
        }
    }
    return NonnullRefPtr<Gfx::Bitmap const> { move(result) };
}

// The size a PNG gives in its header.
// https://www.w3.org/TR/png-3/#11IHDR
static Optional<Gfx::IntSize> png_dimensions(ReadonlyBytes png)
{
    static constexpr u8 signature[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    if (png.size() < 24 || png.slice(0, 8) != ReadonlyBytes { signature, 8 } || png.slice(12, 4) != "IHDR"sv.bytes())
        return {};
    auto read_u32 = [&](size_t offset) {
        return static_cast<u32>(png[offset]) << 24 | static_cast<u32>(png[offset + 1]) << 16 | static_cast<u32>(png[offset + 2]) << 8 | png[offset + 3];
    };
    auto width = read_u32(16);
    auto height = read_u32(20);
    if (width > NumericLimits<i32>::max() || height > NumericLimits<i32>::max())
        return {};
    return Gfx::IntSize { static_cast<int>(width), static_cast<int>(height) };
}

Optional<ByteBuffer> ExtensionHost::favicon_png_for_page(URL::URL const& page_url, int size)
{
    RefPtr<Gfx::Bitmap const> favicon;
    for (auto& view : tab_views()) {
        if (view.url() == page_url && view.favicon()) {
            favicon = view.favicon();
            break;
        }
    }

    if (!favicon) {
        auto* store = history_store();
        auto entry = store ? store->entry_for_url(page_url) : Optional<HistoryEntry> {};
        if (!entry.has_value() || !entry->favicon_png.has_value())
            return {};

        // FIXME: Scale down a larger favicon from the history too. Images are decoded in another process, so this
        //        would have to wait for it. Until then, only one that fits is given out.
        auto png_size = png_dimensions(entry->favicon_png->bytes());
        if (!png_size.has_value() || png_size->width() > size || png_size->height() > size)
            return {};
        return move(entry->favicon_png);
    }

    auto fitted = fit_bitmap_in_square(*favicon, size);
    if (fitted.is_error())
        return {};
    auto png = Gfx::PNGWriter::encode(*fitted.value());
    if (png.is_error())
        return {};
    return png.release_value();
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

void ExtensionHost::extension_did_unload(String const& extension_id)
{
    // NB: Storage is saved as it changes, so nothing is lost by dropping it.
    m_storage.remove(extension_id);
    m_session_storage_open_to_content_scripts.remove(extension_id);
    m_pages_with_injected_scripts.remove(extension_id);
    m_loaded_background_pages.remove(extension_id);
    m_active_tab_grants.remove(extension_id);

    // The calls that waited for its background page fail now that it is gone.
    for (auto& send : m_waiting_for_background_page.take(extension_id).value_or({}))
        send();

    m_actions->forget(extension_id);
}

String ExtensionHost::serialized_actions()
{
    return m_actions->serialized(Application::the().extensions(), Application::the().pinned_extension_ids());
}

static Optional<URL::URL> action_popup_url(LoadedExtension const& extension, StringView popup)
{
    return URL::Parser::basic_parse(MUST(String::formatted("chrome-extension://{}/{}", extension.id, popup.trim("/"sv, TrimMode::Left))));
}

// https://developer.chrome.com/docs/extensions/reference/api/action#method-openPopup
ExtensionHost::CallResult ExtensionHost::open_action_popup(LoadedExtension const& extension, JsonValue const& options)
{
    if (!action_of(extension).has_value())
        return error("The extension has no action"sv);

    // There is a single window, whose ID is 1.
    if (options.is_object()) {
        if (auto window_id = options.as_object().get("windowId"sv); window_id.has_value() && !window_id->is_null()) {
            auto id = window_id->get_double_with_precision_loss();
            if (!id.has_value() || (*id != 1 && *id != -2))
                return error(MUST(String::formatted("No window with id: {}.", window_id->serialized())));
        }
    }

    auto view = active_tab_view();
    if (!view.has_value())
        return error("Could not find an active browser window."sv);

    auto click = m_actions->click(extension, view->view_id());
    if (!click.popup.has_value())
        return error("Extension does not have a popup on the active tab."sv);
    auto url = action_popup_url(extension, *click.popup);
    if (!url.has_value())
        return error("Extension does not have a popup on the active tab."sv);

    // NB: Unlike Chromium, the promise settles before the popup has opened, as the UI does not say when it has.
    Application::the().show_extension_popup(extension.id, view->view_id(), *url);
    return undefined_result();
}

void ExtensionHost::action_clicked(String const& extension_id, u64 view_id)
{
    auto const* extension = Application::the().extension_with_id(extension_id);
    if (!extension || !action_of(*extension).has_value())
        return;
    auto views = tab_views();
    Optional<size_t> index;
    for (size_t i = 0; i < views.size(); ++i) {
        if (views[i].view_id() == view_id)
            index = i;
    }
    if (!index.has_value())
        return;

    auto click = m_actions->click(*extension, view_id);
    if (!click.enabled)
        return;

    // Clicking the button invokes the extension on the tab, whether it opens a popup or not.
    grant_active_tab(*extension, views[*index]);

    if (click.popup.has_value()) {
        auto url = action_popup_url(*extension, *click.popup);
        if (url.has_value())
            Application::the().show_extension_popup(extension->id, view_id, *url);
        return;
    }

    if (!dispatches_events())
        return;
    JsonArray arguments;
    arguments.must_append(tabs_as_seen_by(*extension, tab_object_for(views[*index], *index)));
    broadcast_event(extension->id, "action.onClicked"sv, arguments);
}

static Vector<String> strings_in(JsonObject const& object, StringView key)
{
    Vector<String> strings;
    if (auto array = object.get_array(key); array.has_value())
        array->for_each([&](auto const& value) { if (value.is_string()) strings.append(value.as_string()); });
    return strings;
}

static JsonObject permissions_as_json(Vector<String> const& permissions, Vector<String> const& origins)
{
    JsonArray permissions_json;
    for (auto const& permission : permissions)
        permissions_json.must_append(permission);
    JsonArray origins_json;
    for (auto const& origin : origins)
        origins_json.must_append(origin);
    JsonObject object;
    object.set("permissions"sv, move(permissions_json));
    object.set("origins"sv, move(origins_json));
    return object;
}

// What the user is told an extension asks for, much as Chromium words its warnings.
// https://developer.chrome.com/docs/extensions/reference/permissions-list
static String describe_permission(StringView permission)
{
    if (permission == "history"sv)
        return "Read and change your browsing history"_string;
    if (permission == "bookmarks"sv)
        return "Read and change your bookmarks"_string;
    if (permission == "tabs"sv)
        return "Read your browsing history"_string;
    if (permission == "sessions"sv)
        return "Read your recently closed tabs"_string;
    if (permission == "webNavigation"sv)
        return "Read your browsing history"_string;
    if (permission == "notifications"sv)
        return "Display notifications"_string;
    if (permission == "clipboardRead"sv)
        return "Read data you copy and paste"_string;
    if (permission == "clipboardWrite"sv)
        return "Modify data you copy and paste"_string;
    return MUST(String::formatted("Use \"{}\"", permission));
}

static String describe_origin(StringView origin)
{
    if (origin == "<all_urls>"sv || origin == "*://*/*"sv || origin == "http://*/*"sv || origin == "https://*/*"sv)
        return "Read and change all your data on all websites"_string;
    auto host = origin;
    if (auto separator = host.find("://"sv); separator.has_value())
        host = host.substring_view(*separator + 3);
    host = host.substring_view(0, host.find('/').value_or(host.length()));
    if (host.starts_with("*."sv))
        return MUST(String::formatted("Read and change your data on all {} sites", host.substring_view(2)));
    return MUST(String::formatted("Read and change your data on {}", host));
}

void ExtensionHost::permissions_call(LoadedExtension const& extension, StringView function, JsonArray const& arguments, Completion complete)
{
    auto wanted = !arguments.is_empty() && arguments.at(0).is_object() ? arguments.at(0).as_object() : JsonObject {};
    auto wanted_permissions = strings_in(wanted, "permissions"sv);
    auto wanted_origins = strings_in(wanted, "origins"sv);

    auto required = required_permissions_of(extension);
    auto optional = optional_permissions_of(extension);

    auto holds_permission = [&](StringView permission) { return extension_has_permission(extension, permission); };
    auto holds_origin = [&](StringView origin) {
        return any_of(required.origins, [&](auto const& held) { return match_pattern_covers(held, origin); })
            || any_of(extension.granted_origins, [&](auto const& held) { return match_pattern_covers(held, origin); });
    };

    if (function == "getAll"sv) {
        auto permissions = required.permissions;
        for (auto const& permission : extension.granted_permissions) {
            if (!permissions.contains_slow(permission))
                permissions.append(permission);
        }
        auto origins = required.origins;
        for (auto const& origin : extension.granted_origins) {
            if (!origins.contains_slow(origin))
                origins.append(origin);
        }
        return complete(result(permissions_as_json(permissions, origins)));
    }

    if (function == "contains"sv)
        return complete(result(all_of(wanted_permissions, holds_permission) && all_of(wanted_origins, holds_origin)));

    if (function == "request"sv) {
        // An extension may ask only for what its manifest names, as Chromium checks.
        for (auto const& permission : wanted_permissions) {
            if (!required.permissions.contains_slow(permission) && !optional.permissions.contains_slow(permission))
                return complete(error(MUST(String::formatted("Only permissions specified in the manifest may be requested. \"{}\" is not.", permission))));
        }
        for (auto const& origin : wanted_origins) {
            auto covers = [&](auto const& declared) { return match_pattern_covers(declared, origin); };
            if (!any_of(required.origins, covers) && !any_of(optional.origins, covers))
                return complete(error(MUST(String::formatted("Only permissions specified in the manifest may be requested. \"{}\" is not.", origin))));
        }

        Vector<String> new_permissions;
        for (auto const& permission : wanted_permissions) {
            if (!holds_permission(permission) && !new_permissions.contains_slow(permission))
                new_permissions.append(permission);
        }
        Vector<String> new_origins;
        for (auto const& origin : wanted_origins) {
            if (!holds_origin(origin) && !new_origins.contains_slow(origin))
                new_origins.append(origin);
        }
        if (new_permissions.is_empty() && new_origins.is_empty())
            return complete(result(true));

        Vector<String> descriptions;
        auto describe = [&](String description) {
            if (!descriptions.contains_slow(description))
                descriptions.append(move(description));
        };
        for (auto const& origin : new_origins)
            describe(describe_origin(origin));
        for (auto const& permission : new_permissions)
            describe(describe_permission(permission));

        // NB: The extension may be turned off or removed while the user decides, so it is looked up again.
        Application::the().ask_user_to_grant_extension_permissions(extension.name, descriptions)->when_resolved([this, extension_id = extension.id, new_permissions, new_origins, complete = move(complete)](bool granted) mutable {
            auto const* extension = Application::the().extension_with_id(extension_id);
            if (!extension) {
                complete(error("The extension was turned off"_string));
                return;
            }
            if (!granted) {
                complete(result(false));
                return;
            }
            ExtensionPermissions now_granted { extension->granted_permissions, extension->granted_origins };
            now_granted.permissions.extend(new_permissions);
            now_granted.origins.extend(new_origins);
            Application::the().set_extension_granted_permissions(extension_id, move(now_granted));

            complete(result(true));
            JsonArray event_arguments;
            event_arguments.must_append(permissions_as_json(new_permissions, new_origins));
            broadcast_event(extension_id, "permissions.onAdded"sv, event_arguments);
        });
        return;
    }

    if (function == "remove"sv) {
        // https://developer.chrome.com/docs/extensions/reference/api/permissions#method-remove
        if (any_of(wanted_permissions, [&](auto const& permission) { return required.permissions.contains_slow(permission); })
            || any_of(wanted_origins, [&](auto const& origin) { return required.origins.contains_slow(origin); }))
            return complete(error("You cannot remove required permissions."_string));

        ExtensionPermissions now_granted;
        Vector<String> removed_permissions;
        Vector<String> removed_origins;
        for (auto const& permission : extension.granted_permissions)
            (wanted_permissions.contains_slow(permission) ? removed_permissions : now_granted.permissions).append(permission);
        for (auto const& origin : extension.granted_origins)
            (wanted_origins.contains_slow(origin) ? removed_origins : now_granted.origins).append(origin);

        if (!removed_permissions.is_empty() || !removed_origins.is_empty()) {
            auto extension_id = extension.id;
            Application::the().set_extension_granted_permissions(extension_id, move(now_granted));
            JsonArray event_arguments;
            event_arguments.must_append(permissions_as_json(removed_permissions, removed_origins));
            broadcast_event(extension_id, "permissions.onRemoved"sv, event_arguments);
        }
        return complete(result(true));
    }

    complete(error(MUST(String::formatted("chrome.permissions.{} is not implemented yet", function))));
}

// https://developer.chrome.com/docs/extensions/develop/concepts/activeTab#what-activeTab-allows
void ExtensionHost::grant_active_tab(LoadedExtension const& extension, ViewImplementation& view)
{
    if (!extension_has_permission(extension, "activeTab"sv))
        return;
    // Like host permissions, activeTab reaches web pages only, not the browser's own pages or other extensions'.
    // FIXME: Chromium also grants file: URLs, if the user lets the extension at them.
    auto url = view.url();
    if (!url.scheme().is_one_of("http"sv, "https"sv))
        return;
    m_active_tab_grants.ensure(extension.id).set(view.view_id(), url.origin());
}

bool ExtensionHost::has_active_tab_access(String const& extension_id, u64 view_id, URL::URL const& url) const
{
    auto grants = m_active_tab_grants.find(extension_id);
    if (grants == m_active_tab_grants.end())
        return false;
    auto granted = grants->value.get(view_id);
    return granted.has_value() && granted->is_same_origin(url.origin());
}

}
