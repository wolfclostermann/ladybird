/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/JsonValue.h>
#include <AK/StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/ScriptCompilation.h>
#include <LibJS/SourceCode.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/ContentScripts.h>
#include <LibWeb/Extensions/ExtensionContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWebCommon/Extensions/FrameId.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Extensions {

GC_DEFINE_ALLOCATOR(ExtensionContext);

// The chrome object of a realm of an extension. The script evaluates to a function that takes the native function that
// calls an extension API, sets up the chrome object, and returns the function that dispatches an event to the realm.
//
// Every function of the APIs passes its arguments, as JSON, to the UI process, which carries the call out. Results and
// events come back as JSON too, as Chromium's runtime messaging serializes messages with JSON.
static constexpr StringView runtime_source = R"~~~(
(nativeCall) => {
    "use strict";

    const extensionId = __EXTENSION_ID__;
    const manifest = __MANIFEST__;
    const contextType = __CONTEXT_TYPE__;

    const decode = (json) => (json === undefined || json === "" ? undefined : JSON.parse(json));

    let runtime;

    function invoke(api, args) {
        let callback;
        if (args.length > 0 && typeof args[args.length - 1] === "function")
            callback = args.pop();
        while (args.length > 0 && args[args.length - 1] === undefined)
            args.pop();

        const promise = nativeCall(api, JSON.stringify(args)).then(decode);
        if (!callback)
            return promise;

        promise.then(
            (result) => callback(result),
            (error) => {
                runtime.lastError = { message: error.message };
                try {
                    callback();
                } finally {
                    runtime.lastError = undefined;
                }
            });
        return undefined;
    }

    class ExtensionEvent {
        #listeners = [];
        addListener(listener) {
            if (!this.#listeners.includes(listener))
                this.#listeners.push(listener);
        }
        removeListener(listener) {
            const index = this.#listeners.indexOf(listener);
            if (index !== -1)
                this.#listeners.splice(index, 1);
        }
        hasListener(listener) { return this.#listeners.includes(listener); }
        hasListeners() { return this.#listeners.length > 0; }
        get listeners() { return [...this.#listeners]; }
    }

    const events = new Map();
    const event = (name) => {
        const extensionEvent = new ExtensionEvent();
        events.set(name, extensionEvent);
        return extensionEvent;
    };

    const namespace = (name, functions = [], eventNames = [], extra = {}) => {
        const object = { ...extra };
        for (const functionName of functions)
            object[functionName] = (...args) => invoke(`${name}.${functionName}`, args);
        for (const eventName of eventNames)
            object[eventName] = event(`${name}.${eventName}`);
        return object;
    };

    const storageArea = (area) => namespace(`storage.${area}`,
        ["get", "set", "remove", "clear", "getBytesInUse", "setAccessLevel", "getKeys"], ["onChanged"]);

    runtime = namespace("runtime",
        ["getPlatformInfo", "openOptionsPage", "reload", "setUninstallURL", "getContexts"],
        ["onMessage", "onConnect", "onInstalled", "onStartup", "onSuspend", "onUpdateAvailable", "onMessageExternal"],
        {
            id: extensionId,
            lastError: undefined,
            getManifest: () => structuredClone(manifest),
            getURL: (path) => `chrome-extension://${extensionId}/${String(path).replace(/^\/+/, "")}`,
            sendMessage(...args) {
                // The ID of another extension may come first.
                if (args.length >= 2 && (typeof args[0] === "string" || args[0] === null) && typeof args[1] !== "function")
                    args.shift();
                return invoke("runtime.sendMessage", args);
            },
            connect() { throw new Error("chrome.runtime.connect is not implemented yet"); },
        });

    const chrome = {
        runtime,
        storage: {
            local: storageArea("local"),
            sync: storageArea("sync"),
            session: storageArea("session"),
            managed: storageArea("managed"),
            onChanged: event("storage.onChanged"),
        },
        i18n: {
            getMessage: () => "",
            getUILanguage: () => navigator.language,
            getAcceptLanguages: (callback) => {
                const languages = [...navigator.languages];
                return callback ? callback(languages) : Promise.resolve(languages);
            },
        },
        extension: {
            inIncognitoContext: false,
            getURL: (path) => runtime.getURL(path),
        },
    };

    if (contextType === "extension_page") {
        Object.assign(chrome, {
            tabs: namespace("tabs",
                ["query", "get", "getCurrent", "create", "update", "remove", "reload", "duplicate", "move", "sendMessage",
                    "getZoom", "setZoom", "goBack", "goForward", "discard", "highlight", "captureVisibleTab", "detectLanguage"],
                ["onActivated", "onCreated", "onRemoved", "onUpdated", "onReplaced", "onMoved", "onAttached",
                    "onDetached", "onHighlighted", "onZoomChange"],
                { TAB_ID_NONE: -1 }),
            windows: namespace("windows",
                ["get", "getAll", "getCurrent", "getLastFocused", "create", "update", "remove"],
                ["onCreated", "onRemoved", "onFocusChanged", "onBoundsChanged"],
                { WINDOW_ID_NONE: -1, WINDOW_ID_CURRENT: -2 }),
            webNavigation: namespace("webNavigation",
                ["getFrame", "getAllFrames"],
                ["onBeforeNavigate", "onCommitted", "onDOMContentLoaded", "onCompleted", "onErrorOccurred",
                    "onCreatedNavigationTarget", "onReferenceFragmentUpdated", "onTabReplaced", "onHistoryStateUpdated"]),
            scripting: namespace("scripting", ["insertCSS", "removeCSS", "registerContentScripts",
                "unregisterContentScripts", "getRegisteredContentScripts"], [], {
                // A function cannot be sent as JSON, so its source is sent, to run where it is injected.
                executeScript(injection, callback) {
                    const details = { ...injection };
                    const func = details.func ?? details.function;
                    delete details.function;
                    if (typeof func === "function")
                        details.func = String(func);
                    return invoke("scripting.executeScript", callback ? [details, callback] : [details]);
                },
            }),
            history: namespace("history", ["search", "getVisits", "addUrl", "deleteUrl", "deleteRange", "deleteAll"],
                ["onVisited", "onVisitRemoved"]),
            bookmarks: namespace("bookmarks", ["get", "getChildren", "getRecent", "getTree", "getSubTree", "search",
                "create", "move", "update", "remove", "removeTree"], ["onCreated", "onRemoved", "onChanged", "onMoved",
                "onChildrenReordered", "onImportBegan", "onImportEnded"]),
            sessions: namespace("sessions", ["getRecentlyClosed", "getDevices", "restore"], ["onChanged"],
                { MAX_SESSION_RESULTS: 25 }),
            search: namespace("search", ["query"]),
            notifications: namespace("notifications", ["create", "update", "clear", "getAll", "getPermissionLevel"],
                ["onClosed", "onClicked", "onButtonClicked", "onPermissionLevelChanged", "onShowSettings"]),
            action: namespace("action", ["setIcon", "setTitle", "getTitle", "setBadgeText", "getBadgeText",
                "setBadgeBackgroundColor", "setPopup", "getPopup", "enable", "disable", "openPopup"], ["onClicked"]),
            commands: namespace("commands", ["getAll"], ["onCommand"]),
            permissions: namespace("permissions", ["contains", "getAll", "request", "remove"], ["onAdded", "onRemoved"]),
            contextMenus: namespace("contextMenus", ["create", "update", "remove", "removeAll"], ["onClicked"]),
            alarms: namespace("alarms", ["create", "get", "getAll", "clear", "clearAll"], ["onAlarm"]),
            offscreen: namespace("offscreen", ["createDocument", "closeDocument", "hasDocument"]),
        });
        chrome.browserAction = chrome.action;
    }

    globalThis.chrome = chrome;

    // Runs the listeners of event with the arguments that arguments_json holds, and calls respond with the JSON of a
    // response one of them gave, or with nothing if none did.
    return (name, argumentsJSON, respond) => {
        const extensionEvent = events.get(name);
        const args = decode(argumentsJSON) ?? [];
        const listeners = extensionEvent ? extensionEvent.listeners : [];

        if (name !== "runtime.onMessage") {
            for (const listener of listeners) {
                try {
                    listener(...args);
                } catch (error) {
                    reportError(error);
                }
            }
            respond();
            return;
        }

        // https://developer.chrome.com/docs/extensions/develop/concepts/messaging
        // A listener responds by calling sendResponse, or later if it returns true. Like Firefox, a promise that a
        // listener returns responds with its value too.
        let responded = false;
        let respondsLater = false;
        const sendResponse = (response) => {
            if (responded)
                return;
            responded = true;
            respond(JSON.stringify(response ?? null));
        };

        const [message, sender] = args;
        for (const listener of listeners) {
            let result;
            try {
                result = listener(message, sender, sendResponse);
            } catch (error) {
                reportError(error);
                continue;
            }
            if (result === true) {
                respondsLater = true;
            } else if (result && typeof result.then === "function") {
                respondsLater = true;
                result.then(sendResponse, (error) => {
                    reportError(error);
                    sendResponse(undefined);
                });
            }
        }

        if (!respondsLater && !responded) {
            responded = true;
            respond();
        }
    };
}
)~~~"sv;

static Utf16String runtime_source_for(Extension const& extension, ExtensionContext::Type type)
{
    ByteString source = runtime_source;
    source = source.replace("__EXTENSION_ID__"sv, JsonValue { extension.id }.serialized(), ReplaceMode::FirstOnly);
    source = source.replace("__MANIFEST__"sv, extension.manifest_json, ReplaceMode::FirstOnly);
    source = source.replace("__CONTEXT_TYPE__"sv, type == ExtensionContext::Type::ContentScript ? "\"content_script\""sv : "\"extension_page\""sv, ReplaceMode::FirstOnly);
    return Utf16String::from_utf8(source.view());
}

struct SharedResponder : RefCounted<SharedResponder> {
    // NB: The responses go to the UI process, and nothing that responds holds a cell.
    GC_HOLDS_NO_CELL_EDGES;

    explicit SharedResponder(Function<void(Optional<String>)> function)
        : function(move(function))
    {
    }

    Function<void(Optional<String>)> function;
};

struct PendingCall {
    GC::Root<WebIDL::Promise> promise;
    GC::Root<JS::Realm> realm;
};

static HashMap<u64, PendingCall>& pending_calls()
{
    static auto& calls = *new HashMap<u64, PendingCall>;
    return calls;
}

u64 frame_id_of(DOM::Document const& document)
{
    auto navigable = document.navigable();
    if (!navigable)
        return 0;
    return frame_id_for_navigable(navigable->id(), navigable->is_top_level_traversable());
}

static String context_json_for(DOM::Document const& document, ExtensionContext::Type type)
{
    JsonObject context;
    context.set("contextType"sv, type == ExtensionContext::Type::ContentScript ? "content_script"sv : "extension_page"sv);
    context.set("frameId"sv, frame_id_of(document));
    context.set("url"sv, document.url().serialize());
    if (auto navigable = document.navigable(); navigable && navigable->parent())
        context.set("parentFrameId"sv, frame_id_for_navigable(navigable->parent()->id(), navigable->parent()->is_top_level_traversable()));
    return context.serialized();
}

GC::Ref<ExtensionContext> ExtensionContext::create(DOM::Document& document, JS::Realm& realm, Extension const& extension, Type type)
{
    auto& vm = realm.vm();
    auto context = vm.heap().allocate<ExtensionContext>(extension.id, type, realm);

    auto& settings = document.relevant_settings_object();
    auto source = runtime_source_for(extension, type);
    auto filename = MUST(String::formatted("chrome-extension://{}/_runtime.js", extension.id));
    auto source_code = JS::SourceCode::create(Utf16String::from_utf8(filename), source);
    auto parsed = JS::ParsedProgram::parse(move(source), JS::ProgramType::Script, 1);
    auto script = JS::create_script(move(parsed), move(source_code), realm, filename, nullptr);
    VERIFY(!script.is_error());

    // The native function sends a call to the UI process, and returns a promise that its result settles.
    auto native_call = JS::NativeFunction::create(
        realm,
        GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Weak<DOM::Document> { document }, extension.id, type), [](auto& weak_document, auto& extension_id, auto& type, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto api = TRY(vm.argument(0).to_utf16_string(vm)).to_well_formed_utf8();
            auto arguments_json = TRY(vm.argument(1).to_utf16_string(vm)).to_well_formed_utf8();
            auto& realm = *vm.current_realm();
            auto promise = WebIDL::create_promise(realm);

            auto document = weak_document.ptr();
            if (!document || !document->navigable()) {
                WebIDL::reject_promise(realm, promise, JS::Error::create(realm, "The document of this context has gone away"_utf16));
                return promise->promise();
            }

            static u64 s_next_call_id = 0;
            auto call_id = ++s_next_call_id;
            pending_calls().set(call_id, { GC::make_root(promise), GC::make_root(realm) });
            document->page().client().page_did_call_extension_api(extension_id, context_json_for(*document, type), api, arguments_json, call_id);
            return promise->promise();
        }),
        2,
        "call"_utf16_fly_string,
        &realm);

    HTML::prepare_to_run_script(settings);
    auto result = vm.run(*script.value());
    if (!result.is_error() && result.value().is_function()) {
        auto dispatcher = JS::call(vm, result.value(), JS::js_undefined(), native_call);
        if (dispatcher.is_error())
            HTML::report_an_exception(realm.global_object(), dispatcher.error_value());
        else if (auto dispatcher_value = dispatcher.release_value(); dispatcher_value.is_function())
            context->m_dispatcher = &dispatcher_value.as_function();
    } else if (result.is_error()) {
        HTML::report_an_exception(realm.global_object(), result.error_value());
    }
    HTML::clean_up_after_running_script(settings);

    return context;
}

ExtensionContext::ExtensionContext(String extension_id, Type type, JS::Realm& realm)
    : m_extension_id(move(extension_id))
    , m_type(type)
    , m_realm(realm)
{
}

void ExtensionContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_dispatcher);
}

