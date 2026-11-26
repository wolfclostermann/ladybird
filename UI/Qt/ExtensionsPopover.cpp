/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/ExtensionsPopover.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Theme.h>

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace Ladybird {

static constexpr int POPOVER_WIDTH = 300;
static constexpr int ROW_HEIGHT = 32;
static constexpr int EXTENSION_ICON_SIZE = 16;
static constexpr int ROW_BUTTON_SIZE = 28;
static constexpr int ROW_BUTTON_ICON_SIZE = 16;

static QPushButton* create_row(QWidget& parent, QString const& accessible_name)
{
    auto* row = new QPushButton(&parent);
    row->setObjectName("LadybirdPopoverRow");
    row->setAccessibleName(accessible_name);
    row->setFixedHeight(ROW_HEIGHT);
    row->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 0, 10, 0);
    layout->setSpacing(10);
    return row;
}

static QLabel* create_row_label(QString const& text, QWidget& row)
{
    auto* label = new QLabel(text, &row);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    return label;
}

static QToolButton* create_row_button(QWidget& parent, QString const& tooltip)
{
    auto* button = new QToolButton(&parent);
    button->setObjectName("LadybirdExtensionsPopoverButton");
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(ROW_BUTTON_SIZE, ROW_BUTTON_SIZE);
    button->setIconSize({ ROW_BUTTON_ICON_SIZE, ROW_BUTTON_ICON_SIZE });
    button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    return button;
}

ExtensionsPopover::ExtensionsPopover(QWidget* parent)
    : Popover(parent)
{
    setAccessibleName("Extensions");
    card().setObjectName("LadybirdExtensionsPopover");
    card().setFixedWidth(POPOVER_WIDTH);

    auto* layout = new QVBoxLayout(&card());
    layout->setContentsMargins(6, 10, 6, 6);
    layout->setSpacing(6);

    auto* title = new QLabel("Extensions", &card());
    title->setObjectName("LadybirdExtensionsPopoverTitle");
    title->setContentsMargins(10, 0, 10, 0);
    layout->addWidget(title);

    m_rows = new QWidget(&card());
    m_rows_layout = new QVBoxLayout(m_rows);
    m_rows_layout->setContentsMargins(0, 0, 0, 0);
    m_rows_layout->setSpacing(0);
    layout->addWidget(m_rows);

    auto* separator = new QFrame(&card());
    separator->setObjectName("LadybirdPopoverSeparator");
    separator->setFixedHeight(1);
    auto* separator_row = new QHBoxLayout;
    separator_row->setContentsMargins(10, 0, 10, 0);
    separator_row->addWidget(separator);
    layout->addLayout(separator_row);

    auto* manage_row = create_row(card(), "Manage Extensions");
    manage_row->layout()->addWidget(create_row_label("Manage Extensions", *manage_row));
    QObject::connect(manage_row, &QPushButton::clicked, this, [this] {
        close();
        if (on_manage_extensions)
            on_manage_extensions();
    });
    layout->addWidget(manage_row);
}

void ExtensionsPopover::set_entries(Vector<Entry> entries)
{
    m_entries = AK::move(entries);
    rebuild_rows();
}

void ExtensionsPopover::rebuild_rows()
{
    while (auto* item = m_rows_layout->takeAt(0)) {
        if (auto* widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    auto const& panel = Theme::panel();
    for (auto const& entry : m_entries) {
        auto* line = new QWidget(m_rows);
        auto* line_layout = new QHBoxLayout(line);
        line_layout->setContentsMargins(0, 0, 0, 0);
        line_layout->setSpacing(2);

        // The row stands for the extension's button, so it says what the button says, and clicks it.
        auto label = entry.title.isEmpty() || entry.title == entry.name ? entry.name : QString("%1 (%2)").arg(entry.name, entry.title);
        auto* row = create_row(*line, label);
        row->setToolTip(entry.title.isEmpty() ? entry.name : entry.title);
        row->setEnabled(entry.enabled);
        auto* icon = new QLabel(row);
        icon->setFixedSize(EXTENSION_ICON_SIZE, EXTENSION_ICON_SIZE);
        icon->setAttribute(Qt::WA_TransparentForMouseEvents);
        icon->setPixmap(entry.icon.pixmap(QSize(EXTENSION_ICON_SIZE, EXTENSION_ICON_SIZE), devicePixelRatioF(), entry.enabled ? QIcon::Normal : QIcon::Disabled));
        row->layout()->addWidget(icon);
        row->layout()->addWidget(create_row_label(entry.name, *row));
        QObject::connect(row, &QPushButton::clicked, this, [this, extension_id = entry.extension_id] {
            close();
            if (on_activate)
                on_activate(extension_id);
        });
        line_layout->addWidget(row, 1);

        auto* pin = create_row_button(*line, entry.pinned ? QString("Unpin %1").arg(entry.name) : QString("Pin %1").arg(entry.name));
        pin->setIcon(create_chrome_icon(entry.pinned ? ChromeIcon::PinFilled : ChromeIcon::Pin, entry.pinned ? Theme::accent() : panel.muted));
        QObject::connect(pin, &QToolButton::clicked, this, [this, extension_id = entry.extension_id, pinned = entry.pinned] {
            if (on_set_pinned)
                on_set_pinned(extension_id, !pinned);
        });
        line_layout->addWidget(pin);

        auto* more = create_row_button(*line, QString("More actions for %1").arg(entry.name));
        more->setIcon(create_chrome_icon(ChromeIcon::More, panel.muted));
        QObject::connect(more, &QToolButton::clicked, this, [this, more, extension_id = entry.extension_id] {
            if (on_show_menu)
                on_show_menu(extension_id, more->mapToGlobal(QPoint(0, more->height())));
        });
        line_layout->addWidget(more);

        m_rows_layout->addWidget(line);
    }

    if (m_entries.is_empty()) {
        auto* empty = new QLabel("No extensions have a toolbar button", m_rows);
        empty->setObjectName("LadybirdExtensionsPopoverEmpty");
        empty->setContentsMargins(10, 6, 10, 6);
        m_rows_layout->addWidget(empty);
    }

    card().layout()->activate();
    adjustSize();
}

void ExtensionsPopover::update_chrome_style(QPalette const& palette)
{
    setPalette(palette);
    setStyleSheet(ChromeStyle::extensions_popover_style_sheet(palette));
    rebuild_rows();
}

}
