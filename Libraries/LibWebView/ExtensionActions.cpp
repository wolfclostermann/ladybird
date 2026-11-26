/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonObject.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibURL/Parser.h>
#include <LibWebView/ExtensionActions.h>
#include <LibWebView/Extensions.h>

namespace WebView {

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

ExtensionActions::State& ExtensionActions::state_for(LoadedExtension const& extension)
{
    return m_states.ensure(extension.id, [&] {
        // What the manifest gives the button, which the API can change.
        State state;
        auto action = action_of(extension);
        state.defaults.title = extension.name;
        if (action.has_value()) {
            if (auto title = action->get_string("default_title"sv); title.has_value())
                state.defaults.title = *title;
            if (auto popup = action->get_string("default_popup"sv); popup.has_value())
                state.defaults.popup = *popup;
        }
        if (auto path = action_icon_path(extension, ExtensionActions::icon_size); path.has_value()) {
            if (auto icon = read_extension_file(extension, *path); !icon.is_error())
                state.defaults.icon_png = icon.release_value();
        }
        state.defaults.badge_text = String {};
        state.defaults.enabled = true;
        return state;
    });
}

ExtensionActions::Properties& ExtensionActions::properties_for(State& state, Optional<u64> tab_id)
{
    if (!tab_id.has_value())
        return state.defaults;
    return state.tabs.ensure(*tab_id);
}

void ExtensionActions::did_change()
{
    if (m_change_is_pending)
        return;
    m_change_is_pending = true;
    Core::deferred_invoke([this] {
        m_change_is_pending = false;
        if (on_change)
            on_change();
    });
}

static Optional<Color> color_from_json(JsonValue const& value)
{
    if (value.is_string())
        return Color::from_string(value.as_string());
    if (!value.is_array() || value.as_array().size() != 4)
        return {};
    Array<u8, 4> channels {};
    for (size_t i = 0; i < 4; ++i) {
        auto const& channel = value.as_array().at(i);
        if (!channel.is_number())
            return {};
        channels[i] = static_cast<u8>(clamp(channel.get_double_with_precision_loss().value_or(0), 0.0, 255.0));
    }
    return Color(channels[0], channels[1], channels[2], channels[3]);
}

static JsonArray color_to_json(Color color)
{
    JsonArray array;
    array.must_append(color.red());
    array.must_append(color.green());
    array.must_append(color.blue());
    array.must_append(color.alpha());
    return array;
}

// ImageData, as the renderer passes it on: {width, height, data: [r, g, b, a, ...]}.
static ErrorOr<ByteBuffer> png_from_image_data(JsonObject const& image_data)
{
    auto width = image_data.get_integer<int>("width"sv);
    auto height = image_data.get_integer<int>("height"sv);
    auto data = image_data.get_array("data"sv);
    if (!width.has_value() || !height.has_value() || !data.has_value() || *width <= 0 || *height <= 0 || *width > 512 || *height > 512)
        return Error::from_string_literal("Invalid image data");
    if (data->size() != static_cast<size_t>(*width) * *height * 4)
        return Error::from_string_literal("The image data does not match its size");

    auto bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Unpremultiplied, { *width, *height }));
    auto channel = [&](size_t index) -> u8 {
        auto const& value = data->at(index);
        return value.is_number() ? static_cast<u8>(clamp(value.get_double_with_precision_loss().value_or(0), 0.0, 255.0)) : 0;
    };
    for (int y = 0; y < *height; ++y) {
        for (int x = 0; x < *width; ++x) {
            auto index = (static_cast<size_t>(y) * *width + x) * 4;
            bitmap->set_pixel(x, y, Color(channel(index), channel(index + 1), channel(index + 2), channel(index + 3)));
        }
    }
    return Gfx::PNGWriter::encode(*bitmap);
}

static ErrorOr<ByteBuffer> icon_from_path(LoadedExtension const& extension, URL::URL const& base_url, StringView path)
{
    // Paths are relative to the page or worker that sets them, as Chromium resolves them.
    auto url = URL::Parser::basic_parse(path, base_url);
    if (!url.has_value() || url->scheme() != "chrome-extension"sv || url->serialized_host() != extension.id)
        return Error::from_string_literal("The icon is not a file of the extension");
    auto relative_path = URL::percent_decode(url->serialize_path());
    return read_extension_file(extension, relative_path);
}

