/* PC allocation accounting for the vendored SpeexDSP subset.
 *
 * os_support_custom.h (non-ESP branch) uses libc malloc/free but still calls
 * the accounting hooks speexdsp_mem_note_alloc()/speexdsp_mem_note_free()
 * that the ESP build implements in speexdsp_mem.c. On the PC this file
 * implements the hooks with a live-block ledger, which the test suite uses
 * to prove every SpeexDSP state init/destroy pair is leak-free and to report
 * the library's memory footprint. */
#include "pc_mem.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

struct Ledger {
    std::mutex mutex;
    std::unordered_map<void *, size_t> live;
    size_t live_bytes = 0;
    size_t peak_live_bytes = 0;
    size_t total_allocs = 0;
    size_t total_frees = 0;
    size_t total_alloc_bytes = 0;
};

Ledger &ledger() {
    static Ledger ledger;
    return ledger;
}

}  // namespace

extern "C" {

/* Hooks declared by os_support_custom.h. */
void speexdsp_mem_note_alloc(void *ptr, size_t size) {
    if (ptr == nullptr)
        return;
    Ledger &l = ledger();
    std::lock_guard<std::mutex> lock(l.mutex);
    l.live[ptr] = size;
    l.live_bytes += size;
    if (l.live_bytes > l.peak_live_bytes)
        l.peak_live_bytes = l.live_bytes;
    l.total_allocs++;
    l.total_alloc_bytes += size;
}

void speexdsp_mem_note_free(void *ptr) {
    if (ptr == nullptr)
        return;
    Ledger &l = ledger();
    std::lock_guard<std::mutex> lock(l.mutex);
    auto it = l.live.find(ptr);
    if (it != l.live.end()) {
        l.live_bytes -= it->second;
        l.live.erase(it);
    } else {
        l.live_bytes += 0;  // freeing an unknown pointer: ASan will catch real bugs
    }
    l.total_frees++;
}

}  // extern "C"

namespace pc_mem {

Stats stats() {
    Ledger &l = ledger();
    std::lock_guard<std::mutex> lock(l.mutex);
    Stats s;
    s.live_blocks = l.live.size();
    s.live_bytes = l.live_bytes;
    s.peak_live_bytes = l.peak_live_bytes;
    s.total_allocs = l.total_allocs;
    s.total_frees = l.total_frees;
    s.total_alloc_bytes = l.total_alloc_bytes;
    return s;
}

void reset() {
    Ledger &l = ledger();
    std::lock_guard<std::mutex> lock(l.mutex);
    l.live.clear();
    l.live_bytes = 0;
    l.peak_live_bytes = 0;
    l.total_allocs = 0;
    l.total_frees = 0;
    l.total_alloc_bytes = 0;
}

std::string leak_report(size_t max_entries) {
    Ledger &l = ledger();
    std::lock_guard<std::mutex> lock(l.mutex);
    std::string out;
    char buf[128];
    size_t shown = 0;
    for (const auto &entry : l.live) {
        std::snprintf(buf, sizeof(buf), "  leaked block %p: %zu bytes\n", entry.first, entry.second);
        out += buf;
        if (++shown >= max_entries) {
            out += "  ...\n";
            break;
        }
    }
    if (shown == 0)
        out = "  no leaked blocks\n";
    return out;
}

}  // namespace pc_mem
