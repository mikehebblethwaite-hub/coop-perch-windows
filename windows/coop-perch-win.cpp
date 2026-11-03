/* Copyright (c) 2026 Mike J Hebblethwaite. All rights reserved. See ../COPYRIGHT. */
#include "PerchFrame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

namespace {

constexpr int kGridX = 16;
constexpr int kGridY = 10;
constexpr std::uint32_t kBoxHeaderBytes = 4096;
constexpr std::uint32_t kBoxVersion = 2;
constexpr std::uint32_t kRingFrames = 600;
constexpr std::size_t kCellCount = kGridX * kGridY;
constexpr std::size_t kFrameValues = kCellCount * 9;
constexpr std::uint32_t kPlaneFrameBytes = 8 + static_cast<std::uint32_t>(kFrameValues * sizeof(std::uint16_t));

struct BoxHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t headerBytes;
    std::uint32_t ring;
    std::uint32_t ndim;
    std::uint32_t dim[4];
    std::uint32_t frameElems;
    std::uint32_t frameBytes;
    char dtype[4];
    std::uint32_t elemBytes;
    volatile LONG64 count;
    volatile LONG wake;
    std::uint32_t writerPid;
    char plane[32];
    char fields[1024];
};

static_assert(sizeof(BoxHeader) <= kBoxHeaderBytes);
static_assert(offsetof(BoxHeader, count) == 56);
static_assert(sizeof(BoxHeader) == 1128);
static_assert(kPlaneFrameBytes % 8 == 0);

struct Cell {
    std::array<std::uint16_t, 9> values{};
    std::array<double, 3> sums{};
    bool changed = false;
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

std::uint64_t unix_time_100ns() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart - 116444736000000000ULL;
}

double unix_seconds() {
    return static_cast<double>(unix_time_100ns()) / 10000000.0;
}

bool same_rgb(const BYTE* a, const BYTE* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

HANDLE g_stop_event = nullptr;

BOOL WINAPI console_handler(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT) return FALSE;
    if (g_stop_event) SetEvent(g_stop_event);
    return TRUE;
}

std::filesystem::path default_output_dir() {
    wchar_t localAppData[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        throw std::runtime_error("LOCALAPPDATA is not available");
    }
    return std::filesystem::path(localAppData) / L"Perch" / L"tape" / L"display";
}

class BoxWriter {
public:
    explicit BoxWriter(const std::filesystem::path& directory) {
        std::filesystem::create_directories(directory);
        const auto path = directory / L"eye.box";
        file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("cannot create eye.box");
        }

        const std::uint64_t bytes = kBoxHeaderBytes + static_cast<std::uint64_t>(kPlaneFrameBytes) * kRingFrames;
        LARGE_INTEGER size{};
        size.QuadPart = static_cast<LONGLONG>(bytes);
        if (!SetFilePointerEx(file_, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file_)) {
            throw std::runtime_error("cannot size eye.box");
        }
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        if (!mapping_) {
            throw std::runtime_error("cannot map eye.box");
        }
        base_ = static_cast<BYTE*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, 0));
        if (!base_) {
            throw std::runtime_error("cannot map eye.box view");
        }
        header_ = reinterpret_cast<BoxHeader*>(base_);
        std::memset(base_, 0, kBoxHeaderBytes);
        std::memcpy(header_->magic, "PERCHBOX", 8);
        header_->version = kBoxVersion;
        header_->headerBytes = kBoxHeaderBytes;
        header_->ring = kRingFrames;
        header_->ndim = 3;
        header_->dim[0] = kGridY;
        header_->dim[1] = kGridX;
        header_->dim[2] = 9;
        header_->frameElems = static_cast<std::uint32_t>(kFrameValues);
        header_->frameBytes = kPlaneFrameBytes;
        std::memcpy(header_->dtype, "u16", 4);
        header_->elemBytes = 2;
        header_->writerPid = GetCurrentProcessId();
        std::memcpy(header_->plane, "eye", 4);
        std::snprintf(header_->fields, sizeof header_->fields,
                      "R G B luma strike zoom_x zoom_y zoom_w zoom_h; screen vectors from IddCx changed frames");
        event_ = CreateEventW(nullptr, FALSE, FALSE, L"Local\\CoopPerch.eye");
        if (!event_) throw std::runtime_error("cannot create the eye plane event");
    }

    ~BoxWriter() {
        if (base_) UnmapViewOfFile(base_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        if (event_) CloseHandle(event_);
    }

    void write(const std::array<Cell, kCellCount>& cells, double timestamp) {
        const LONG64 count = InterlockedCompareExchange64(&header_->count, 0, 0);
        BYTE* frame = base_ + kBoxHeaderBytes + static_cast<std::size_t>(count % kRingFrames) * kPlaneFrameBytes;
        std::memcpy(frame, &timestamp, sizeof timestamp);
        auto* values = reinterpret_cast<std::uint16_t*>(frame + sizeof timestamp);
        for (std::size_t i = 0; i < kCellCount; ++i) {
            std::memcpy(values + i * 9, cells[i].values.data(), cells[i].values.size() * sizeof(std::uint16_t));
        }
        MemoryBarrier();
        InterlockedExchange64(&header_->count, count + 1);
        InterlockedIncrement(&header_->wake);
        if (!SetEvent(event_)) throw std::runtime_error("cannot signal the eye plane event");
    }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    HANDLE event_ = nullptr;
    BYTE* base_ = nullptr;
    BoxHeader* header_ = nullptr;
};

