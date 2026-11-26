/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibWebView/Application.h>
#include <LibWebView/Extensions.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI/ExtensionsUI.h>

namespace WebView {

// The size at which the page shows icons, in CSS pixels, doubled for high-density displays.
static constexpr int icon_size = 48 * 2;

// The about:extensions pages that are open, which are told whenever extensions change.
static Vector<ExtensionsUI*>& open_pages()
{
    static auto* pages = new Vector<ExtensionsUI*>;
    return *pages;
}

ExtensionsUI::~ExtensionsUI()
{
    open_pages().remove_first_matching([this](auto* page) { return page == this; });
}

void ExtensionsUI::extensions_did_change()
{
    for (auto* page : open_pages()) {
        if (page->has_client())
            page->load_extensions();
    }
}

void ExtensionsUI::register_interfaces()
{
    open_pages().append(this);
    register_interface("loadExtensions"sv, [this](auto const&) {
        load_extensions();
    });
    register_interface("openOptionsPage"sv, [this](auto const& data) {
        open_options_page(data);
    });
    register_interface("setExtensionEnabled"sv, [this](auto const& data) {
        set_extension_enabled(data);
    });
    register_interface("removeExtension"sv, [this](auto const& data) {
        remove_extension(data);
    });
    register_interface("reloadExtension"sv, [this](auto const& data) {
        reload_extension(data);
    });
    register_interface("retryExtensionLoad"sv, [this](auto const& data) {
        retry_extension_load(data);
    });
    register_interface("loadUnpacked"sv, [this](auto const&) {
        install_extension(false);
    });
    register_interface("installPackage"sv, [this](auto const&) {
        install_extension(true);
    });
    register_interface("forgetExtensionFolder"sv, [this](auto const& data) {
        forget_extension_folder(data);
    });
}

static JsonArray strings_of(JsonObject const& object, StringView key)
{
    JsonArray strings;
    if (auto array = object.get_array(key); array.has_value()) {
        for (auto const& value : array->values()) {
            if (value.is_string())
                strings.must_append(value.as_string());
        }
    }
    return strings;
}

static bool is_match_pattern(StringView string)
{
    return string == "<all_urls>"sv || string.contains("://"sv);
}

static Optional<String> icon_data_url(LoadedExtension const& extension)
{
    auto path = icon_path(extension, icon_size);
    if (!path.has_value())
        return {};

    auto contents = read_extension_file(extension, *path);
    if (contents.is_error())
        return {};

    // Icons are PNG files, as the manifest's documentation asks of them, though some extensions use SVG.
    auto type = path->bytes_as_string_view().ends_with(".svg"sv, CaseSensitivity::CaseInsensitive) ? "image/svg+xml"sv : "image/png"sv;
    auto encoded = encode_base64(contents.value());
    if (encoded.is_error())
        return {};
    return MUST(String::formatted("data:{};base64,{}", type, encoded.value()));
}

static JsonObject serialize_extension(LoadedExtension const& extension, bool enabled)
{
    auto const& manifest = extension.manifest;

    JsonObject serialized;
    serialized.set("id"sv, extension.id);
    serialized.set("enabled"sv, enabled);
    serialized.set("name"sv, extension.name);
    serialized.set("version"sv, extension.version);
    serialized.set("description"sv, manifest.get_string("description"sv).value_or({}));
    serialized.set("directory"sv, String::from_utf8_with_replacement_character(extension.directory));
    serialized.set("fromCommandLine"sv, Application::the().is_extension_folder_from_command_line(extension.directory));
    serialized.set("packed"sv, Application::the().is_unpacked_extension_package(extension.directory));
    serialized.set("manifestVersion"sv, manifest.get_integer<i64>("manifest_version"sv).value_or(0));
    if (auto icon = icon_data_url(extension); icon.has_value())
        serialized.set("icon"sv, icon.release_value());

    // Manifest V2 lists the hosts it may access among its permissions; Manifest V3 has a key of their own for them.
    JsonArray permissions;
    JsonArray host_permissions = strings_of(manifest, "host_permissions"sv);
    strings_of(manifest, "permissions"sv).for_each([&](JsonValue const& permission) {
        if (is_match_pattern(permission.as_string()))
            host_permissions.must_append(permission);
        else
            permissions.must_append(permission);
    });
    serialized.set("permissions"sv, move(permissions));
    serialized.set("hostPermissions"sv, move(host_permissions));

    Vector<String> content_script_matches;
    extension.content_scripts.for_each([&](JsonValue const& content_script) {
        if (!content_script.is_object())
            return;
        strings_of(content_script.as_object(), "matches"sv).for_each([&](JsonValue const& match) {
            if (!content_script_matches.contains_slow(match.as_string()))
                content_script_matches.append(match.as_string());
        });
    });
    JsonArray serialized_content_script_matches;
    for (auto const& match : content_script_matches)
        serialized_content_script_matches.must_append(match);
    serialized.set("contentScriptMatches"sv, move(serialized_content_script_matches));

    if (background_page_path(extension).has_value()) {
        auto background = manifest.get_object("background"sv);
        bool is_service_worker = background.has_value() && background->has_string("service_worker"sv);
        serialized.set("background"sv, is_service_worker ? "Service worker"sv : "Background page"sv);
        serialized.set("backgroundRunning"sv, Application::the().extension_background_view(extension.id) != nullptr);
    }

    serialized.set("hasOptionsPage"sv, options_page_path(extension).has_value());
    return serialized;
}

void ExtensionsUI::load_extensions()
{
    auto const& application = Application::the();

    JsonArray extensions;
    for (auto const& extension : application.extensions())
        extensions.must_append(serialize_extension(extension, true));
    for (auto const& extension : application.disabled_extensions())
        extensions.must_append(serialize_extension(extension, false));

    JsonArray errors;
    for (auto const& [directory, error] : application.extension_load_errors()) {
        JsonObject serialized;
        serialized.set("directory"sv, String::from_utf8_with_replacement_character(directory));
        serialized.set("error"sv, error);
        errors.must_append(move(serialized));
    }

    JsonObject message;
    message.set("extensions"sv, move(extensions));
    message.set("errors"sv, move(errors));
    async_send_message("loadExtensions"sv, move(message));
}

void ExtensionsUI::open_options_page(JsonValue const& data)
{
    if (!data.is_string() || !has_client())
        return;

    // Only a click on the page opens a tab.
    auto* page = client().page(page_id());
    if (!page || !page->consume_transient_user_activation())
        return;

    Application::the().open_extension_options_page(data.as_string());
}

void ExtensionsUI::set_extension_enabled(JsonValue const& data)
{
    if (!data.is_object())
        return;
    auto id = data.as_object().get_string("id"sv);
    auto enabled = data.as_object().get_bool("enabled"sv);
    if (!id.has_value() || !enabled.has_value())
        return;

    if (auto result = Application::the().set_extension_enabled(*id, *enabled); result.is_error())
        report_failure(*enabled ? "turn on"sv : "turn off"sv, *id, result.error());
    load_extensions();
}

void ExtensionsUI::remove_extension(JsonValue const& data)
{
    if (!data.is_string())
        return;

    if (auto result = Application::the().remove_extension(data.as_string()); result.is_error())
        report_failure("remove"sv, data.as_string(), result.error());
    load_extensions();
}

void ExtensionsUI::reload_extension(JsonValue const& data)
{
    if (!data.is_string())
        return;

    if (auto result = Application::the().reload_extension(data.as_string()); result.is_error())
        report_failure("reload"sv, data.as_string(), result.error());
    load_extensions();
}

void ExtensionsUI::retry_extension_load(JsonValue const& data)
{
    if (!data.is_string())
        return;

    // A failure shows as the folder's new reason, in the list of extensions that could not be loaded.
    (void)Application::the().retry_extension_load(data.as_string().to_byte_string());
    load_extensions();
}

void ExtensionsUI::install_extension(bool package)
{
    if (!has_client())
        return;

    // Only a click on the page asks for a folder or file.
    auto* page = client().page(page_id());
    if (!page || !page->consume_transient_user_activation())
        return;

    Application::the().ask_user_for_extension_path(package)->when_resolved([weak_this = make_weak_ptr<ExtensionsUI>(), package](ByteString const& path) {
        auto result = package ? Application::the().install_extension_package(path) : Application::the().install_unpacked_extension(path);
        auto self = weak_this.strong_ref();
        if (!self)
            return;
        if (result.is_error()) {
            JsonObject message;
            message.set("directory"sv, String::from_utf8_with_replacement_character(path));
            message.set("error"sv, MUST(String::formatted("{}", result.error())));
            self->async_send_message("installFailed"sv, move(message));
        }
        self->load_extensions();
    });
}

void ExtensionsUI::forget_extension_folder(JsonValue const& data)
{
    if (!data.is_string())
        return;

    Application::the().forget_extension_folder(data.as_string());
    load_extensions();
}

void ExtensionsUI::report_failure(StringView action, String const& extension_id, Error const& error)
{
    JsonObject message;
    message.set("id"sv, extension_id);
    message.set("error"sv, MUST(String::formatted("Could not {} the extension: {}", action, error)));
    async_send_message("extensionActionFailed"sv, move(message));
}

}
