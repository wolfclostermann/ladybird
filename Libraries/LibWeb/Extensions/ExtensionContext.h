/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Extensions {

struct Extension;

// A realm in which an extension's code runs, with the extension APIs: the isolated world of its content scripts in a
// document, or the realm of one of its own pages.
class ExtensionContext final : public GC::Cell {
    GC_CELL(ExtensionContext, GC::Cell);
    GC_DECLARE_ALLOCATOR(ExtensionContext);

public:
    enum class Type : u8 {
        ContentScript,
        ExtensionPage,
    };

    // Sets up the extension APIs in realm, a realm of document.
    static GC::Ref<ExtensionContext> create(DOM::Document&, JS::Realm&, Extension const&, Type);

    String const& extension_id() const { return m_extension_id; }
    Type type() const { return m_type; }
    JS::Realm& realm() const { return *m_realm; }

    // Runs the listeners of an event of the extension APIs in this context. Calls on_complete with the JSON of a response
    // that a listener gave, if one did.
    void dispatch_event(String const& event, String const& arguments_json, Function<void(Optional<String>)> on_complete);

private:
    ExtensionContext(String extension_id, Type, JS::Realm&);

    virtual void visit_edges(Cell::Visitor&) override;

    String m_extension_id;
    Type m_type;
    GC::Ref<JS::Realm> m_realm;
    GC::Ptr<JS::FunctionObject> m_dispatcher;
};

// Settles the promise of a call to the extension APIs, with the JSON of its result or an error message.
WEB_API void complete_api_call(u64 call_id, bool is_error, String const& payload);

// Dispatches an event of the extension APIs to the contexts of an extension in page's documents. target_json picks
// them: {"contextType": "content_script" or "extension_page", "frameId": n}, each optional. Calls on_complete with the
// JSON of the first response a listener gave, if one did.
WEB_API void dispatch_event(Page&, String const& extension_id, String const& target_json, String const& event, String const& arguments_json, Function<void(Optional<String>)> on_complete);

// The ID by which the extension APIs name a document's frame: 0 for that of a top-level traversable.
u64 frame_id_of(DOM::Document const&);

}
