// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// TextureCache's multithreaded back methods (docs/mt-phase5-spec.md §7.1, §11.1). Included at
// the end of texture_cache.h.

#include "video_core/texture_cache/texture_cache_base.h"

namespace VideoCommon {

template <class P>
void TextureCache<P>::MtSetEpochManager(MT::EpochManager* manager) {
    mt_epoch = manager;
    slot_images.SetEpochManager(manager);
    slot_image_views.SetEpochManager(manager);
    slot_samplers.SetEpochManager(manager);
    for (auto& channel : channel_storage) {
        channel.image_views_mt.SetEpochManager(manager);
        channel.samplers_mt.SetEpochManager(manager);
    }
}

} // namespace VideoCommon
