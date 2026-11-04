#include "mapping_session.h"

#include <windows.h>

#include <avrt.h>

#include <array>
#include <bit>
#include <utility>

#include "pad_state.h"

namespace fgc {
namespace {

using KeyBits = std::array<uint64_t, kKeyCodeSpace / 64>;

uint32_t inputsHeldFor(const MappingTable& table, const KeyBits& keysDown) {
    uint32_t held = 0;
    for (size_t word = 0; word < keysDown.size(); ++word) {
        for (uint64_t bits = keysDown[word]; bits != 0; bits &= bits - 1) {
            held |= table.inputsForKey[word * 64 + static_cast<size_t>(std::countr_zero(bits))];
        }
    }
    return held;
}

}  // namespace

MappingSession::MappingSession(std::string hardwareId, std::unique_ptr<IKeyboardSession> keyboard,
                               std::unique_ptr<IVirtualPad> pad, const KeyArray& keys,
                               std::atomic<uint32_t>& runningCounter, EventQueue& events)
    : hardwareId_(std::move(hardwareId)),
      keyboard_(std::move(keyboard)),
      pad_(std::move(pad)),
      runningCounter_(runningCounter),
      events_(events),
      ownedTable_(std::make_unique<MappingTable>(MappingTable::fromKeys(keys))),
      table_(ownedTable_.get()) {
    ownedTable_->version = ++versionCounter_;
}

MappingSession::~MappingSession() {
    stop();
}

void MappingSession::markDisconnected() noexcept {
    disconnected_.store(true, std::memory_order_release);
    keyboard_->interrupt();
}

void MappingSession::setMapping(const KeyArray& keys) {
    auto next = std::make_unique<MappingTable>(MappingTable::fromKeys(keys));
    next->version = ++versionCounter_;
    table_.store(next.get(), std::memory_order_seq_cst);

    // A reader that entered before the store above may still be using the old table.
    const uint32_t epoch = readerEpoch_.load(std::memory_order_seq_cst);
    while ((epoch & 1u) != 0 && readerEpoch_.load(std::memory_order_seq_cst) == epoch) {
        std::this_thread::yield();
    }
    ownedTable_ = std::move(next);
}

void MappingSession::start() {
    runningCounter_.fetch_add(1, std::memory_order_relaxed);
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        runningCounter_.fetch_sub(1, std::memory_order_relaxed);
        throw;
    }
}

void MappingSession::stop() {
    if (thread_.joinable()) {
        keyboard_->interrupt();
        thread_.join();
    }
}

LatencySnapshot MappingSession::latency() const noexcept {
    LatencySnapshot snapshot;
    snapshot.samples = latencySamples_.load(std::memory_order_relaxed);
    snapshot.lastTicks = latencyLastTicks_.load(std::memory_order_relaxed);
    snapshot.maxTicks = latencyMaxTicks_.load(std::memory_order_relaxed);
    if (snapshot.samples > 0) {
        snapshot.averageTicks = latencySumTicks_.load(std::memory_order_relaxed) / snapshot.samples;
        snapshot.readAverageTicks = latencyReadSumTicks_.load(std::memory_order_relaxed) / snapshot.samples;
        snapshot.reportAverageTicks = latencyReportSumTicks_.load(std::memory_order_relaxed) / snapshot.samples;
    }
    snapshot.readMaxTicks = latencyReadMaxTicks_.load(std::memory_order_relaxed);
    snapshot.reportMaxTicks = latencyReportMaxTicks_.load(std::memory_order_relaxed);
    return snapshot;
}

void MappingSession::resetLatency() noexcept {
    latencyResets_.fetch_add(1, std::memory_order_relaxed);
    latencySamples_.store(0, std::memory_order_relaxed);
    latencySumTicks_.store(0, std::memory_order_relaxed);
    latencyLastTicks_.store(0, std::memory_order_relaxed);
    latencyMaxTicks_.store(0, std::memory_order_relaxed);
    latencyReadSumTicks_.store(0, std::memory_order_relaxed);
    latencyReadMaxTicks_.store(0, std::memory_order_relaxed);
    latencyReportSumTicks_.store(0, std::memory_order_relaxed);
    latencyReportMaxTicks_.store(0, std::memory_order_relaxed);
}

