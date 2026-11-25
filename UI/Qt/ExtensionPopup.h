/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/String.h>
#include <AK/kmalloc.h>
#include <LibGfx/Size.h>
#include <LibURL/URL.h>
#include <UI/Qt/Popover.h>

namespace Ladybird {

class WebContentView;

// The popup of an extension's toolbar button, which shows a page of the extension over the tab it was opened for, and
// takes the size of that page as Chromium sizes one.
// https://developer.chrome.com/docs/extensions/develop/ui/add-popup
class ExtensionPopup final : public Popover {
public:
    AK_ALLOC_WITH_KMALLOC;

    ExtensionPopup(QWidget* parent, String extension_id, WebContentView const& tab_view, URL::URL const&);
    virtual ~ExtensionPopup() override;

    String const& extension_id() const { return m_extension_id; }
    u64 view_id() const;

    // The size the popup's page would fit in, in CSS pixels.
    void set_page_size(Gfx::IntSize);

    // The popup changed its size, so it may have to be moved to stay beside its button.
    Function<void()> on_resize;

private:
    String m_extension_id;
    WebContentView* m_view { nullptr };
};

}
