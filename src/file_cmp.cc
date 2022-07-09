#include "file_cmp.hh"

#include <algorithm>
#include <exception>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "config.hh"
#include "oss.hh"

namespace dedupe {

inline namespace detail_v1 {

// RAII wrapper for xxhash library.
class hasher_t {
  XXH3_state_t *_state;

 public:
  hasher_t() {
    _state = XXH3_createState();
    if (_state == nullptr) {
      throw std::runtime_error("XXH3_createState failed");
    }
    if (XXH3_128bits_reset_withSeed(_state, hash_seed) == XXH_ERROR) {
      throw std::runtime_error("XXH3_128bits_reset_withSeed failed");
    }
  }
  ~hasher_t() noexcept {
    if (_state != nullptr) {
      XXH3_freeState(_state);
    }
  }

  hasher_t(const hasher_t &rhs) = delete;
  hasher_t(hasher_t &&rhs) = delete;
  hasher_t &operator=(const hasher_t &rhs) = delete;
  hasher_t &operator=(hasher_t &&rhs) = delete;

  void update(const char *data, const uint64_t size) {
    if (XXH3_128bits_update(_state, data, size) == XXH_ERROR) {
      throw std::runtime_error("XXH3_128bits_update failed");
    }
  }
  XXH128_hash_t digest() noexcept { return XXH3_128bits_digest(_state); }
};

// memory manager for buffer.
class buf_man_t {
  std::unordered_map<std::thread::id, char *> buf_map;
  std::shared_mutex rw_lk;

 public:
  buf_man_t() = default;

  // this is dangerous, make sure no one is still using buffer
  void release_buf() noexcept {
    std::unique_lock lk(rw_lk);
    for (auto &buf : buf_map) {
      delete[] buf.second;
    }
    buf_map.clear();
  }
  char *get() {
    auto id = std::this_thread::get_id();
    char *buf = nullptr;
    {
      std::shared_lock lk(rw_lk);
      auto it = buf_map.find(id);
      if (it != buf_map.end()) {
        buf = it->second;
      }
    }
    if (buf == nullptr) {
      buf = new char[buf_sz];
      if (buf == nullptr) {
        throw std::runtime_error("allocation failed");
      }
      {
        std::unique_lock lk(rw_lk);
        buf_map.emplace(id, buf);
      }
    }
    return buf;
  }

  ~buf_man_t() noexcept { release_buf(); }
};
buf_man_t buf_man;

std::strong_ordering file_cmp_t::operator<=>(const file_cmp_t &rhs) const {
  // check for hard link equality
  if (_hard_link_cnt > 1 && _hard_link_cnt == rhs._hard_link_cnt &&
      std::filesystem::equivalent(_file_entry.path(), rhs._file_entry.path())) {
    return std::strong_ordering::equal;
  }
  // compare hashes
  std::strong_ordering cmp = std::strong_ordering::equal;
  for (auto i = 0U; i < _max_hash; ++i) {
    lazy_hash(i);
    rhs.lazy_hash(i);
    cmp = _file_hashes[i].high64 <=> rhs._file_hashes[i].high64;
    if (cmp != std::strong_ordering::equal) {
      break;
    }
    cmp = _file_hashes[i].low64 <=> rhs._file_hashes[i].low64;
    if (cmp != std::strong_ordering::equal) {
      break;
    }
  }
  close_file();
  rhs.close_file();
  return cmp;
}

void file_cmp_t::open_file() const noexcept {
  if (!_file_stream.is_open()) {
    _file_stream.open(_file_entry.path(), std::ios::binary);
    auto processed = _file_entry.size() - _remain_sz;
    _file_stream.seekg((int64_t)processed);
  }
}

void file_cmp_t::close_file() const noexcept {
  if (_file_stream.is_open()) {
    _file_stream.close();
  }
}

void file_cmp_t::lazy_hash(const uint32_t idx) const {
  if (idx < _file_hashes.size()) {
    return;
  }

  hasher_t hasher;
  open_file();

  auto blk_sz =
      std::min(idx == 0U ? hash_blk_sz : hash_blk_sz << (idx - 1U), _remain_sz);
  _remain_sz -= blk_sz;
  auto buf = buf_man.get();

  while (blk_sz > 0) {
    const auto read_sz = std::min(buf_sz, blk_sz);
    const auto read_len = _file_stream.read(buf, (int64_t)read_sz).gcount();
    if (read_sz != (uint64_t)read_len) {
      oss(std::cerr) << "[err] read error: " << _file_entry.path() << '\n';
      _file_hashes.resize(_max_hash);
      _remain_sz = 0;
      return;
    }
    hasher.update(buf, read_sz);
    blk_sz -= read_sz;
  }
  _file_hashes.emplace_back(hasher.digest());
}

}  // namespace detail_v1

}  // namespace dedupe