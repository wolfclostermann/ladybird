/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/Optional.h>
#include <AK/kmalloc.h>
#include <AK/String.h>
#include <LibGfx/Color.h>
#include <LibURL/URL.h>
#include <LibWebView/ExtensionHost.h>
#include <LibWebView/Export.h>

namespace WebView {

struct LoadedExtension;

// The toolbar buttons of extensions, and what the action API sets of them, for all tabs or for one.
// https://developer.chrome.com/docs/extensions/reference/api/action
class WEBVIEW_API ExtensionActions {
    AK_MAKE_NONCOPYABLE(ExtensionActions);
    AK_MAKE_NONMOVABLE(ExtensionActions);

public:
    AK_ALLOC_WITH_KMALLOC;

    ExtensionActions() = default;

    // The size at which the UI draws a button's icon, in pixels, which is 16 CSS pixels at twice the density.
    static constexpr int icon_size = 32;

    // Carries out action.<function>, for a context of the extension at base_url, against which the paths it names
    // are resolved.
    ExtensionHost::CallResult call(LoadedExtension const&, URL::URL const& base_url, StringView function, JsonArray const& arguments);

    // The page the button of the extension opens in a tab, if it opens one rather than telling the extension of the
    // click. Empty when the button is disabled.
    struct Click {
        bool enabled { true };
        Optional<String> popup;
    };
    Click click(LoadedExtension const&, u64 view_id);

    void view_will_be_destroyed(u64 view_id);
    // Forgets what the action API set of an extension's button, as the extension was turned off or removed.
    void forget(String const& extension_id) { m_states.remove(extension_id); }

    // The buttons of the extensions that have one, as JSON for the UI: for each, what it shows in every tab, and what
    // it shows instead in particular tabs.
    String serialized(ReadonlySpan<LoadedExtension>);

    // Called whenever what a button shows changes, once for each change or run of changes.
    Function<void()> on_change;

private:
    struct Properties {
        Optional<String> title;
        Optional<ByteBuffer> icon_png;
        Optional<String> badge_text;
        Optional<Color> badge_background_color;
        Optional<Color> badge_text_color;
        Optional<String> popup;
        Optional<bool> enabled;
    };
    struct State {
        Properties defaults;
        HashMap<u64, Properties> tabs;
    };

    State& state_for(LoadedExtension const&);
    Properties& properties_for(State&, Optional<u64> tab_id);
    void did_change();

    HashMap<String, State> m_states;
    bool m_change_is_pending { false };
};

}
