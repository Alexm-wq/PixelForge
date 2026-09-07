#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace pixelforge::win32 {

class CanvasPlayback {
public:
    void note_task(std::uint64_t task_id) {
        if (task_id == task_id_) return;
        task_id_ = task_id;
        waiting_for_canvas_ = task_id != 0;
        queue_.clear();
    }

    void reset_to_authoritative(std::uint64_t task_id,
                                int width,
                                int height,
                                std::uint64_t revision,
                                const std::vector<std::uint32_t>& pixels) {
        task_id_ = task_id;
        waiting_for_canvas_ = false;
        width_ = width;
        height_ = height;
        revision_ = revision;
        shown_ = pixels;
        authoritative_ = pixels;
        queue_.clear();
    }

    bool observe_authoritative(std::uint64_t task_id,
                               bool canvas_ready,
                               int width,
                               int height,
                               std::uint64_t revision,
                               const std::vector<std::uint32_t>& pixels) {
        note_task(task_id);
        if (!canvas_ready || width <= 0 || height <= 0) return false;

        const std::size_t expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        if (pixels.size() != expected) return false;

        bool changed = false;
        if (waiting_for_canvas_ || width_ != width || height_ != height || shown_.size() != expected) {
            waiting_for_canvas_ = false;
            width_ = width;
            height_ = height;
            shown_.assign(expected, 0x00000000u);
            authoritative_.assign(expected, 0x00000000u);
            queue_.clear();
            changed = true;
        }

        if (revision == revision_ && authoritative_ == pixels) return changed;
        if (authoritative_.size() != pixels.size()) authoritative_.assign(pixels.size(), 0x00000000u);

        changed = enqueue_diff(authoritative_, pixels, width, revision) || changed;
        authoritative_ = pixels;
        revision_ = revision;
        return changed;
    }

    bool tick(std::uint64_t now_ms) {
        if (queue_.empty()) return false;
        auto& commit = queue_.front();
        if (commit.start_ms == 0) commit.start_ms = now_ms;

        const std::uint64_t elapsed = now_ms >= commit.start_ms ? now_ms - commit.start_ms : 0;
        const double linear = std::clamp(
            static_cast<double>(elapsed) / static_cast<double>(std::max<std::uint32_t>(1, commit.duration_ms)),
            0.0, 1.0);
        const double eased = linear * linear * (3.0 - 2.0 * linear);
        std::size_t target = static_cast<std::size_t>(eased * static_cast<double>(commit.changes.size()));
        if (elapsed > 0 && target == 0 && !commit.changes.empty()) target = 1;
        target = std::min(target, commit.changes.size());

        bool changed = false;
        while (commit.revealed < target) {
            const auto& pixel = commit.changes[commit.revealed++];
            if (pixel.index < shown_.size()) shown_[pixel.index] = pixel.after;
            changed = true;
        }

        if (linear >= 1.0 || commit.revealed >= commit.changes.size()) {
            while (commit.revealed < commit.changes.size()) {
                const auto& pixel = commit.changes[commit.revealed++];
                if (pixel.index < shown_.size()) shown_[pixel.index] = pixel.after;
                changed = true;
            }
            queue_.pop_front();
        }
        return changed;
    }

    [[nodiscard]] const std::vector<std::uint32_t>& pixels_or(
        const std::vector<std::uint32_t>& authoritative,
        int width,
        int height) const noexcept {
        const std::size_t expected = width > 0 && height > 0
            ? static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
            : 0;
        return width_ == width && height_ == height && shown_.size() == expected ? shown_ : authoritative;
    }

    [[nodiscard]] bool animating() const noexcept { return !queue_.empty(); }

private:
    struct DisplayPixelChange {
        std::size_t index = 0;
        std::uint32_t after = 0;
        std::uint64_t order = 0;
    };

    struct AnimatedCommit {
        std::vector<DisplayPixelChange> changes;
        std::uint64_t start_ms = 0;
        std::uint32_t duration_ms = 0;
        std::size_t revealed = 0;
    };

    static std::uint32_t mix32(std::uint32_t value) noexcept {
        value ^= value >> 16;
        value *= 0x7feb352du;
        value ^= value >> 15;
        value *= 0x846ca68bu;
        value ^= value >> 16;
        return value;
    }

    static std::uint32_t duration_for(std::size_t count, std::size_t backlog) noexcept {
        double duration = 240.0 + std::sqrt(static_cast<double>(count)) * 44.0;
        duration = std::clamp(duration, 300.0, 2200.0);
        if (backlog >= 4) duration = std::min(duration, 450.0);
        else if (backlog >= 2) duration = std::min(duration, 900.0);
        return static_cast<std::uint32_t>(duration);
    }

    bool enqueue_diff(const std::vector<std::uint32_t>& before,
                      const std::vector<std::uint32_t>& after,
                      int width,
                      std::uint64_t revision) {
        if (before.size() != after.size() || width <= 0) return false;

        AnimatedCommit commit;
        commit.changes.reserve(after.size() / 4 + 1);
        for (std::size_t i = 0; i < after.size(); ++i) {
            if (before[i] == after[i]) continue;
            const int x = static_cast<int>(i % static_cast<std::size_t>(width));
            const int y = static_cast<int>(i / static_cast<std::size_t>(width));
            const std::uint32_t tile_x = static_cast<std::uint32_t>(x / 4);
            const std::uint32_t tile_y = static_cast<std::uint32_t>(y / 4);
            const std::uint32_t tile_seed = mix32(
                tile_x * 0x9e3779b9u ^ tile_y * 0x85ebca6bu ^ static_cast<std::uint32_t>(revision));
            const std::uint32_t local = static_cast<std::uint32_t>((y & 3) * 4 + (x & 3));
            commit.changes.push_back({i, after[i], (static_cast<std::uint64_t>(tile_seed) << 8) | local});
        }
        if (commit.changes.empty()) return false;

        std::sort(commit.changes.begin(), commit.changes.end(), [](const auto& a, const auto& b) {
            return a.order < b.order;
        });
        commit.duration_ms = duration_for(commit.changes.size(), queue_.size());
        queue_.push_back(std::move(commit));
        return true;
    }

    std::uint64_t task_id_ = 0;
    std::uint64_t revision_ = 0;
    bool waiting_for_canvas_ = false;
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint32_t> shown_;
    std::vector<std::uint32_t> authoritative_;
    std::deque<AnimatedCommit> queue_;
};

} // namespace pixelforge::win32
