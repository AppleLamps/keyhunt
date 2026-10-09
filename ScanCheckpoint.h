#ifndef SCAN_CHECKPOINT_H
#define SCAN_CHECKPOINT_H

#include "secp256k1/Int.h"
#include <deque>
#include <mutex>
#include <string>
#include <vector>

// A persistent allocator of disjoint, unit-stride key blocks. Allocations stay
// pending until their entire block has been scanned. Resume replays pending
// blocks before issuing new ones, allowing a different number of workers.
class ScanCheckpoint {
public:
  ScanCheckpoint(const std::string &path, const std::string &identity,
                 Int start, Int end, uint64_t block_keys, bool random);
  ~ScanCheckpoint();
  bool Acquire(Int &id, Int &base, uint64_t &keys);
  void Complete(Int id);
  void Flush();
  std::string Summary();

private:
  std::string path_, header_;
  Int start_, end_, blocks_, next_, next_block_, step_;
  uint64_t block_keys_;
  std::vector<Int> pending_;
  std::deque<Int> retry_;
  std::mutex mutex_;
  std::mutex save_mutex_;
#if defined(_WIN64) && !defined(__CYGWIN__)
  void *lock_ = nullptr;
#else
  int lock_ = -1;
#endif
  void Lock();
  void Unlock();
  void Load();
  void Save();
};

#endif