class Eye {
public:
    explicit Eye(const std::filesystem::path& output)
        : box_(output), tape_(output / L"tape.txt", std::ios::app), vectors_(output / L"vectors.bin", std::ios::binary | std::ios::app) {
        if (!tape_ || !vectors_) throw std::runtime_error("cannot open Perch tape files");
        tape_ << "# shape: time code fields | S strike col,row@x,y wxh\n";
        startTime_ = unix_seconds();
    }

    bool consume(std::uint32_t width, std::uint32_t height, std::uint32_t stride, const BYTE* pixels) {
        if (!pixels || width < kGridX || height < kGridY || width > perch::win::kMaxWidth || height > perch::win::kMaxHeight ||
            stride < width * 4) {
            return false;
        }
        const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
        if (width != width_ || height != height_) {
            width_ = width;
            height_ = height;
            previous_.assign(bytes, 0);
            cells_ = {};
            copy_frame(pixels, stride, previous_.data());
            initialize_vectors(previous_.data());
            tape_ << time_text() << " O eye display " << width_ << "x" << height_ << " " << kGridX << "x" << kGridY << "\n";
            tape_.flush();
            if (!tape_) throw std::runtime_error("cannot flush Perch tape");
            return true;
        }

        std::array<int, kCellCount> changed{};
        int changedCount = 0;
        for (Cell& cell : cells_) {
            cell.changed = false;
            cell.values[4] = 0;
            cell.values[5] = cell.values[6] = cell.values[7] = cell.values[8] = 0;
        }
        const int cellWidth = std::max(1, static_cast<int>(width_) / kGridX);
        const int cellHeight = std::max(1, static_cast<int>(height_) / kGridY);
        for (int row = 0; row < kGridY; ++row) {
            const int y0 = row * cellHeight;
            const int y1 = row == kGridY - 1 ? static_cast<int>(height_) : std::min(static_cast<int>(height_), y0 + cellHeight);
            for (int col = 0; col < kGridX; ++col) {
                const int x0 = col * cellWidth;
                const int x1 = col == kGridX - 1 ? static_cast<int>(width_) : std::min(static_cast<int>(width_), x0 + cellWidth);
                Cell& cell = cells_[row * kGridX + col];
                int bx0 = x1, by0 = y1, bx1 = x0, by1 = y0;
                bool differs = false;
                for (int y = y0; y < y1; ++y) {
                    const BYTE* current = pixels + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x0) * 4;
                    const BYTE* old = previous_.data() + (static_cast<std::size_t>(y) * width_ + x0) * 4;
                    if (std::memcmp(current, old, static_cast<std::size_t>(x1 - x0) * 4) == 0) continue;
                    for (int x = x0; x < x1; ++x) {
                        const BYTE* nowPixel = pixels + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4;
                        const BYTE* oldPixel = previous_.data() + (static_cast<std::size_t>(y) * width_ + x) * 4;
                        if (!same_rgb(nowPixel, oldPixel)) {
                            differs = true;
                            bx0 = std::min(bx0, x);
                            bx1 = std::max(bx1, x + 1);
                            by0 = std::min(by0, y);
                            by1 = std::max(by1, y + 1);
                        }
                    }
                }
                if (!differs) {
                    for (int y = y0; y < y1; ++y) {
                        const BYTE* source = pixels + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x0) * 4;
                        BYTE* destination = previous_.data() + (static_cast<std::size_t>(y) * width_ + x0) * 4;
                        std::memcpy(destination, source, static_cast<std::size_t>(x1 - x0) * 4);
                    }
                    continue;
                }

                for (int y = by0; y < by1; ++y) {
                    const BYTE* source = pixels + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(bx0) * 4;
                    BYTE* destination = previous_.data() + (static_cast<std::size_t>(y) * width_ + bx0) * 4;
                    for (int x = bx0; x < bx1; ++x, source += 4, destination += 4) {
                        cell.sums[0] += static_cast<double>(source[2]) - destination[2];
                        cell.sums[1] += static_cast<double>(source[1]) - destination[1];
                        cell.sums[2] += static_cast<double>(source[0]) - destination[0];
                    }
                    const BYTE* sourceRect = pixels + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(bx0) * 4;
                    BYTE* destinationRect = previous_.data() + (static_cast<std::size_t>(y) * width_ + bx0) * 4;
                    std::memcpy(destinationRect, sourceRect, static_cast<std::size_t>(bx1 - bx0) * 4);
                }
                const std::size_t index = static_cast<std::size_t>(row) * kGridX + col;
                cell.changed = true;
                cell.x0 = bx0;
                cell.y0 = by0;
                cell.x1 = bx1;
                cell.y1 = by1;
                const double area = static_cast<double>(x1 - x0) * (y1 - y0);
                const double r = cell.sums[0] / area, g = cell.sums[1] / area, b = cell.sums[2] / area;
                cell.values[0] = static_cast<std::uint16_t>(r + 0.5);
                cell.values[1] = static_cast<std::uint16_t>(g + 0.5);
                cell.values[2] = static_cast<std::uint16_t>(b + 0.5);
                cell.values[3] = static_cast<std::uint16_t>(0.299 * r + 0.587 * g + 0.114 * b + 0.5);
                cell.values[4] = 1;
                cell.values[5] = static_cast<std::uint16_t>(bx0);
                cell.values[6] = static_cast<std::uint16_t>(by0);
                cell.values[7] = static_cast<std::uint16_t>(bx1 - bx0);
                cell.values[8] = static_cast<std::uint16_t>(by1 - by0);
                changed[changedCount++] = static_cast<int>(index);
                const float record[2 + 4] = {
                    static_cast<float>(unix_seconds() - startTime_), static_cast<float>(index),
                    static_cast<float>(cell.values[0]), static_cast<float>(cell.values[1]),
                    static_cast<float>(cell.values[2]), static_cast<float>(cell.values[3])
                };
                vectors_.write(reinterpret_cast<const char*>(record), sizeof record);
                if (!vectors_) throw std::runtime_error("cannot append Perch vectors");
            }
        }

        if (changedCount == 0) return true;
        const double timestamp = unix_seconds();
        box_.write(cells_, timestamp);
        write_tape(changed, changedCount);
        tape_.flush();
        if (!tape_) throw std::runtime_error("cannot flush Perch tape");
        for (Cell& cell : cells_) cell.changed = false;
        vectors_.flush();
        if (!vectors_) throw std::runtime_error("cannot flush Perch vectors");
        return true;
    }

