#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

#include "fgc_core.h"

namespace fgc {

struct EventRecord {
    FgcEventType type = FGC_EVENT_NONE;
    char hardwareId[FGC_HARDWARE_ID_MAX] = {};
};

// Bounded lock-free queue (Vyukov's MPMC design). Mapping threads push, the front end pops.
// A full queue drops the new event; events are rare and the capacity is generous.
class EventQueue {
public:
    EventQueue() {
        for (size_t i = 0; i < kCapacity; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    bool push(FgcEventType type, const char* hardwareId) noexcept {
        size_t position = enqueue_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[position % kCapacity];
            const size_t sequence = cell.sequence.load(std::memory_order_acquire);
            const intptr_t difference = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position);
            if (difference == 0) {
                if (enqueue_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                    cell.record.type = type;
                    strncpy_s(cell.record.hardwareId, sizeof(cell.record.hardwareId), hardwareId, _TRUNCATE);
                    cell.sequence.store(position + 1, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueue_.load(std::memory_order_relaxed);
            }
        }
    }

    bool pop(EventRecord& out) noexcept {
        size_t position = dequeue_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[position % kCapacity];
            const size_t sequence = cell.sequence.load(std::memory_order_acquire);
            const intptr_t difference = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position + 1);
            if (difference == 0) {
                if (dequeue_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                    out = cell.record;
                    cell.sequence.store(position + kCapacity, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = dequeue_.load(std::memory_order_relaxed);
            }
        }
    }

private:
    static constexpr size_t kCapacity = 64;

    struct Cell {
        std::atomic<size_t> sequence{0};
        EventRecord record;
    };

    std::array<Cell, kCapacity> cells_;
    std::atomic<size_t> enqueue_{0};
    std::atomic<size_t> dequeue_{0};
};

}  // namespace fgc
