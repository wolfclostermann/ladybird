/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWebCommon/HTML/CrossProcessId.h>

namespace Web::Extensions {

// The ID by which the extension APIs name a frame: 0 for a top-level traversable, and for any other navigable a number
// that its ID makes unique among those of every process, and that JavaScript holds exactly.
inline u64 frame_id_for_navigable(HTML::CrossProcessId navigable_id, bool is_top_level_traversable)
{
    if (is_top_level_traversable)
        return 0;
    return ((navigable_id.namespace_id & 0x1fffff) << 32) | (navigable_id.local_id & 0xffffffff);
}

}
