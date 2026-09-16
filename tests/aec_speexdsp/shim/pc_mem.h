/* Live-allocation ledger API for the PC SpeexDSP test suite (see pc_mem.cpp). */
#pragma once

#include <cstddef>
#include <string>

namespace pc_mem {

struct Stats {
    size_t live_blocks = 0;
    size_t live_bytes = 0;
    size_t peak_live_bytes = 0;
    size_t total_allocs = 0;
    size_t total_frees = 0;
    size_t total_alloc_bytes = 0;
};

Stats stats();
void reset();
std::string leak_report(size_t max_entries = 16);

}  // namespace pc_mem
