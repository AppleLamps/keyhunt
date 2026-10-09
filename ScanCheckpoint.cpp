#include "ScanCheckpoint.h"
#include "hash/sha256.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

#if defined(_WIN64) && !defined(__CYGWIN__)
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {
std::string Hex(Int value) {
  char *raw = value.GetBase16();
  std::string result(raw);
  free(raw);
  if (result.size() < 64) result.insert(0, 64 - result.size(), '0');
  return result;
}

bool ValidHex(const std::string &text) {
  return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string Digest(const std::string &text) {
  uint8_t hash[32];
  sha256((uint8_t *)text.data(), text.size(), hash);
  return sha256_hex(hash);
}

[[noreturn]] void Error(const std::string &message) {
  throw std::runtime_error("checkpoint: " + message);
}

Int ReadInt(std::istream &input) {
  std::string text;
  if (!(input >> text) || !ValidHex(text)) Error("invalid integer in saved state");
  Int result;
  result.SetBase16(text.c_str());
  return result;
}
} // namespace

ScanCheckpoint::ScanCheckpoint(const std::string &path, const std::string &identity,
                               Int start, Int end, uint64_t block_keys, bool random)
    : path_(path), start_(start), end_(end), block_keys_(block_keys) {
  if (!ValidHex(identity) || !start.IsLower(&end) || block_keys < 1024 || block_keys % 1024)
    Error("invalid scan configuration");
  // Range end is exclusive. Division is done only once, outside the hot loop.
  blocks_.Sub(&end, &start);
  blocks_.Add(block_keys - 1);
  Int divisor(block_keys);
  blocks_.Div(&divisor);
  header_ = "KEYHUNT_SCAN_V1\nidentity " + identity +
            "\nrange_start " + Hex(start) + "\nrange_end " + Hex(end) +
            "\nblock_keys " + std::to_string(block_keys) +
            "\norder " + (random ? "random" : "sequential") + "\n";
  Lock();
  try {
    FILE *probe = fopen(path_.c_str(), "rb");
    if (probe) {
      fclose(probe);
      Load();
    } else {
      if (errno != ENOENT) Error("cannot read " + path_ + ": " + strerror(errno));
      next_.SetInt32(0);
      next_block_.SetInt32(0);
      step_.SetInt32(1);
      if (random && !blocks_.IsOne()) {
        Int zero(uint64_t(0)), gcd;
        next_block_.Rand(&zero, &blocks_);
        // A step coprime to the block count visits every block exactly once.
        // Advance by addition modulo blocks_: no wide multiplication per job.
        do {
          step_.Rand(&zero, &blocks_);
          gcd.Set(&step_);
          gcd.GCD(&blocks_);
        } while (!gcd.IsOne());
      }
      Save();
    }
    for (const Int &id : pending_) retry_.push_back(id);
  } catch (...) {
    Unlock();
    throw;
  }
}

ScanCheckpoint::~ScanCheckpoint() { Unlock(); }

void ScanCheckpoint::Lock() {
#if defined(_WIN64) && !defined(__CYGWIN__)
  lock_ = CreateFileA((path_ + ".lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                      0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (lock_ == INVALID_HANDLE_VALUE) {
    lock_ = nullptr;
    Error("cannot lock " + path_ + " (another scanner may be using it)");
  }
#else
  lock_ = open((path_ + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
  if (lock_ < 0) Error("cannot open lock for " + path_ + ": " + strerror(errno));
  if (flock(lock_, LOCK_EX | LOCK_NB) != 0) {
    Unlock();
    Error("cannot lock " + path_ + " (another scanner may be using it)");
  }
#endif
}

void ScanCheckpoint::Unlock() {
#if defined(_WIN64) && !defined(__CYGWIN__)
  if (lock_) CloseHandle(lock_);
  lock_ = nullptr;
#else
  if (lock_ >= 0) close(lock_);
  lock_ = -1;
#endif
}

void ScanCheckpoint::Load() {
  std::ifstream input(path_, std::ios::binary | std::ios::ate);
  if (!input || input.tellg() < 0 || input.tellg() > 8 * 1024 * 1024)
    Error("cannot read saved state or file is too large");
  std::string data((size_t)input.tellg(), '\0');
  input.seekg(0);
  if (!input.read(&data[0], data.size())) Error("cannot read saved state");
  size_t checksum = data.rfind("sha256 ");
  if (checksum == std::string::npos || data.substr(checksum) != "sha256 " + Digest(data.substr(0, checksum)) + "\n")
    Error("saved state checksum failed; refusing to skip any keys");
  if (data.compare(0, header_.size(), header_) != 0)
    Error("saved target, range or scan settings differ; use the original settings or a different -P file");
  std::istringstream state(data.substr(header_.size(), checksum - header_.size()));
  auto field = [&](const char *name) {
    std::string label;
    if (!(state >> label) || label != name) Error("invalid saved state field");
    return ReadInt(state);
  };
  next_ = field("next");
  next_block_ = field("next_block");
  step_ = field("step");
  std::string label;
  size_t count;
  if (!(state >> label >> count) || label != "pending" || count > 100000)
    Error("invalid pending block count");
  if (next_.IsGreater(&blocks_) || !next_block_.IsLower(&blocks_) || step_.IsZero())
    Error("invalid saved scan position");
  if (!blocks_.IsOne()) {
    Int gcd(step_);
    gcd.GCD(&blocks_);
    if (!step_.IsLower(&blocks_) || !gcd.IsOne()) Error("invalid saved random traversal");
  } else if (!step_.IsOne()) Error("invalid single-block traversal");
  Int pending_count((uint64_t)count);
  if (pending_count.IsGreater(&next_)) Error("pending count exceeds issued blocks");
  for (size_t i = 0; i < count; i++) {
    Int id = ReadInt(state);
    if (!id.IsLower(&blocks_)) Error("pending block is outside the range");
    for (Int &prior : pending_) if (id.IsEqual(&prior)) Error("duplicate pending block");
    pending_.push_back(id);
  }
  if (state >> label) Error("unexpected saved state data");
}

void ScanCheckpoint::Save() {
  std::string body;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    body = header_ + "next " + Hex(next_) + "\nnext_block " + Hex(next_block_) +
           "\nstep " + Hex(step_) + "\npending " + std::to_string(pending_.size()) + "\n";
    for (const Int &id : pending_) body += Hex(id) + "\n";
  }
  // Workers keep scanning while the main thread performs durable disk I/O.
  std::string data = body + "sha256 " + Digest(body) + "\n";
  std::string temporary = path_ + ".tmp";
  FILE *file = fopen(temporary.c_str(), "wb");
  if (!file) Error("cannot write " + temporary + ": " + strerror(errno));
  bool ok = fwrite(data.data(), 1, data.size(), file) == data.size() && fflush(file) == 0;
#if defined(_WIN64) && !defined(__CYGWIN__)
  if (ok) ok = _commit(_fileno(file)) == 0;
#else
  if (ok) ok = fsync(fileno(file)) == 0;
#endif
  if (fclose(file) != 0) ok = false;
  if (!ok) Error("cannot flush " + temporary + "; previous checkpoint retained");
#if defined(_WIN64) && !defined(__CYGWIN__)
  if (!MoveFileExA(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    Error("cannot replace " + path_);
#else
  if (rename(temporary.c_str(), path_.c_str()) != 0)
    Error("cannot replace " + path_ + ": " + strerror(errno));
  size_t slash = path_.find_last_of('/');
  std::string directory = slash == std::string::npos ? "." : path_.substr(0, slash + 1);
  int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd >= 0) {
    // Some WSL/filesystem combinations do not implement directory fsync.
    int result = fsync(fd), error = errno;
    close(fd);
    if (result != 0 && error != EINVAL && error != ENOTSUP)
      Error("cannot flush checkpoint directory: " + std::string(strerror(error)));
  }
#endif
}

bool ScanCheckpoint::Acquire(Int &id, Int &base, uint64_t &keys) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!retry_.empty()) {
    id = retry_.front();
    retry_.pop_front();
  } else {
    if (!next_.IsLower(&blocks_)) return false;
    id.Set(&next_block_);
    next_.AddOne();
    next_block_.Add(&step_);
    if (next_block_.IsGreaterOrEqual(&blocks_)) next_block_.Sub(&blocks_);
    pending_.push_back(id);
  }
  base.Set(&id);
  base.Mult(block_keys_);
  base.Add(&start_);
  Int remaining;
  remaining.Sub(&end_, &base);
  Int limit(block_keys_);
  keys = remaining.IsLower(&limit) ? remaining.GetInt64() : block_keys_;
  return true;
}

void ScanCheckpoint::Complete(Int id) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto found = std::find_if(pending_.begin(), pending_.end(), [&](Int &value) { return id.IsEqual(&value); });
  if (found == pending_.end()) Error("completed block was not assigned");
  pending_.erase(found);
}

void ScanCheckpoint::Flush() {
  std::lock_guard<std::mutex> guard(save_mutex_);
  Save();
}

std::string ScanCheckpoint::Summary() {
  std::lock_guard<std::mutex> guard(mutex_);
  Int completed(next_);
  completed.Sub((uint64_t)pending_.size());
  char *done = completed.GetBase10(), *total = blocks_.GetBase10();
  std::string result = std::string(done) + "/" + total + " blocks complete, " +
                       std::to_string(pending_.size()) + " unfinished; " + path_;
  free(done);
  free(total);
  return result;
}