// https://developer.chrome.com/docs/extensions/reference/api/action#method-setIcon
static ErrorOr<ByteBuffer> icon_from_details(LoadedExtension const& extension, URL::URL const& base_url, JsonObject const& details)
{
    if (auto image_data = details.get("imageData"sv); image_data.has_value() && image_data->is_object()) {
        auto const& object = image_data->as_object();
        if (object.has("width"sv))
            return png_from_image_data(object);

        // Images keyed by their sizes, of which the one that best fits the button is drawn.
        Optional<JsonObject const&> best;
        int best_size = 0;
        object.for_each_member([&](String const& key, JsonValue const& value) {
            auto size = key.to_number<int>();
            if (!size.has_value() || !value.is_object())
                return;
            bool is_better = !best.has_value()
                || (best_size < ExtensionActions::icon_size && *size > best_size)
                || (*size >= ExtensionActions::icon_size && *size < best_size);
            if (is_better) {
                best = value.as_object();
                best_size = *size;
            }
        });
        if (!best.has_value())
            return Error::from_string_literal("Invalid image data");
        return png_from_image_data(*best);
    }

    if (auto path = details.get("path"sv); path.has_value()) {
        if (path->is_string())
            return icon_from_path(extension, base_url, path->as_string());
        if (path->is_object()) {
            if (auto best = best_icon_path(path->as_object(), ExtensionActions::icon_size); best.has_value())
                return icon_from_path(extension, base_url, *best);
        }
    }
    return Error::from_string_literal("Either path or imageData must be given");
}

ExtensionHost::CallResult ExtensionActions::call(LoadedExtension const& extension, URL::URL const& base_url, StringView function, JsonArray const& arguments)
{
    auto argument = [&](size_t index) -> JsonValue {
        return index < arguments.size() ? arguments.at(index) : JsonValue {};
    };

    auto& state = state_for(extension);

    // Most functions take {tabId}, or details with a tabId in them; enable, disable and isEnabled take the ID itself.
    Optional<u64> tab_id;
    JsonObject details;
    if (function.is_one_of("enable"sv, "disable"sv, "isEnabled"sv)) {
        if (auto id = argument(0); id.is_integer<i64>() && id.as_integer<i64>() >= 0)
            tab_id = static_cast<u64>(id.as_integer<i64>());
    } else if (argument(0).is_object()) {
        details = argument(0).as_object();
        if (auto id = details.get_integer<i64>("tabId"sv); id.has_value() && *id >= 0)
            tab_id = static_cast<u64>(*id);
    }

    // What is set for a tab overrides what is set for all of them.
    auto effective = [&](auto member) {
        if (tab_id.has_value()) {
            if (auto tab = state.tabs.get(*tab_id); tab.has_value() && ((*tab).*member).has_value())
                return ((*tab).*member).value();
        }
        return (state.defaults.*member).value_or({});
    };

    // A null value set for a tab goes back to what all tabs show.
    auto set = [&](auto member, auto value) {
        auto& properties = properties_for(state, tab_id);
        if (value.has_value() || !tab_id.has_value())
            properties.*member = move(value);
        else
            properties.*member = {};
        did_change();
    };

    if (function == "setTitle"sv) {
        auto title = details.get_string("title"sv);
        set(&Properties::title, title.has_value() ? Optional<String> { *title } : (tab_id.has_value() ? Optional<String> {} : Optional<String> { extension.name }));
        return undefined_result();
    }
    if (function == "getTitle"sv)
        return result(effective(&Properties::title));

    if (function == "setIcon"sv) {
        auto icon = icon_from_details(extension, base_url, details);
        if (icon.is_error())
            return error(MUST(String::formatted("{}", icon.error())));
        set(&Properties::icon_png, Optional<ByteBuffer> { icon.release_value() });
        return undefined_result();
    }

    if (function == "setBadgeText"sv) {
        auto text = details.get_string("text"sv);
        set(&Properties::badge_text, text.has_value() ? Optional<String> { *text } : (tab_id.has_value() ? Optional<String> {} : Optional<String> { String {} }));
        return undefined_result();
    }
    if (function == "getBadgeText"sv)
        return result(effective(&Properties::badge_text));

    if (function == "setBadgeBackgroundColor"sv || function == "setBadgeTextColor"sv) {
        auto member = function == "setBadgeBackgroundColor"sv ? &Properties::badge_background_color : &Properties::badge_text_color;
        auto color_value = details.get("color"sv);
        if (!color_value.has_value() || color_value->is_null()) {
            set(member, Optional<Color> {});
            return undefined_result();
        }
        auto color = color_from_json(*color_value);
        if (!color.has_value())
            return error("Invalid color"sv);
        set(member, color);
        return undefined_result();
    }
    if (function == "getBadgeBackgroundColor"sv || function == "getBadgeTextColor"sv) {
        auto member = function == "getBadgeBackgroundColor"sv ? &Properties::badge_background_color : &Properties::badge_text_color;
        Optional<Color> color;
        if (tab_id.has_value()) {
            if (auto tab = state.tabs.get(*tab_id); tab.has_value())
                color = (*tab).*member;
        }
        if (!color.has_value())
            color = state.defaults.*member;
        // Chromium's default badge color.
        return result(color_to_json(color.value_or(function == "getBadgeBackgroundColor"sv ? Color(0x5f, 0x63, 0x68) : Color::White)));
    }

    // https://developer.chrome.com/docs/extensions/reference/api/action#method-setPopup
    if (function == "setPopup"sv) {
        auto popup = details.get_string("popup"sv);
        set(&Properties::popup, popup.has_value() ? Optional<String> { *popup } : (tab_id.has_value() ? Optional<String> {} : Optional<String> { String {} }));
        return undefined_result();
    }
    if (function == "getPopup"sv) {
        auto popup = effective(&Properties::popup);
        if (popup.is_empty())
            return result(String {});
        return result(MUST(String::formatted("chrome-extension://{}/{}", extension.id, popup.bytes_as_string_view().trim("/"sv, TrimMode::Left))));
    }

    if (function == "enable"sv || function == "disable"sv) {
        set(&Properties::enabled, Optional<bool> { function == "enable"sv });
        return undefined_result();
    }
    if (function == "isEnabled"sv)
        return result(effective(&Properties::enabled));

    // https://developer.chrome.com/docs/extensions/reference/api/action#method-getUserSettings
    if (function == "getUserSettings"sv) {
        JsonObject settings;
        settings.set("isOnToolbar"sv, action_of(extension).has_value());
        return result(settings);
    }

    return error(MUST(String::formatted("chrome.action.{} is not implemented yet", function)));
}