private:
    void copy_frame(const BYTE* source, std::uint32_t sourceStride, BYTE* destination) const {
        for (std::uint32_t y = 0; y < height_; ++y) {
            std::memcpy(destination + static_cast<std::size_t>(y) * width_ * 4,
                        source + static_cast<std::size_t>(y) * sourceStride, static_cast<std::size_t>(width_) * 4);
        }
    }

    void initialize_vectors(const BYTE* pixels) {
        const int cellWidth = std::max(1, static_cast<int>(width_) / kGridX);
        const int cellHeight = std::max(1, static_cast<int>(height_) / kGridY);
        for (int row = 0; row < kGridY; ++row) {
            const int y0 = row * cellHeight;
            const int y1 = row == kGridY - 1 ? static_cast<int>(height_) : std::min(static_cast<int>(height_), y0 + cellHeight);
            for (int col = 0; col < kGridX; ++col) {
                const int x0 = col * cellWidth;
                const int x1 = col == kGridX - 1 ? static_cast<int>(width_) : std::min(static_cast<int>(width_), x0 + cellWidth);
                double red = 0, green = 0, blue = 0;
                std::uint64_t n = 0;
                for (int y = y0; y < y1; ++y) {
                    const BYTE* p = pixels + (static_cast<std::size_t>(y) * width_ + x0) * 4;
                    for (int x = x0; x < x1; ++x, p += 4) {
                        blue += p[0]; green += p[1]; red += p[2]; ++n;
                    }
                }
                const double r = red / n, g = green / n, b = blue / n;
                Cell& cell = cells_[row * kGridX + col];
                cell.sums = {red, green, blue};
                cell.values[0] = static_cast<std::uint16_t>(r + 0.5);
                cell.values[1] = static_cast<std::uint16_t>(g + 0.5);
                cell.values[2] = static_cast<std::uint16_t>(b + 0.5);
                cell.values[3] = static_cast<std::uint16_t>(0.299 * r + 0.587 * g + 0.114 * b + 0.5);
            }
        }
    }

    void write_tape(const std::array<int, kCellCount>& changed, int count) {
        std::array<bool, kCellCount> seen{};
        std::string line;
        for (int i = 0; i < count; ++i) {
            const int first = changed[i];
            if (seen[first]) continue;
            std::array<int, kCellCount> queue{};
            int head = 0, tail = 0;
            queue[tail++] = first;
            seen[first] = true;
            int c0 = kGridX, c1 = -1, r0 = kGridY, r1 = -1, x0 = width_, y0 = height_, x1 = 0, y1 = 0;
            while (head < tail) {
                const int cellIndex = queue[head++];
                const int col = cellIndex % kGridX;
                const int row = cellIndex / kGridX;
                const Cell& cell = cells_[cellIndex];
                c0 = std::min(c0, col); c1 = std::max(c1, col);
                r0 = std::min(r0, row); r1 = std::max(r1, row);
                x0 = std::min(x0, cell.x0); y0 = std::min(y0, cell.y0);
                x1 = std::max(x1, cell.x1); y1 = std::max(y1, cell.y1);
                for (int dr = -1; dr <= 1; ++dr) {
                    for (int dc = -1; dc <= 1; ++dc) {
                        const int nr = row + dr, nc = col + dc;
                        if (nr < 0 || nc < 0 || nr >= kGridY || nc >= kGridX) continue;
                        const int neighbor = nr * kGridX + nc;
                        if (!seen[neighbor] && cells_[neighbor].changed) {
                            seen[neighbor] = true;
                            queue[tail++] = neighbor;
                        }
                    }
                }
            }
            if (!line.empty()) line += ' ';
            char part[96]{};
            if (c0 == c1 && r0 == r1) {
                std::snprintf(part, sizeof part, "%d,%d@%d,%d %dx%d", c0, r0, x0, y0, x1 - x0, y1 - y0);
            } else {
                std::snprintf(part, sizeof part, "%d,%d-%d,%d@%d,%d %dx%d", c0, r0, c1, r1, x0, y0, x1 - x0, y1 - y0);
            }
            line += part;
        }
        tape_ << time_text() << " S " << line << "\n";
        tape_.flush();
        if (!tape_) throw std::runtime_error("cannot append the Perch tape");
    }

    static std::string time_text() {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char text[16]{};
        std::snprintf(text, sizeof text, "%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
        return text;
    }

    BoxWriter box_;
    std::ofstream tape_;
    std::ofstream vectors_;
    std::array<Cell, kCellCount> cells_{};
    std::vector<BYTE> previous_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    double startTime_ = 0;
};

