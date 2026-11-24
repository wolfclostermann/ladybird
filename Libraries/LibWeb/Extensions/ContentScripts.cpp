/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/ScriptCompilation.h>
#include <LibJS/SourceCode.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/ContentScripts.h>
#include <LibWeb/Extensions/ExtensionContext.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Window.h>

namespace Web::Extensions {

static Vector<Extension>& extensions()
{
    static auto& extensions = *new Vector<Extension>;
    return extensions;
}

bool has_extensions()
{
    return !extensions().is_empty();
}

static Vector<String> strings_of(JsonObject const& object, StringView key)
{
    Vector<String> strings;
    if (auto array = object.get_array(key); array.has_value()) {
        for (auto const& value : array->values()) {
            if (value.is_string())
                strings.append(value.as_string());
        }
    }
    return strings;
}

static RunAt run_at_from_string(StringView run_at)
{
    if (run_at == "document_start"sv)
        return RunAt::DocumentStart;
    if (run_at == "document_end"sv)
        return RunAt::DocumentEnd;
    return RunAt::DocumentIdle;
}

ErrorOr<void> set_extensions_from_json(StringView json)
{
    auto parsed = TRY(JsonParser::parse(json));
    if (!parsed.is_array())
        return Error::from_string_literal("Extensions must be a JSON array");

    Vector<Extension> new_extensions;
    for (auto const& extension_value : parsed.as_array().values()) {
        if (!extension_value.is_object())
            return Error::from_string_literal("Each extension must be a JSON object");
        auto const& extension_object = extension_value.as_object();

        Extension extension;
        extension.id = extension_object.get_string("id"sv).value_or({});
        extension.manifest_json = extension_object.get_string("manifest"sv).value_or("{}"_string);

        if (auto content_scripts = extension_object.get_array("content_scripts"sv); content_scripts.has_value()) {
            for (auto const& content_script_value : content_scripts->values()) {
                if (!content_script_value.is_object())
                    continue;
                auto const& content_script_object = content_script_value.as_object();

                ContentScript content_script;
                content_script.matches = strings_of(content_script_object, "matches"sv);
                content_script.exclude_matches = strings_of(content_script_object, "exclude_matches"sv);
                content_script.run_at = run_at_from_string(content_script_object.get_string("run_at"sv).value_or({}));
                content_script.all_frames = content_script_object.get_bool("all_frames"sv).value_or(false);
                content_script.match_about_blank = content_script_object.get_bool("match_about_blank"sv).value_or(false);
                content_script.css = Utf16String::from_utf8(content_script_object.get_string("css"sv).value_or({}));

                if (auto js = content_script_object.get_array("js"sv); js.has_value()) {
                    for (auto const& file_value : js->values()) {
                        if (!file_value.is_object())
                            continue;
                        auto const& file_object = file_value.as_object();
                        content_script.js.append({
                            .path = file_object.get_string("path"sv).value_or({}),
                            .source = Utf16String::from_utf8(file_object.get_string("source"sv).value_or({})),
                        });
                    }
                }

                extension.content_scripts.append(move(content_script));
            }
        }

        new_extensions.append(move(extension));
    }

    extensions() = move(new_extensions);
    return {};
}

// The URL that the content scripts of document are matched against: that of the document itself, or for an
// about:blank or about:srcdoc document whose content scripts match about:blank documents, that of its parent.
static Optional<URL::URL> url_for_matching(DOM::Document const& document, ContentScript const& content_script)
{
    auto const& url = document.url();
    if (url.scheme() != "about"sv)
        return url;

    if (!content_script.match_about_blank)
        return {};

    auto navigable = document.navigable();
    if (!navigable)
        return {};
    auto parent = navigable->parent();
    if (!parent)
        return {};
    auto* local_parent = as_if<HTML::LocalNavigable>(parent.ptr());
    if (!local_parent || !local_parent->active_document())
        return {};
    return url_for_matching(*local_parent->active_document(), content_script);
}

static bool content_script_matches(DOM::Document const& document, ContentScript const& content_script)
{
    auto navigable = document.navigable();
    if (!navigable)
        return false;
    if (!content_script.all_frames && !navigable->is_top_level_traversable())
        return false;

    auto url = url_for_matching(document, content_script);
    if (!url.has_value())
        return false;

    auto matches = [&](String const& pattern) { return match_pattern_matches(pattern, *url); };
    return any_of(content_script.matches, matches) && !any_of(content_script.exclude_matches, matches);
}

static void run_script_in_realm(JS::Realm& realm, HTML::EnvironmentSettingsObject& settings, String const& filename, Utf16String source)
{
    auto display_filename = Utf16String::from_utf8(filename);
    auto source_code = JS::SourceCode::create(display_filename, source);
    auto parsed = JS::ParsedProgram::parse(move(source), JS::ProgramType::Script, 1);
    auto script = JS::create_script(move(parsed), move(source_code), realm, filename, nullptr);
    if (script.is_error()) {
        dbgln("Extensions: Failed to parse content script {}: {}", filename, script.error().first().to_utf16_string());
        return;
    }

    HTML::prepare_to_run_script(settings);
    auto result = realm.vm().run(*script.value());
    if (result.is_error())
        HTML::report_an_exception(realm.global_object(), result.error_value());
    HTML::clean_up_after_running_script(settings);
}

// Creates the isolated world of extension in document: a realm whose global object is a wrapper of document's Window
// in a wrapper world of its own.
static GC::Ref<JS::Realm> create_isolated_world(DOM::Document& document)
{
    auto& window = *document.window();
    auto& window_realm = HTML::relevant_realm(window);
    auto& vm = Bindings::main_thread_vm();

    GC::Ptr<JS::HostObject> global_object;
    auto execution_context = Bindings::create_a_new_javascript_realm(
        vm,
        HTML::CrossOriginIsolationMode::None,
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            global_object = Bindings::create_global_object_wrapper(realm, GC::Ref<HTML::Window> { window });
            return global_object.as_nonnull();
        },
        [&](JS::Realm&) -> GC::Ref<JS::Object> {
            return global_object.as_nonnull();
        });