ExtensionActions::Click ExtensionActions::click(LoadedExtension const& extension, u64 view_id)
{
    auto& state = state_for(extension);
    auto tab = state.tabs.get(view_id);

    Click click;
    click.enabled = (tab.has_value() && tab->enabled.has_value()) ? *tab->enabled : state.defaults.enabled.value_or(true);
    auto popup = (tab.has_value() && tab->popup.has_value()) ? *tab->popup : state.defaults.popup.value_or({});
    if (!popup.is_empty())
        click.popup = move(popup);
    return click;
}

void ExtensionActions::view_will_be_destroyed(u64 view_id)
{
    bool changed = false;
    for (auto& [id, state] : m_states)
        changed |= state.tabs.remove(view_id);
    if (changed)
        did_change();
}

static JsonObject properties_to_json(auto const& properties)
{
    JsonObject object;
    if (properties.title.has_value())
        object.set("title"sv, *properties.title);
    if (properties.icon_png.has_value()) {
        if (auto encoded = encode_base64(properties.icon_png->bytes()); !encoded.is_error())
            object.set("icon"sv, encoded.release_value());
    }
    if (properties.badge_text.has_value())
        object.set("badgeText"sv, *properties.badge_text);
    if (properties.badge_background_color.has_value())
        object.set("badgeBackgroundColor"sv, color_to_json(*properties.badge_background_color));
    if (properties.badge_text_color.has_value())
        object.set("badgeTextColor"sv, color_to_json(*properties.badge_text_color));
    if (properties.popup.has_value())
        object.set("hasPopup"sv, !properties.popup->is_empty());
    if (properties.enabled.has_value())
        object.set("enabled"sv, *properties.enabled);
    return object;
}

String ExtensionActions::serialized(ReadonlySpan<LoadedExtension> extensions)
{
    JsonArray buttons;
    for (auto const& extension : extensions) {
        if (!action_of(extension).has_value())
            continue;
        auto& state = state_for(extension);

        JsonObject tabs;
        for (auto const& [tab_id, properties] : state.tabs)
            tabs.set(String::number(tab_id), properties_to_json(properties));

        JsonObject button;
        button.set("extensionId"sv, extension.id);
        button.set("name"sv, extension.name);
        button.set("hasOptionsPage"sv, options_page_path(extension).has_value());
        button.set("defaults"sv, properties_to_json(state.defaults));
        button.set("tabs"sv, move(tabs));
        buttons.must_append(move(button));
    }
    return buttons.serialized();
}

}
