#include "PixelDocument.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace pixelforge {

PixelDocument::PixelDocument(CanvasLimits limits) : limits_(limits) {}

bool PixelDocument::can_resize(int width, int height, std::string* reason) const {
    if (width < limits_.min_width || height < limits_.min_height) {
        if (reason) *reason = "Canvas dimensions must be positive.";
        return false;
    }
    if (width > limits_.max_width || height > limits_.max_height) {
        if (reason) {
            *reason = "Canvas exceeds PixelForge hard limit of " +
                std::to_string(limits_.max_width) + "x" + std::to_string(limits_.max_height) + ".";
        }
        return false;
    }
    const auto pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixels > limits_.max_pixels) {
        if (reason) *reason = "Canvas exceeds PixelForge maximum pixel count.";
        return false;
    }
    return true;
}

bool PixelDocument::resize(int width, int height, std::string* reason) {
    if (!can_resize(width, height, reason)) return false;
    width_ = width;
    height_ = height;
    pixels_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0x00000000u);
    ++revision_;
    clear_history();
    return true;
}

bool PixelDocument::in_bounds(int x, int y) const noexcept {
    return x >= 0 && y >= 0 && x < width_ && y < height_;
}

std::size_t PixelDocument::index_of(int x, int y) const noexcept {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x);
}

std::uint32_t PixelDocument::pixel(int x, int y) const {
    if (!in_bounds(x, y)) return 0;
    return pixels_[index_of(x, y)];
}

PixelDocument::Transaction::Transaction(PixelDocument& document) : document_(&document) {}

PixelDocument::Transaction::Transaction(Transaction&& other) noexcept
    : document_(std::exchange(other.document_, nullptr)),
      pending_(std::move(other.pending_)),
      completed_(std::exchange(other.completed_, true)) {}

PixelDocument::Transaction& PixelDocument::Transaction::operator=(Transaction&& other) noexcept {
    if (this == &other) return *this;
    cancel();
    document_ = std::exchange(other.document_, nullptr);
    pending_ = std::move(other.pending_);
    completed_ = std::exchange(other.completed_, true);
    return *this;
}

PixelDocument::Transaction::~Transaction() {
    if (!completed_) cancel();
}

bool PixelDocument::Transaction::set_pixel(int x, int y, std::uint32_t argb) {
    if (!document_ || !document_->in_bounds(x, y) || completed_) return false;
    for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) {
        if (it->x == x && it->y == y) {
            it->after = argb;
            return true;
        }
    }
    const auto before = document_->pixel(x, y);
    if (before == argb) return true;
    pending_.push_back({x, y, before, argb});
    return true;
}

bool PixelDocument::Transaction::fill_rect(int x, int y, int width, int height, std::uint32_t argb) {
    if (!document_ || completed_ || width <= 0 || height <= 0) return false;
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(document_->width(), x + width);
    const int y1 = std::min(document_->height(), y + height);
    if (x0 >= x1 || y0 >= y1) return false;
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) set_pixel(px, py, argb);
    }
    return true;
}

bool PixelDocument::Transaction::commit() {
    if (!document_ || completed_) return false;
    completed_ = true;
    return document_->commit_changes(std::move(pending_));
}

void PixelDocument::Transaction::cancel() noexcept {
    pending_.clear();
    completed_ = true;
    document_ = nullptr;
}

bool PixelDocument::commit_changes(std::vector<PixelChange> changes) {
    changes.erase(std::remove_if(changes.begin(), changes.end(), [](const PixelChange& change) {
        return change.before == change.after;
    }), changes.end());
    if (changes.empty()) return false;

    EditDelta delta;
    delta.revision_before = revision_;
    delta.min_x = std::numeric_limits<int>::max();
    delta.min_y = std::numeric_limits<int>::max();
    delta.max_x = std::numeric_limits<int>::min();
    delta.max_y = std::numeric_limits<int>::min();

    for (const auto& change : changes) {
        if (!in_bounds(change.x, change.y)) continue;
        pixels_[index_of(change.x, change.y)] = change.after;
        delta.min_x = std::min(delta.min_x, change.x);
        delta.min_y = std::min(delta.min_y, change.y);
        delta.max_x = std::max(delta.max_x, change.x);
        delta.max_y = std::max(delta.max_y, change.y);
        delta.changes.push_back(change);
    }
    if (delta.changes.empty()) return false;

    ++revision_;
    delta.revision_after = revision_;
    undo_stack_.push_back(std::move(delta));
    redo_stack_.clear();
    return true;
}

bool PixelDocument::undo() {
    if (undo_stack_.empty()) return false;
    auto delta = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    for (const auto& change : delta.changes) pixels_[index_of(change.x, change.y)] = change.before;
    ++revision_;
    redo_stack_.push_back(std::move(delta));
    return true;
}

bool PixelDocument::redo() {
    if (redo_stack_.empty()) return false;
    auto delta = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    for (const auto& change : delta.changes) pixels_[index_of(change.x, change.y)] = change.after;
    ++revision_;
    undo_stack_.push_back(std::move(delta));
    return true;
}

void PixelDocument::clear_history() {
    undo_stack_.clear();
    redo_stack_.clear();
}

} // namespace pixelforge