enum class SnapshotResult { NoNewFrame, Ready, Invalid };

void verify_self_test(const std::filesystem::path& output) {
    auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(std::string("self-test: ") + message);
    };

    const auto boxPath = output / L"eye.box";
    std::ifstream box(boxPath, std::ios::binary);
    BoxHeader header{};
    box.read(reinterpret_cast<char*>(&header), sizeof header);
    require(static_cast<bool>(box), "cannot read eye.box header");
    require(std::memcmp(header.magic, "PERCHBOX", 8) == 0 && header.version == kBoxVersion,
            "eye.box magic or version mismatch");
    require(header.headerBytes == kBoxHeaderBytes && header.ring == kRingFrames &&
                header.ndim == 3 && header.dim[0] == kGridY && header.dim[1] == kGridX &&
                header.dim[2] == 9 && header.frameElems == kFrameValues &&
                header.frameBytes == kPlaneFrameBytes && std::memcmp(header.dtype, "u16", 4) == 0,
            "eye.box layout mismatch");
    std::uint64_t frameCount = 0;
    std::memcpy(&frameCount, reinterpret_cast<const BYTE*>(&header) + offsetof(BoxHeader, count), sizeof frameCount);
    require(frameCount == 1, "eye.box should contain one changed frame");

    std::array<std::uint16_t, kFrameValues> values{};
    box.seekg(kBoxHeaderBytes + sizeof(double), std::ios::beg);
    box.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(sizeof values));
    require(static_cast<bool>(box), "cannot read eye.box test frame");
    for (int row = 0; row < kGridY; ++row) {
        for (int col = 0; col < kGridX; ++col) {
            const std::size_t offset = static_cast<std::size_t>(row * kGridX + col) * 9;
            const bool strike = row >= 3 && row <= 4 && col >= 4 && col <= 6;
            require(values[offset + 4] == (strike ? 1 : 0), "eye.box strike cells mismatch");
            if (strike) {
                require(values[offset] == 255 && values[offset + 1] == 220 && values[offset + 2] == 16 &&
                            values[offset + 3] == 207 && values[offset + 5] == col * 20 &&
                            values[offset + 6] == row * 20 && values[offset + 7] == 20 &&
                            values[offset + 8] == 20,
                        "eye.box changed-cell vectors mismatch");
            }
        }
    }

    std::ifstream tape(output / L"tape.txt");
    std::string lastLine, line;
    while (std::getline(tape, line)) {
        if (!line.empty()) lastLine = line;
    }
    require(tape.eof() && lastLine.find(" S 4,3-6,4@80,60 60x40") != std::string::npos,
            "tape region mismatch");

    struct VectorRecord {
        float seconds;
        float index;
        float red;
        float green;
        float blue;
        float luma;
    };
    const auto vectorsPath = output / L"vectors.bin";
    const std::uintmax_t vectorBytes = std::filesystem::file_size(vectorsPath);
    constexpr std::size_t kChangedCells = 6;
    require(vectorBytes >= sizeof(VectorRecord) * kChangedCells && vectorBytes % sizeof(VectorRecord) == 0,
            "vector record size mismatch");
    std::ifstream vectors(vectorsPath, std::ios::binary);
    vectors.seekg(static_cast<std::streamoff>(vectorBytes - sizeof(VectorRecord) * kChangedCells), std::ios::beg);
    const std::array<int, kChangedCells> expectedIndices = {52, 53, 54, 68, 69, 70};
    for (int expectedIndex : expectedIndices) {
        VectorRecord record{};
        vectors.read(reinterpret_cast<char*>(&record), sizeof record);
        require(static_cast<bool>(vectors) && std::isfinite(record.seconds) && record.seconds >= 0 &&
                    record.index == static_cast<float>(expectedIndex) && record.red == 255 &&
                    record.green == 220 && record.blue == 16 && record.luma == 207,
                "vector record mismatch");
    }
}