void ExtensionContext::dispatch_event(String const& event, String const& arguments_json, Function<void(Optional<String>)> on_complete)
{
    if (!m_dispatcher) {
        on_complete({});
        return;
    }

    auto& realm = *m_realm;
    auto& vm = realm.vm();

    // The listeners may respond once, at once or later.
    auto shared_on_complete = make_ref_counted<SharedResponder>(move(on_complete));
    auto respond = JS::NativeFunction::create(
        realm,
        GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(shared_on_complete), [](auto& shared_on_complete, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (!shared_on_complete->function)
                return JS::js_undefined();
            Optional<String> response;
            if (!vm.argument(0).is_undefined())
                response = TRY(vm.argument(0).to_utf16_string(vm)).to_well_formed_utf8();
            auto function = move(shared_on_complete->function);
            function(move(response));
            return JS::js_undefined();
        }),
        1,
        "respond"_utf16_fly_string,
        &realm);

    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    auto result = JS::call(vm, *m_dispatcher, JS::js_undefined(), JS::PrimitiveString::create(vm, Utf16String::from_utf8(event)), JS::PrimitiveString::create(vm, Utf16String::from_utf8(arguments_json)), respond);
    if (result.is_error()) {
        HTML::report_an_exception(realm.global_object(), result.error_value());
        if (shared_on_complete->function) {
            auto function = move(shared_on_complete->function);
            function({});
        }
    }
}

