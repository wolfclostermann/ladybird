/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ExtensionPopup.h>
#include <UI/Qt/WebContentView.h>

#include <QFrame>
#include <QVBoxLayout>

namespace Ladybird {

// Chromium's bounds for a popup's size: https://developer.chrome.com/docs/extensions/reference/api/action#popup
static constexpr Gfx::IntSize minimum_popup_size { 25, 25 };
static constexpr Gfx::IntSize maximum_popup_size { 800, 600 };

// A popup shows at this size until its page has told it its own.
static constexpr Gfx::IntSize initial_popup_size { 320, 120 };

// The card's rounded corners stay clear of the page.
static constexpr int popup_padding = 4;

ExtensionPopup::ExtensionPopup(QWidget* parent, String extension_id, WebContentView const& tab_view, URL::URL const& url)
    : Popover(parent)
    , m_extension_id(AK::move(extension_id))
{
    setAttribute(Qt::WA_DeleteOnClose);

    auto* layout = new QVBoxLayout(&card());
    layout->setContentsMargins(popup_padding, popup_padding, popup_padding, popup_padding);
    layout->setSpacing(0);

    // NB: The popup's view belongs to the tab's, so the extension APIs don't count it as a tab of its own.
    auto initial_state = WebContentViewInitialState {
        .is_private = tab_view.is_private(),
        .maximum_frames_per_second = 60.0,
        .display_id = {},
        .screen_shows_high_dynamic_range = false,
        .owner_view_id = tab_view.view_id(),
        .is_tab = false,
    };
    m_view = new WebContentView(&card(), {}, initial_state);
    m_view->setFixedSize(initial_popup_size.width(), initial_popup_size.height());
    layout->addWidget(m_view);

    // window.close() from the popup's page closes the popup.
    m_view->on_close = [this] {
        close();
    };

    m_view->load(url);
}

ExtensionPopup::~ExtensionPopup() = default;

u64 ExtensionPopup::view_id() const
{
    return m_view->view_id();
}

void ExtensionPopup::set_page_size(Gfx::IntSize size)
{
    auto width = clamp(size.width(), minimum_popup_size.width(), maximum_popup_size.width());
    auto height = clamp(size.height(), minimum_popup_size.height(), maximum_popup_size.height());
    if (m_view->width() == width && m_view->height() == height)
        return;

    m_view->setFixedSize(width, height);
    card().adjustSize();
    adjustSize();
    if (on_resize)
        on_resize();
}

}
