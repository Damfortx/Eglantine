// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// TextureCache's multithreaded back methods (docs/mt-phase5-spec.md §7.1, §11.1). Included at
// the end of texture_cache.h.

#include <algorithm>
#include <optional>

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

namespace MtDetail {
/// What makes an image need more than an LRU touch from PrepareImage (TryFillImageViewsClean's
/// conditions, plus sparse work a written view would trigger and deletion).
inline constexpr ImageFlagBits UncleanImageFlags =
    ImageFlagBits::CpuModified | ImageFlagBits::HasAliases | ImageFlagBits::Dead |
    ImageFlagBits::UnwrittenSparse | ImageFlagBits::SparseTiles | ImageFlagBits::SparseLayers;
} // namespace MtDetail

template <class P>
auto TextureCache<P>::TryResolveViewId(ImageViewId id, bool blacklist, MtResolvedView& out,
                                       bool allow_refresh) const noexcept -> MtViewResolve {
    if (id == NULL_IMAGE_VIEW_ID) {
        out = MtResolvedView{};
        return MtViewResolve::Null;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        const u32 view_gen = slot_image_views.Generation(id);
        if ((view_gen & 1) == 0) {
            return MtViewResolve::Miss;
        }
        const ImageView& view = slot_image_views.UncheckedAt(id);
        if (view.IsBuffer()) {
            return MtViewResolve::NotClean;
        }
        const ImageId image_id = view.image_id;
        const u32 image_gen = slot_images.Generation(image_id);
        if ((image_gen & 1) == 0) {
            return MtViewResolve::Miss;
        }
        const Image& image = slot_images.UncheckedAt(image_id);
        const u32 version = image.version.load(std::memory_order_acquire);
        const ImageFlagBits flags = image.flags.Load(std::memory_order_acquire);
        // A CPU-modified image needs only RefreshContents from PrepareImage when nothing else
        // makes it unclean: with allow_refresh the committer does that at commit.
        const ImageFlagBits unclean_flags =
            allow_refresh ? MtDetail::UncleanImageFlags & ~ImageFlagBits::CpuModified
                          : MtDetail::UncleanImageFlags;
        const bool unclean = True(flags & unclean_flags) ||
                             (blacklist && True(flags & ImageFlagBits::Rescaled));
        if (slot_image_views.Generation(id) != view_gen ||
            slot_images.Generation(image_id) != image_gen ||
            image.version.load(std::memory_order_acquire) != version) {
            continue;
        }
        if (unclean) {
            return MtViewResolve::NotClean;
        }
        out = MtResolvedView{
            .view = id,
            .view_gen = view_gen,
            .image = image_id,
            .image_gen = image_gen,
            .image_version = version,
            .object = &view,
            .refresh = True(flags & ImageFlagBits::CpuModified),
        };
        return MtViewResolve::Ok;
    }
    return MtViewResolve::Miss;
}

template <class P>
auto TextureCache<P>::TryResolveViewCaptured(const TextureCacheChannelInfo& channel,
                                             const TICEntry& entry, bool valid_entry,
                                             bool blacklist, MtResolvedView& out,
                                             bool allow_refresh) const noexcept -> MtViewResolve {
    if (!valid_entry) {
        out = MtResolvedView{};
        return MtViewResolve::Null;
    }
    // Only ids created from valid entries are in the map; a missing one is a structure change
    // (CreateImageView) the committer makes.
    const std::optional<ImageViewId> found = channel.image_views_mt.Find(entry);
    if (!found) {
        return MtViewResolve::Miss;
    }
    return TryResolveViewId(*found, blacklist, out, allow_refresh);
}

template <class P>
auto TextureCache<P>::TryResolveSamplerCaptured(const TextureCacheChannelInfo& channel,
                                                const TSCEntry& entry, SamplerId& out,
                                                u32& gen) const noexcept -> MtSamplerResolve {
    if (std::ranges::all_of(entry.raw, [](u64 value) { return value == 0; })) {
        out = NULL_SAMPLER_ID;
        gen = 0;
        return MtSamplerResolve::Null;
    }
    const std::optional<SamplerId> found = channel.samplers_mt.Find(entry);
    if (!found) {
        return MtSamplerResolve::Miss;
    }
    gen = slot_samplers.Generation(*found);
    if ((gen & 1) == 0) {
        return MtSamplerResolve::Miss;
    }
    out = *found;
    return MtSamplerResolve::Ok;
}

template <class P>
SamplerId TextureCache<P>::CreateSamplerFromResolver(TextureCacheChannelInfo& channel,
                                                     const TSCEntry& entry) {
    std::scoped_lock lock{sampler_create_mutex};
    if (const std::optional<SamplerId> found = channel.samplers_mt.Find(entry)) {
        return *found;
    }
    const auto [pair, is_new] = channel.samplers.try_emplace(entry);
    if (is_new) {
        pair->second = slot_samplers.insert(runtime, entry);
    }
    channel.samplers_mt.InsertOrAssign(entry, pair->second);
    return pair->second;
}

template <class P>
bool TextureCache<P>::IsRescalingResolved(const MtResolvedView& view) const noexcept {
    if (view.object == nullptr) {
        return false;
    }
    return True(slot_images.UncheckedAt(view.image).flags.Load(std::memory_order_acquire) &
                ImageFlagBits::Rescaled);
}

template <class P>
bool TextureCache<P>::MtValidateView(const MtResolvedView& view) const noexcept {
    if (view.object == nullptr) {
        return true;
    }
    return slot_image_views.IsCurrent(view.view, view.view_gen) &&
           slot_images.IsCurrent(view.image, view.image_gen) &&
           slot_images.UncheckedAt(view.image).version.load(std::memory_order_acquire) ==
               view.image_version;
}

template <class P>
void TextureCache<P>::MtApplyViewIntent(const MtResolvedView& view, bool written, bool blacklist,
                                        bool refresh) {
    if (view.object == nullptr) {
        return;
    }
    Image& image = slot_images[view.image];
    if (refresh) {
        // PrepareImage's RefreshContents (a no-op if an earlier draw refreshed it already).
        RefreshContents(image, view.image);
    }
    if (blacklist) {
        image.scale_rating = 0;
    }
    lru_cache.Touch(image.lru_index, frame_tick);
    if (written) {
        MarkModification(image);
    }
}

} // namespace VideoCommon
