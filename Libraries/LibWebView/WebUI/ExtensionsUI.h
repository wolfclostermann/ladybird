/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/WebUI.h>

namespace WebView {

// about:extensions, which lists the extensions that were loaded and the ones that could not be.
class ExtensionsUI final : public WebUI {
    WEB_UI(ExtensionsUI);

private:
    virtual void register_interfaces() override;

    void load_extensions();
    void open_options_page(JsonValue const&);
};

}