SnapshotResult read_snapshot(perch::win::SharedFrames* shared, std::vector<BYTE>& pixels,
                             std::uint32_t& width, std::uint32_t& height, std::uint32_t& stride, LONG64& seen) {
    const LONG64 globalBefore = shared->latestSequence;
    MemoryBarrier();
    if (globalBefore == 0 || globalBefore == seen) return SnapshotResult::NoNewFrame;
    const LONG active = shared->activeSlot;
    MemoryBarrier();
    if (active < 0 || active > 1) return SnapshotResult::Invalid;
    const perch::win::FrameSlot& slot = shared->slots[active];
    const LONG64 before = slot.sequence;
    MemoryBarrier();
    if (before == 0 || (before & 1)) return SnapshotResult::NoNewFrame;
    const std::uint32_t w = slot.width, h = slot.height, rowBytes = slot.stride, byteCount = slot.bytes;
    if (w < kGridX || h < kGridY || w > perch::win::kMaxWidth || h > perch::win::kMaxHeight ||
        rowBytes < w * 4 || rowBytes > perch::win::kMaxWidth * 4 ||
        byteCount != static_cast<std::uint64_t>(rowBytes) * h || byteCount > perch::win::kMaxFrameBytes) {
        return SnapshotResult::Invalid;
    }
    pixels.resize(byteCount);
    std::memcpy(pixels.data(), slot.pixels, byteCount);
    MemoryBarrier();
    const LONG64 after = slot.sequence;
    const LONG64 globalAfter = shared->latestSequence;
    const LONG activeAfter = shared->activeSlot;
    if (before != after || (after & 1) || globalBefore != globalAfter ||
        active != activeAfter) return SnapshotResult::NoNewFrame;
    width = w; height = h; stride = rowBytes; seen = globalAfter;
    return SnapshotResult::Ready;
}