void complete_api_call(u64 call_id, bool is_error, String const& payload)
{
    auto call = pending_calls().take(call_id);
    if (!call.has_value())
        return;

    auto& realm = *call->realm;
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation, realm.global_object(), GC::create_function<void()>(realm.heap(), GC::captures(call->realm, call->promise, is_error, payload), [](auto& realm, auto& promise, auto& is_error, auto& payload) {
        HTML::TemporaryExecutionContext execution_context { *realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        if (is_error)
            WebIDL::reject_promise(*realm, *promise, JS::Error::create(*realm, Utf16String::from_utf8(payload)));
        else
            WebIDL::resolve_promise(*realm, *promise, JS::PrimitiveString::create(realm->vm(), Utf16String::from_utf8(payload)));
    }));
}

void dispatch_event(Page& page, String const& extension_id, String const& target_json, String const& event, String const& arguments_json, Function<void(Optional<String>)> on_complete)
{
    Optional<String> context_type;
    Optional<u64> frame_id;
    if (auto target = JsonParser::parse(target_json); !target.is_error() && target.value().is_object()) {
        auto const& object = target.value().as_object();
        if (auto type = object.get_string("contextType"sv); type.has_value())
            context_type = *type;
        frame_id = object.get_integer<u64>("frameId"sv);
    }

    Vector<GC::Root<ExtensionContext>> contexts;
    for (auto const& navigable : page.hosted_navigables()) {
        auto document = navigable->active_document();
        if (!document)
            continue;
        auto context = document->extension_context(extension_id);
        if (!context)
            continue;
        if (context_type.has_value() && *context_type != (context->type() == ExtensionContext::Type::ContentScript ? "content_script"sv : "extension_page"sv))
            continue;
        if (frame_id.has_value() && *frame_id != frame_id_of(*document))
            continue;
        contexts.append(GC::make_root(*context));
    }

    if (contexts.is_empty()) {
        on_complete({});
        return;
    }

    // The first response wins. Without one, the dispatch completes once every context has run its listeners.
    struct State : RefCounted<State> {
        GC_HOLDS_NO_CELL_EDGES;
        size_t remaining { 0 };
        Function<void(Optional<String>)> on_complete;
    };
    auto state = make_ref_counted<State>();
    state->remaining = contexts.size();
    state->on_complete = move(on_complete);

    for (auto& context : contexts) {
        HTML::queue_global_task(HTML::Task::Source::DOMManipulation, context->realm().global_object(), GC::create_function<void()>(context->heap(), GC::captures(context, state, event, arguments_json), [](auto& context, auto& state, auto& event, auto& arguments_json) {
            context->dispatch_event(event, arguments_json, [state](Optional<String> response) {
                --state->remaining;
                if (!state->on_complete)
                    return;
                if (response.has_value() || state->remaining == 0) {
                    auto on_complete = move(state->on_complete);
                    on_complete(move(response));
                }
            });
        }));
    }
}

}
