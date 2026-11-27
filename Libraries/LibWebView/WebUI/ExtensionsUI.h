/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/WebUI.h>

namespace WebView {

// about:extensions, which lists the extensions that were loaded and the ones that could not be, and turns them on and
// off or removes them.
class ExtensionsUI final : public WebUI {
    WEB_UI(ExtensionsUI);

public:
    virtual ~ExtensionsUI() override;

    // Shows the extensions afresh on every open about:extensions page, as they were changed from elsewhere, such as
    // the menu of an extension's toolbar button.
    static void extensions_did_change();

private:
    virtual void register_interfaces() override;

    void load_extensions();
    void open_options_page(JsonValue const&);
    void set_extension_enabled(JsonValue const&);
    void remove_extension(JsonValue const&);
    void reload_extension(JsonValue const&);
    void retry_extension_load(JsonValue const&);
    // Asks the user for an unpacked extension's folder, or a packed extension's file, and installs it.
    void install_extension(bool package);
    void forget_extension_folder(JsonValue const&);
    // Takes back an optional permission or host permission the user granted an extension.
    void revoke_extension_permission(JsonValue const&);
    // Gives a command of an extension the shortcut the user pressed, or takes its shortcut away.
    void set_extension_shortcut(JsonValue const&);
    void report_failure(StringView action, String const& extension_id, Error const&);
};

}
