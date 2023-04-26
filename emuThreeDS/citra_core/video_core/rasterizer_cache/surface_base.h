// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <boost/icl/interval_set.hpp>
#include "video_core/rasterizer_cache/surface_params.h"

namespace VideoCore {

using SurfaceRegions = boost::icl::interval_set<PAddr, std::less, SurfaceInterval>;

class SurfaceBase : public SurfaceParams {
public:
    explicit SurfaceBase(const SurfaceParams& params);

    bool Overlaps(PAddr overlap_addr, size_t overlap_size) const noexcept {
        const PAddr overlap_end = overlap_addr + static_cast<PAddr>(overlap_size);
        return addr < overlap_end && overlap_addr < end;
    }

    u64 ModificationTick() const noexcept {
        return modification_tick;
    }

    CustomPixelFormat CustomFormat() const noexcept {
        return custom_format;
    }

    bool IsCustom() const noexcept {
        return is_custom;
    }

    bool IsRegionValid(SurfaceInterval interval) const {
        return (invalid_regions.find(interval) == invalid_regions.end());
    }

    void MarkValid(SurfaceInterval interval) {
        invalid_regions.erase(interval);
        modification_tick++;
    }

    void MarkInvalid(SurfaceInterval interval) {
        invalid_regions.insert(interval);
        modification_tick++;
    }

    bool IsFullyInvalid() const {
        auto interval = GetInterval();
        return *invalid_regions.equal_range(interval).first == interval;
    }

    /// Returns true when this surface can be used to fill the fill_interval of dest_surface
    bool CanFill(const SurfaceParams& dest_surface, SurfaceInterval fill_interval) const;

    /// Returns true when surface can validate copy_interval of dest_surface
    bool CanCopy(const SurfaceParams& dest_surface, SurfaceInterval copy_interval) const;

    /// Returns the region of the biggest valid rectange within interval
    SurfaceInterval GetCopyableInterval(const SurfaceParams& params) const;

    /// Returns the clear value used to validate another surface from this fill surface
    ClearValue MakeClearValue(PAddr copy_addr, PixelFormat dst_format);

private:
    /// Returns the fill buffer value starting from copy_addr
    std::array<u8, 4> MakeFillBuffer(PAddr copy_addr);

public:
    bool registered = false;
    bool picked = false;
    bool is_custom = false;
    CustomPixelFormat custom_format{};
    SurfaceRegions invalid_regions;
    std::array<u8, 4> fill_data;
    u32 fill_size = 0;
    u64 modification_tick = 1;
};

} // namespace VideoCore
