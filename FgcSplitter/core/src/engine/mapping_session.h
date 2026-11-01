#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "backends/keyboard_backend.h"
#include "backends/pad_backend.h"
#include "mapping_table.h"

namespace fgc {

// One keyboard feeding one virtual pad on its own high-priority thread.
class MappingSession {
public:
    // runningCounter is incremented by start() and decremented when the thread exits.
    MappingSession(std::string hardwareId, std::unique_ptr<IKeyboardSession> keyboard,
                   std::unique_ptr<IVirtualPad> pad, const KeyArray& keys,
                   std::atomic<uint32_t>& runningCounter);
    ~MappingSession();

    MappingSession(const MappingSession&) = delete;
    MappingSession& operator=(const MappingSession&) = delete;

    void start();

    // Blocks until the thread has exited. The pad is left neutral.
    void stop();

    // Swaps in a new mapping without ever making the mapping thread wait. Callers must not
    // overlap calls with each other (the context's control lock does that).
    void setMapping(const KeyArray& keys);

    // True once the thread has exited, including when the keyboard disappeared.
    bool finished() const noexcept {
        return finished_.load(std::memory_order_acquire);
    }

private:
    void run() noexcept;

    std::string hardwareId_;
    std::unique_ptr<IKeyboardSession> keyboard_;
    std::unique_ptr<IVirtualPad> pad_;
    std::atomic<uint32_t>& runningCounter_;
    std::atomic<bool> finished_{false};

    // The thread reads table_ inside an odd/even readerEpoch_ window; setMapping() frees the
    // old table only after that window has closed.
    std::unique_ptr<MappingTable> ownedTable_;
    std::atomic<const MappingTable*> table_;
    std::atomic<uint32_t> readerEpoch_{0};
    uint32_t versionCounter_ = 0;

    std::thread thread_;  // last member so it is destroyed first
};

}  // namespace fgc