void MappingSession::run() noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // Highest MMCSS class; the thread blocks between keys, so its CPU reservation is never used up.
    DWORD taskIndex = 0;
    const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    uint32_t held = 0;
    uint32_t seenVersion = 0;
    KeyBits keysDown{};
    PadState submitted;
    KeyEvent event;
    uint64_t samples = 0;
    uint64_t sumTicks = 0;
    uint64_t maxTicks = 0;
    uint64_t readSumTicks = 0;
    uint64_t readMaxTicks = 0;
    uint64_t reportSumTicks = 0;
    uint64_t reportMaxTicks = 0;
    uint32_t seenResets = 0;
    uint32_t presses = 0;
    WaitResult ended = WaitResult::Interrupted;

    while ((ended = keyboard_->wait(event)) == WaitResult::Event) {
        const size_t key = event.keyCode;
        if (key >= kKeyCodeSpace) {
            continue;
        }
        const uint64_t bit = uint64_t{1} << (key & 63);
        // Holding a key repeats its press; only the first one counts.
        const bool newPress = event.down && (keysDown[key >> 6] & bit) == 0;
        presses += newPress ? 1u : 0u;
        if (event.down) {
            keysDown[key >> 6] |= bit;
        } else {
            keysDown[key >> 6] &= ~bit;
        }

        readerEpoch_.fetch_add(1, std::memory_order_seq_cst);
        const MappingTable& table = *table_.load(std::memory_order_seq_cst);
        if (table.version != seenVersion) {
            // Rebuilding from the keys physically down keeps a key pressed before the remap from sticking.
            seenVersion = table.version;
            held = inputsHeldFor(table, keysDown);
        } else {
            const uint32_t mask = table.inputsForKey[key];
            held = event.down ? (held | mask) : (held & ~mask);
        }
        readerEpoch_.fetch_add(1, std::memory_order_seq_cst);

        // A held-key change can leave the pad unchanged (a left+right tie, say); skip the driver call then.
        const PadState state = computePadState(held);
        if (state == submitted) {
            heldInputs_.value.store(held, std::memory_order_relaxed);
            heldInputs_.presses.store(presses, std::memory_order_relaxed);
            continue;
        }
        LARGE_INTEGER beforeReport;
        QueryPerformanceCounter(&beforeReport);
        if (pad_->submit(state) == FGC_OK) {
            submitted = state;

            LARGE_INTEGER done;
            QueryPerformanceCounter(&done);
            if (event.timestampQpc > 0) {
                const auto span = [](int64_t from, int64_t to) { return to > from ? static_cast<uint64_t>(to - from) : uint64_t{0}; };
                const uint64_t ticks = span(event.timestampQpc, done.QuadPart);
                const uint64_t readTicks = span(event.timestampQpc, event.readDoneQpc);
                const uint64_t reportTicks = span(beforeReport.QuadPart, done.QuadPart);
                const uint32_t resets = latencyResets_.load(std::memory_order_relaxed);
                if (resets != seenResets) {
                    seenResets = resets;
                    samples = sumTicks = maxTicks = 0;
                    readSumTicks = readMaxTicks = reportSumTicks = reportMaxTicks = 0;
                }
                ++samples;
                sumTicks += ticks;
                maxTicks = ticks > maxTicks ? ticks : maxTicks;
                readSumTicks += readTicks;
                readMaxTicks = readTicks > readMaxTicks ? readTicks : readMaxTicks;
                reportSumTicks += reportTicks;
                reportMaxTicks = reportTicks > reportMaxTicks ? reportTicks : reportMaxTicks;
                latencySamples_.store(samples, std::memory_order_relaxed);
                latencySumTicks_.store(sumTicks, std::memory_order_relaxed);
                latencyLastTicks_.store(ticks, std::memory_order_relaxed);
                latencyMaxTicks_.store(maxTicks, std::memory_order_relaxed);
                latencyReadSumTicks_.store(readSumTicks, std::memory_order_relaxed);
                latencyReadMaxTicks_.store(readMaxTicks, std::memory_order_relaxed);
                latencyReportSumTicks_.store(reportSumTicks, std::memory_order_relaxed);
                latencyReportMaxTicks_.store(reportMaxTicks, std::memory_order_relaxed);
            }
        }

        // Published only after the report went out, so showing held inputs never delays the controller.
        heldInputs_.value.store(held, std::memory_order_relaxed);
        heldInputs_.presses.store(presses, std::memory_order_relaxed);
    }

    heldInputs_.value.store(0, std::memory_order_relaxed);
    pad_->reset();

    // Keys still held would never get their release to Windows, since the driver swallowed it.
    uint16_t heldKeys[kKeyCodeSpace];
    size_t heldCount = 0;
    for (size_t word = 0; word < keysDown.size(); ++word) {
        for (uint64_t bits = keysDown[word]; bits != 0; bits &= bits - 1) {
            heldKeys[heldCount++] = static_cast<uint16_t>(word * 64 + static_cast<size_t>(std::countr_zero(bits)));
        }
    }
    keyboard_->releaseKeys(heldKeys, heldCount);

    if (ended == WaitResult::DeviceLost) {
        events_.push(FGC_EVENT_SESSION_FAULTED, hardwareId_.c_str());
    } else if (disconnected_.load(std::memory_order_acquire)) {
        events_.push(FGC_EVENT_KEYBOARD_DISCONNECTED, hardwareId_.c_str());
    }

    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
    finished_.store(true, std::memory_order_release);
    runningCounter_.fetch_sub(1, std::memory_order_relaxed);
}

}  // namespace fgc
