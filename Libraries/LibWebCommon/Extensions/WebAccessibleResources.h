/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Optional.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web::Extensions {

// Whether an extension, by its manifest, lets what made a request, of an origin, load the file at the path, relative to
// the extension's root. Manifest V2 makes its web-accessible resources accessible to every page; Manifest V3 names the
// sites and extensions that may load each of them, so it needs the requesting origin.
// https://developer.chrome.com/docs/extensions/reference/manifest/web-accessible-resources
WEBCOMMON_API bool is_web_accessible_resource(JsonObject const& manifest, StringView path, Optional<URL::Origin> const& requesting_origin);

}
