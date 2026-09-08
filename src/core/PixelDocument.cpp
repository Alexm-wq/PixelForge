#include "PixelDocument.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace pixelforge {

PixelDocument::PixelDocument(CanvasLimits limits) : limits_(limits) {}

bool PixelDocument::can_resize(int width, int height, std::string* reason) const {
    if (width < limits_.min_width || height < limits_.min_height) {
        if (reason) *reason = "Canvas dimensions must be positive.";
        return false;
    }
    if (width > limits_.max_width || height > limits_.max_height) {
        if (reason) *reason = "Canvas dimensions exceed the explicitly configured host limits.";
        return false;
    }
    const auto pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixels > limits_.max_pixels) {
        if (reason) *reason = "Canvas pixel count exceeds the explicitly configured host limit.";
        return false;
    }
    if (pixels > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))) {
        if (reason) *reason = "Canvas is too large to represent in this process address space.";
        return false;
    }
    return true;
}

bool PixelDocument::resize(int width, int height, std::string* reason) {
    if (!can_resize(width, height, reason)) return false;
    const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<std::uint32_t> replacement;
    try {
        replacement.assign(count, 0x00000000u);
    } catch (const std::bad_alloc&) {
        if (reason) *reason = "Not enough memory to allocate the requested canvas.";
        return false;
    } catch (const std::length_error&) {
        if (reason) *reason = "Requested canvas exceeds the vector representation supported by this runtime.";
        return false;
    }
    width_ = width;
    height_ = height;
    pixels_ = std::move(replacement);
    ++revision_;
    clear_history();
    return true;
}

bool PixelDocument::in_bounds(int x, int y) const noexcept {
    return x >= 0 && y >= 0 && x < width_ && y < height_;
}

bool PixelDocument::replace_pixels(int width, int height, std::vector<std::uint32_t> pixels, std::string* reason) {
    if (!can_resize(width, height, reason)) return false;
    if (pixels.size() != static_cast<std::size_t>(width) * height) {
        if (reason) *reason = "Restored pixel count does not match canvas dimensions.";
        return false;
    }
    width_ = width; height_ = height; pixels_ = std::move(pixels);
    ++revision_; clear_history();
    return true;
}

std::size_t PixelDocument::index_of(int x, int y) const noexcept {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x);
}

std::uint32_t PixelDocument::pixel(int x, int y) const {
    if (!in_bounds(x, y)) return 0;
    return pixels_[index_of(x, y)];
}

PixelDocument::Transaction::Transaction(PixelDocument& document)
    : document_(&document), base_revision_(document.revision()) {}

PixelDocument::Transaction::Transaction(Transaction&& other) noexcept
    : document_(std::exchange(other.document_, nullptr)),
      pending_(std::move(other.pending_)),
      pending_index_(std::move(other.pending_index_)),
      base_revision_(other.base_revision_),
      completed_(std::exchange(other.completed_, true)) {}

PixelDocument::Transaction& PixelDocument::Transaction::operator=(Transaction&& other) noexcept {
    if (this == &other) return *this;
    cancel();
    document_ = std::exchange(other.document_, nullptr);
    pending_ = std::move(other.pending_);
    pending_index_ = std::move(other.pending_index_);
    base_revision_ = other.base_revision_;
    completed_ = std::exchange(other.completed_, true);
    return *this;
}

PixelDocument::Transaction::~Transaction() {
    if (!completed_) cancel();
}

bool PixelDocument::Transaction::set_pixel(int x, int y, std::uint32_t argb) {
    if (!document_ || completed_ || document_->revision() != base_revision_ || !document_->in_bounds(x, y)) return false;
    // Small mouse edits need no canvas-sized allocation. Large patches switch
    // to direct indexing, making repeated/overlapping writes constant time.
    if (pending_index_.empty() && pending_.size() >= 64) {
        pending_index_.assign(document_->pixels().size(), -1);
        for (std::size_t i = 0; i < pending_.size(); ++i)
            pending_index_[document_->index_of(pending_[i].x, pending_[i].y)] = static_cast<int>(i);
    }
    const auto index = document_->index_of(x, y);
    if (!pending_index_.empty()) {
        const int slot = pending_index_[index];
        if (slot >= 0) {
            pending_[slot].after = argb;
            return true;
        }
    } else {
        for (auto& change : pending_) {
            if (change.x == x && change.y == y) { change.after = argb; return true; }
        }
    }
    const auto before = document_->pixel(x, y);
    if (before == argb) return true;
    if (!pending_index_.empty()) pending_index_[index] = static_cast<int>(pending_.size());
    pending_.push_back({x, y, before, argb});
    return true;
}

bool PixelDocument::Transaction::fill_rect(int x, int y, int width, int height, std::uint32_t argb) {
    if (!document_ || completed_ || width <= 0 || height <= 0) return false;
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = static_cast<int>(std::min<std::int64_t>(document_->width(), static_cast<std::int64_t>(x) + width));
    const int y1 = static_cast<int>(std::min<std::int64_t>(document_->height(), static_cast<std::int64_t>(y) + height));
    if (x0 >= x1 || y0 >= y1) return false;
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) if (!set_pixel(px, py, argb)) return false;
    }
    return true;
}

bool PixelDocument::Transaction::commit() {
    if (!document_ || completed_ || document_->revision() != base_revision_) return false;
    completed_ = true;
    return document_->commit_changes(std::move(pending_));
}

std::size_t PixelDocument::Transaction::pending_changes() const noexcept {
    return static_cast<std::size_t>(std::count_if(pending_.begin(), pending_.end(),
        [](const PixelChange& change) { return change.before != change.after; }));
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
    }
    delta.changes = std::move(changes);

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
