/* Copyright (c) 2026 Mike J Hebblethwaite. All rights reserved. See ../COPYRIGHT. */
#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>

namespace perch::win {

constexpr wchar_t kFrameMappingName[] = L"Global\\CoopPerch.Idd.Frames.v1";
constexpr wchar_t kFrameEventName[] = L"Global\\CoopPerch.Idd.FrameReady.v1";
constexpr char kFrameMagic[8] = {'P', 'E', 'R', 'C', 'H', 'F', 'R', 'M'};
constexpr std::uint32_t kFrameVersion = 1;
constexpr std::uint32_t kMaxWidth = 3840;
constexpr std::uint32_t kMaxHeight = 2160;
constexpr std::size_t kMaxFrameBytes = static_cast<std::size_t>(kMaxWidth) * kMaxHeight * 4;

struct FrameSlot {
    volatile LONG64 sequence;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;
    std::uint32_t format;
    std::uint32_t bytes;
    std::uint32_t reserved;
    BYTE pixels[kMaxFrameBytes];
};

struct SharedFrames {
    char magic[8];
    std::uint32_t version;
    volatile LONG activeSlot;
    volatile LONG64 latestSequence;
    FrameSlot slots[2];
};

} // namespace perch::win