void verify_readonly_snapshot() {
    constexpr std::uint32_t width = 320, height = 200, stride = width * 4;
    constexpr std::uint32_t byteCount = stride * height;
    constexpr std::size_t allocationBytes =
        offsetof(perch::win::SharedFrames, slots) + offsetof(perch::win::FrameSlot, pixels) + byteCount;
    auto release = [](perch::win::SharedFrames* shared) { VirtualFree(shared, 0, MEM_RELEASE); };
    std::unique_ptr<perch::win::SharedFrames, decltype(release)> shared(
        static_cast<perch::win::SharedFrames*>(
            VirtualAlloc(nullptr, allocationBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)),
        release);
    if (!shared) throw std::runtime_error("self-test: cannot allocate a frame snapshot");

    shared->latestSequence = 1;
    shared->activeSlot = 0;
    perch::win::FrameSlot& slot = shared->slots[0];
    slot.sequence = 2;
    slot.width = width;
    slot.height = height;
    slot.stride = stride;
    slot.bytes = byteCount;
    DWORD previousProtection = 0;
    if (!VirtualProtect(shared.get(), allocationBytes, PAGE_READONLY, &previousProtection)) {
        throw std::runtime_error("self-test: cannot protect frame snapshot as read-only");
    }

    std::vector<BYTE> pixels;
    std::uint32_t actualWidth = 0, actualHeight = 0, actualStride = 0;
    LONG64 seen = 0;
    if (read_snapshot(shared.get(), pixels, actualWidth, actualHeight, actualStride, seen) != SnapshotResult::Ready ||
        actualWidth != width || actualHeight != height || actualStride != stride || seen != 1 ||
        pixels.size() != byteCount) {
        throw std::runtime_error("self-test: read-only frame snapshot mismatch");
    }
}

