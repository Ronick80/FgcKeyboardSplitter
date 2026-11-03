#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "backends/keyboard_backend.h"
#include "backends/pad_backend.h"
#include "event_queue.h"
#include "mapping_table.h"

namespace fgc {

// Values are QueryPerformanceCounter ticks. Total is wake to the controller accepting the report; it splits
// into the keyboard driver read and the virtual controller call (the rest is the app's own few microseconds).
struct LatencySnapshot {
    uint64_t samples = 0;
    uint64_t lastTicks = 0;
    uint64_t averageTicks = 0;
    uint64_t maxTicks = 0;
    uint64_t readAverageTicks = 0;
    uint64_t readMaxTicks = 0;
    uint64_t reportAverageTicks = 0;
    uint64_t reportMaxTicks = 0;
};

// One keyboard feeding one virtual pad on its own high-priority thread.
class MappingSession {
public:
    // runningCounter is incremented by start() and decremented when the thread exits. Events about an
    // unplanned end of the session are pushed to events.
    MappingSession(std::string hardwareId, std::unique_ptr<IKeyboardSession> keyboard,
                   std::unique_ptr<IVirtualPad> pad, const KeyArray& keys,
                   std::atomic<uint32_t>& runningCounter, EventQueue& events);
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

    // False once the keyboard was unplugged.
    bool keyboardConnected() const noexcept {
        return keyboard_->connected();
    }

    // Ends the session as an unplanned disconnect: the thread exits, resets the pad, and reports it.
    void markDisconnected() noexcept;

    std::chrono::steady_clock::time_point startedAt() const noexcept {
        return startedAt_;
    }

    // Set once Windows was seen listing this session's pad. Control plane only (the context's control lock).
    void markPadVisible() noexcept {
        padVisible_ = true;
    }
    bool padVisible() const noexcept {
        return padVisible_;
    }

    // The inputs whose keys are held right now, one bit per FgcXboxInput. Lock-free; safe while the thread runs.
    uint32_t heldInputs() const noexcept {
        return heldInputs_.value.load(std::memory_order_relaxed);
    }

    // Lock-free; safe while the thread runs.
    LatencySnapshot latency() const noexcept;
    void resetLatency() noexcept;

private:
    void run() noexcept;

    std::string hardwareId_;
    std::unique_ptr<IKeyboardSession> keyboard_;
    std::unique_ptr<IVirtualPad> pad_;
    std::atomic<uint32_t>& runningCounter_;
    EventQueue& events_;
    std::atomic<bool> finished_{false};
    std::atomic<bool> disconnected_{false};
    const std::chrono::steady_clock::time_point startedAt_ = std::chrono::steady_clock::now();
    bool padVisible_ = false;

    // Written only by the mapping thread (and zeroed by resetLatency()); read by latency().
    std::atomic<uint64_t> latencySamples_{0};
    std::atomic<uint64_t> latencySumTicks_{0};
    std::atomic<uint64_t> latencyLastTicks_{0};
    std::atomic<uint64_t> latencyMaxTicks_{0};
    std::atomic<uint64_t> latencyReadSumTicks_{0};
    std::atomic<uint64_t> latencyReadMaxTicks_{0};
    std::atomic<uint64_t> latencyReportSumTicks_{0};
    std::atomic<uint64_t> latencyReportMaxTicks_{0};
    std::atomic<uint32_t> latencyResets_{0};  // the thread restarts its own totals when this changes

    // Written by the mapping thread after each report, read by a UI that shows held inputs. It has a cache line
    // to itself, so those reads never touch the line the thread's other per-event state lives on.
#pragma warning(push)
#pragma warning(disable : 4324)  // padded to the cache line on purpose
    struct alignas(64) HeldInputs {
        std::atomic<uint32_t> value{0};
    };
    HeldInputs heldInputs_;
#pragma warning(pop)

    // The thread reads table_ inside an odd/even readerEpoch_ window; setMapping() frees the
    // old table only after that window has closed.
    std::unique_ptr<MappingTable> ownedTable_;
    std::atomic<const MappingTable*> table_;
    std::atomic<uint32_t> readerEpoch_{0};
    uint32_t versionCounter_ = 0;

    std::thread thread_;  // last member so it is destroyed first
};

}  // namespace fgc