    auto& realm = *execution_context->realm();
    auto intrinsics = realm.create<Bindings::Intrinsics>(realm);
    auto wrapper_world = vm.heap().allocate<Bindings::WrapperWorld>(Bindings::WrapperWorld::Type::Extension);
    realm.set_host_defined(vm.heap().allocate<Bindings::HostDefined>(intrinsics, wrapper_world, window_realm));
    Bindings::cache_global_object_wrapper(realm);

    // The members of Window are defined on its wrapper once the realm has its intrinsics, as for the realm of the page.
    MUST(Bindings::initialize_window_web_interfaces(window, realm));
    return realm;
}

static JS::Realm& isolated_world_for(DOM::Document& document, Extension const& extension)
{
    if (auto context = document.extension_context(extension.id))
        return context->realm();
    auto realm = create_isolated_world(document);
    document.set_extension_context(extension.id, ExtensionContext::create(document, realm, extension, ExtensionContext::Type::ContentScript));
    return realm;
}

static Extension const* extension_with_id(StringView id)
{
    for (auto const& extension : extensions()) {
        if (extension.id == id)
            return &extension;
    }
    return nullptr;
}

// The pages of an extension, such as its background page, get the extension APIs in their own realm.
static void set_up_extension_page(DOM::Document& document)
{
    auto const& url = document.url();
    if (url.scheme() != "chrome-extension"sv)
        return;
    auto const* extension = extension_with_id(url.serialized_host());
    if (!extension || document.extension_context(extension->id))
        return;

    auto& realm = HTML::relevant_realm(*document.window());
    document.set_extension_context(extension->id, ExtensionContext::create(document, realm, *extension, ExtensionContext::Type::ExtensionPage));
}

static void run_content_scripts_at(DOM::Document& document, RunAt run_at)
{
    for (auto const& extension : extensions()) {
        for (auto const& content_script : extension.content_scripts) {
            if (content_script.run_at != run_at || content_script.js.is_empty())
                continue;
            if (!content_script_matches(document, content_script))
                continue;

            // The document may have gone away while an earlier script ran.
            if (!document.window() || !document.navigable())
                return;

            auto& realm = isolated_world_for(document, extension);
            for (auto const& file : content_script.js) {
                auto filename = MUST(String::formatted("chrome-extension://{}/{}", extension.id, file.path));
                run_script_in_realm(realm, document.relevant_settings_object(), filename, file.source);
            }
        }
    }
}

void run_content_scripts(DOM::Document& document, RunAt run_at)
{
    if (!has_extensions() || !document.window())
        return;

    // Each point runs once, after the points before it, even if the document reached a later point without passing
    // through the hook of an earlier one.
    for (auto point : { RunAt::DocumentStart, RunAt::DocumentEnd, RunAt::DocumentIdle }) {
        if (to_underlying(point) > to_underlying(run_at))
            break;
        if (document.extension_content_scripts_have_run(point))
            continue;
        document.set_extension_content_scripts_have_run(point);

        // The CSS of the content scripts that match the document applies from the start, once it is known which do.
        if (point == RunAt::DocumentStart) {
            document.invalidate_extension_style_sheet();
            set_up_extension_page(document);
        }

        run_content_scripts_at(document, point);
    }
}

Utf16String style_sheet_for_document(DOM::Document const& document)
{
    if (!has_extensions())
        return {};

    Utf16StringBuilder builder;
    for (auto const& extension : extensions()) {
        for (auto const& content_script : extension.content_scripts) {
            if (content_script.css.is_empty() || !content_script_matches(document, content_script))
                continue;
            if (!builder.is_empty())
                builder.append_ascii('\n');
            builder.append(content_script.css.utf16_view());
        }
    }
    return builder.to_string();
}

}
