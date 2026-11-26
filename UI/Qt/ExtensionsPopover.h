/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <UI/Qt/Popover.h>

#include <QIcon>
#include <QPoint>

class QVBoxLayout;

namespace Ladybird {

// The extensions menu, which lists every extension with a toolbar button, whether or not the button is pinned to the
// toolbar, as Chromium's does: a row clicks the extension's button, the pin pins or unpins it, and the last button
// opens the button's menu.
class ExtensionsPopover final : public Popover {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ExtensionsPopover(QWidget* parent);

    struct Entry {
        String extension_id;
        QIcon icon;
        QString name;
        // What the button says of itself in the tab that is shown, if it is not the extension's name.
        QString title;
        bool enabled { true };
        bool pinned { false };
    };
    void set_entries(Vector<Entry>);
    void update_chrome_style(QPalette const&);

    Function<void(String const&)> on_activate;
    Function<void(String const&, bool)> on_set_pinned;
    Function<void(String const&, QPoint global_position)> on_show_menu;
    Function<void()> on_manage_extensions;

private:
    void rebuild_rows();

    Vector<Entry> m_entries;
    QWidget* m_rows { nullptr };
    QVBoxLayout* m_rows_layout { nullptr };
};

}
