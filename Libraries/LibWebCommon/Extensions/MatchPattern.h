/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web::Extensions {

// https://developer.chrome.com/docs/extensions/develop/concepts/match-patterns
WEBCOMMON_API bool match_pattern_matches(StringView pattern, URL::URL const&);

}
