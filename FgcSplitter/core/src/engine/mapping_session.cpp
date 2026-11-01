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
                               std::atomic<uint32_t>& runningCounter)
    : hardwareId_(std::move(hardwareId)),
      keyboard_(std::move(keyboard)),
      pad_(std::move(pad)),
      runningCounter_(runningCounter),
      ownedTable_(std::make_unique<MappingTable>(MappingTable::fromKeys(keys))),
      table_(ownedTable_.get()) {
    ownedTable_->version = ++versionCounter_;
}

MappingSession::~MappingSession() {
    stop();
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

    while (keyboard_->wait(event) == WaitResult::Event) {
        const size_t key = event.keyCode;
        if (key >= kKeyCodeSpace) {
            continue;
        }
        const uint64_t bit = uint64_t{1} << (key & 63);
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
            continue;
        }
        if (pad_->submit(state) == FGC_OK) {
            submitted = state;
        }
    }

    pad_->reset();

    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
    finished_.store(true, std::memory_order_release);
    runningCounter_.fetch_sub(1, std::memory_order_relaxed);
}

}  // namespace fgc
