#pragma once

namespace dedupe {

// 512B
constexpr auto hash_blk_sz = 512UL;
// 16MiB
constexpr auto max_buf_sz = 16UL * 1024UL * 1024UL;

constexpr auto hash_seed = 0x178ee47c0190226cUL;

}  // namespace dedupe