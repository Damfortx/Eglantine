// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Phase 5b (EDEN_NX2_MT_REPLAY): the replay encoder over Vulkan command buffers. The encoder
// itself is generic (mt/replay_arena.h) so the standalone stress tests run the same code.

#include "video_core/mt/replay_arena.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

using VideoCommon::MT::ReplayClass;
using VideoCommon::MT::ReplayCloseReason;
using DrawReplay = VideoCommon::MT::BasicDrawReplay<vk::CommandBuffer>;

} // namespace Vulkan
