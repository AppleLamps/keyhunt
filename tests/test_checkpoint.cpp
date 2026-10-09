#include "../ScanCheckpoint.h"
#include "../secp256k1/SECP256k1.h"
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
static const std::string identity(64, 'a');

static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}

template <typename F> static void rejects(F f, const char *message) {
  bool rejected = false;
  try { f(); } catch (const std::exception &) { rejected = true; }
  require(rejected, message);
}

static void coverage(const fs::path &directory, bool random, uint64_t blocks) {
  std::string path = (directory / ("coverage-" + std::to_string(random) + "-" + std::to_string(blocks))).string();
  Int start(uint64_t(1)), end(uint64_t(1 + blocks * 1024 - 5));
  std::set<uint64_t> done;
  std::vector<uint64_t> unfinished;
  {
    ScanCheckpoint state(path, identity, start, end, 1024, random);
    for (uint64_t i = 0; i < std::min(blocks, uint64_t(4)); i++) {
      Int id, base;
      uint64_t keys;
      require(state.Acquire(id, base, keys), "missing initial job");
      uint64_t index = id.GetInt64();
      require(base.GetInt64() == 1 + index * 1024, "incorrect block base");
      require(keys == (index + 1 == blocks ? 1019 : 1024), "incorrect tail length");
      if (i % 2 == 0) unfinished.push_back(index);
      else { state.Complete(id); state.Flush(); done.insert(index); }
    }
  }
  {
    // Interrupted jobs are replayed, completed jobs are never returned again.
    ScanCheckpoint state(path, identity, start, end, 1024, random);
    Int id, base;
    uint64_t keys;
    std::set<uint64_t> replayed;
    while (state.Acquire(id, base, keys)) {
      uint64_t index = id.GetInt64();
      require(index < blocks && done.insert(index).second, "completed or invalid job returned");
      replayed.insert(index);
      state.Complete(id);
    }
    state.Flush();
    for (uint64_t index : unfinished) require(replayed.count(index), "unfinished block skipped");
  }
  require(done.size() == blocks, "range not fully covered");
  ScanCheckpoint complete(path, identity, start, end, 1024, random);
  Int id, base;
  uint64_t keys;
  require(!complete.Acquire(id, base, keys), "finished range was rescanned");
}

int main() {
  fs::path directory = fs::temp_directory_path() / ("keyhunt-checkpoint-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directory(directory);
  try {
    Secp256K1 secp;
    secp.Init();
    rseed(71);
    for (bool random : {false, true})
      for (uint64_t blocks : {1, 2, 7, 16, 30, 83}) coverage(directory, random, blocks);

    std::string path = (directory / "concurrent").string();
    Int start(uint64_t(1)), end(uint64_t(1 + 127 * 1024));
    {
      ScanCheckpoint state(path, identity, start, end, 1024, true);
      rejects([&] { ScanCheckpoint other(path, identity, start, end, 1024, true); }, "second scanner accepted same checkpoint");
      std::mutex mutex;
      std::set<uint64_t> assigned;
      bool duplicated = false;
      std::vector<std::thread> workers;
      for (int t = 0; t < 8; t++) workers.emplace_back([&] {
        Int id, base;
        uint64_t keys;
        while (state.Acquire(id, base, keys)) {
          {
            std::lock_guard<std::mutex> guard(mutex);
            if (!assigned.insert(id.GetInt64()).second) duplicated = true;
          }
          std::this_thread::yield();
          state.Complete(id);
        }
      });
      for (auto &worker : workers) worker.join();
      require(!duplicated && assigned.size() == 127, "concurrent coverage failed");
      state.Flush();
    }
    rejects([&] { ScanCheckpoint changed(path, std::string(64, 'b'), start, end, 1024, true); }, "target mismatch accepted");
    rejects([&] { ScanCheckpoint changed(path, identity, start, end, 2048, true); }, "block-size mismatch accepted");
    rejects([&] { ScanCheckpoint changed(path, identity, start, end, 1024, false); }, "order mismatch accepted");
    { std::ofstream corrupt(path); corrupt << "truncated checkpoint"; }
    rejects([&] { ScanCheckpoint broken(path, identity, start, end, 1024, true); }, "corrupt checkpoint accepted");

    path = (directory / "write-failure").string();
    {
      ScanCheckpoint state(path, identity, start, end, 1024, false);
      Int id, base;
      uint64_t keys;
      require(state.Acquire(id, base, keys), "missing write-failure job");
      fs::create_directory(path + ".tmp");
      state.Complete(id);
      rejects([&] { state.Flush(); }, "checkpoint write failure ignored");
      fs::remove(path + ".tmp");
    }
    {
      std::ofstream torn(path + ".tmp"); torn << "interrupted replacement"; torn.close();
      ScanCheckpoint state(path, identity, start, end, 1024, false);
      Int id, base;
      uint64_t keys;
      require(state.Acquire(id, base, keys) && id.IsZero(), "failed save skipped work");
    }

    // Puzzle 71 and a near-full 256-bit range exercise positions beyond u64.
    for (int bits : {71, 255}) {
      Int large_start(uint64_t(1)), large_end(uint64_t(1));
      large_start.ShiftL(bits - 1); large_end.ShiftL(bits);
      path = (directory / ("large-" + std::to_string(bits))).string();
      ScanCheckpoint state(path, identity, large_start, large_end, 0x1000000, true);
      std::set<std::string> ids;
      for (int i = 0; i < 16; i++) {
        Int id, base;
        uint64_t keys;
        require(state.Acquire(id, base, keys), "missing large-range block");
        require(base.IsGreaterOrEqual(&large_start) && base.IsLower(&large_end), "large block outside range");
        require(keys == 0x1000000, "incorrect large block size");
        char *hex = id.GetBase16();
        bool fresh = ids.insert(hex).second;
        free(hex);
        require(fresh, "large-range duplicate");
        state.Complete(id);
      }
    }
    fs::remove_all(directory);
    puts("[test_checkpoint] OK (resume, random coverage, concurrency, locking, corruption, failed saves, wide ranges)");
    return 0;
  } catch (const std::exception &error) {
    fprintf(stderr, "[test_checkpoint] FAILED: %s\n", error.what());
    fs::remove_all(directory);
    return 1;
  }
}