int run(bool selfTest, const std::filesystem::path& output) {
    if (selfTest) {
        {
            Eye eye(output);
            constexpr std::uint32_t width = 320, height = 200, stride = width * 4;
            std::vector<BYTE> frame(static_cast<std::size_t>(stride) * height, 0);
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    BYTE* p = frame.data() + static_cast<std::size_t>(y) * stride + x * 4;
                    p[0] = 24; p[1] = 36; p[2] = 48; p[3] = 255;
                }
            }
            if (!eye.consume(width, height, stride, frame.data())) return 1;
            for (std::uint32_t y = 60; y < 100; ++y) {
                for (std::uint32_t x = 80; x < 140; ++x) {
                    BYTE* p = frame.data() + static_cast<std::size_t>(y) * stride + x * 4;
                    p[0] = 16; p[1] = 220; p[2] = 255;
                }
            }
            if (!eye.consume(width, height, stride, frame.data())) return 1;
        }
        verify_self_test(output);
        verify_readonly_snapshot();
        return 0;
    }

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, perch::win::kFrameMappingName);
    if (!mapping) throw std::runtime_error("IDD frame mapping not available; start the Perch indirect display driver first");
    auto* shared = static_cast<perch::win::SharedFrames*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(perch::win::SharedFrames)));
    if (!shared) {
        CloseHandle(mapping);
        throw std::runtime_error("cannot map the IDD frame feed");
    }
    if (std::memcmp(shared->magic, perch::win::kFrameMagic, 8) != 0 || shared->version != perch::win::kFrameVersion) {
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        throw std::runtime_error("IDD frame feed has an unsupported version");
    }
    HANDLE ready = OpenEventW(SYNCHRONIZE, FALSE, perch::win::kFrameEventName);
    if (!ready) {
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        throw std::runtime_error("cannot open the IDD frame event");
    }
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) {
        CloseHandle(ready); UnmapViewOfFile(shared); CloseHandle(mapping);
        throw std::runtime_error("cannot create the stop event");
    }
    g_stop_event = stop;
    SetConsoleCtrlHandler(console_handler, TRUE);

    std::cout << "Perch Windows eye listening for IddCx frames; tape: " << output.string() << "\n";
    Eye eye(output);
    std::vector<BYTE> pixels;
    LONG64 seen = 0;
    std::uint32_t width = 0, height = 0, stride = 0;
    for (;;) {
        HANDLE events[] = {ready, stop};
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0 + 1) break;
        if (wait != WAIT_OBJECT_0) {
            CloseHandle(stop); CloseHandle(ready); UnmapViewOfFile(shared); CloseHandle(mapping);
            throw std::runtime_error("wait for IddCx frame failed");
        }
        const SnapshotResult snapshot = read_snapshot(shared, pixels, width, height, stride, seen);
        if (snapshot == SnapshotResult::Invalid) {
            CloseHandle(stop); CloseHandle(ready); UnmapViewOfFile(shared); CloseHandle(mapping);
            throw std::runtime_error("invalid frame metadata received from the IDD driver");
        }
        if (snapshot == SnapshotResult::Ready && !eye.consume(width, height, stride, pixels.data())) {
            CloseHandle(stop); CloseHandle(ready); UnmapViewOfFile(shared); CloseHandle(mapping);
            throw std::runtime_error("invalid frame received from the IDD driver");
        }
    }
    SetConsoleCtrlHandler(console_handler, FALSE);
    g_stop_event = nullptr;
    CloseHandle(stop);
    CloseHandle(ready);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        bool selfTest = false;
        std::filesystem::path output = default_output_dir();
        for (int i = 1; i < argc; ++i) {
            if (std::wcscmp(argv[i], L"--self-test") == 0) selfTest = true;
            else if (std::wcscmp(argv[i], L"--output") == 0 && i + 1 < argc) output = argv[++i];
            else if (std::wcscmp(argv[i], L"--help") == 0) {
                std::wcout << L"Usage: coop-perch-win.exe [--output DIRECTORY] [--self-test]\n";
                return 0;
            } else {
                std::wcerr << L"Unknown argument: " << argv[i] << L"\n";
                return 2;
            }
        }
        const int result = run(selfTest, output);
        if (selfTest && result == 0) std::cout << "Perch Windows eye self-test passed; output: " << output.string() << "\n";
        return result;
    } catch (const std::exception& e) {
        std::cerr << "coop-perch-win: " << e.what() << "\n";
        return 1;
    }
}
