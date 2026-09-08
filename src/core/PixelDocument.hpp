#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace pixelforge {

struct CanvasLimits {
    int min_width = 1;
    int min_height = 1;
    // No arbitrary artistic-size ceiling. PixelDocument::can_resize still checks
    // representability and any explicitly supplied host policy; allocation itself
    // is the final resource boundary.
    int max_width = std::numeric_limits<int>::max();
    int max_height = std::numeric_limits<int>::max();
    std::uint64_t max_pixels = std::numeric_limits<std::uint64_t>::max();
};

struct PixelChange {
    int x = 0;
    int y = 0;
    std::uint32_t before = 0;
    std::uint32_t after = 0;
};

struct EditDelta {
    std::uint64_t revision_before = 0;
    std::uint64_t revision_after = 0;
    int min_x = 0;
    int min_y = 0;
    int max_x = -1;
    int max_y = -1;
    std::vector<PixelChange> changes;
};

class PixelDocument {
public:
    explicit PixelDocument(CanvasLimits limits = {});

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] const CanvasLimits& limits() const noexcept { return limits_; }
    [[nodiscard]] const std::vector<std::uint32_t>& pixels() const noexcept { return pixels_; }

    [[nodiscard]] bool can_resize(int width, int height, std::string* reason = nullptr) const;
    bool resize(int width, int height, std::string* reason = nullptr);

    [[nodiscard]] std::uint32_t pixel(int x, int y) const;

    class Transaction {
    public:
        explicit Transaction(PixelDocument& document);
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
        Transaction(Transaction&&) noexcept;
        Transaction& operator=(Transaction&&) noexcept;
        ~Transaction();

        bool set_pixel(int x, int y, std::uint32_t argb);
        bool fill_rect(int x, int y, int width, int height, std::uint32_t argb);
        [[nodiscard]] std::size_t pending_changes() const noexcept;
        bool commit();
        void cancel() noexcept;

    private:
        PixelDocument* document_ = nullptr;
        std::vector<PixelChange> pending_;
        std::vector<int> pending_index_;
        std::uint64_t base_revision_ = 0;
        bool completed_ = false;
    };

    [[nodiscard]] Transaction begin_transaction() { return Transaction(*this); }

    bool undo();
    bool redo();
    void clear_history();

private:
    friend class Transaction;

    [[nodiscard]] bool in_bounds(int x, int y) const noexcept;
    [[nodiscard]] std::size_t index_of(int x, int y) const noexcept;
    bool commit_changes(std::vector<PixelChange> changes);

    CanvasLimits limits_;
    int width_ = 0;
    int height_ = 0;
    std::uint64_t revision_ = 0;
    std::vector<std::uint32_t> pixels_;
    std::vector<EditDelta> undo_stack_;
    std::vector<EditDelta> redo_stack_;
};

} // namespace pixelforge
