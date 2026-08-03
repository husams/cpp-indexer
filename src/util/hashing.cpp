#include "util/hashing.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

extern "C" {
#include "md5/md5.h"   // vendored public-domain RFC 1321 implementation (D4)
#include "sha1/sha1.h" // vendored public-domain SHA-1 (ADR-006 M5)
}

namespace cidx {

namespace {

std::string digest_to_hex(std::span<const unsigned char> digest) {
  static constexpr std::array kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string out;
  out.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    out += kHex[byte >> 4];
    out += kHex[byte & 0x0F];
  }
  return out;
}

std::string digest_to_hex_32(const std::array<std::uint32_t, 8> &digest) {
  static constexpr std::array kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string out;
  out.reserve(64);
  for (const auto word : digest) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      out.push_back(kHex[(word >> shift) & 0x0fU]);
    }
  }
  return out;
}

class Sha256 {
public:
  void update(const void *data, std::size_t len) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    bit_count_ += static_cast<std::uint64_t>(len) * 8U;
    while (len > 0) {
      const auto count = std::min(len, block_.size() - buffered_);
      std::memcpy(block_.data() + buffered_, bytes, count);
      buffered_ += count;
      bytes += count;
      len -= count;
      if (buffered_ == block_.size()) {
        transform(block_.data());
        buffered_ = 0;
      }
    }
  }

  std::array<std::uint32_t, 8> final() {
    block_[buffered_++] = 0x80;
    if (buffered_ > 56) {
      std::fill(block_.begin() + static_cast<std::ptrdiff_t>(buffered_),
                block_.end(), 0);
      transform(block_.data());
      buffered_ = 0;
    }
    std::fill(block_.begin() + static_cast<std::ptrdiff_t>(buffered_),
              block_.begin() + 56, 0);
    for (int shift = 56; shift >= 0; shift -= 8) {
      block_[56 + static_cast<std::size_t>((56 - shift) / 8)] =
          static_cast<unsigned char>((bit_count_ >> shift) & 0xffU);
    }
    transform(block_.data());
    return state_;
  }

private:
  static constexpr std::array<std::uint32_t, 64> kRoundConstants = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

  static constexpr std::uint32_t rotate_right(std::uint32_t value, int shift) {
    return (value >> shift) | (value << (32 - shift));
  }

  void transform(const unsigned char *block) {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t i = 0; i < 16; ++i) {
      const auto offset = i * 4;
      schedule[i] = (static_cast<std::uint32_t>(block[offset]) << 24) |
                    (static_cast<std::uint32_t>(block[offset + 1]) << 16) |
                    (static_cast<std::uint32_t>(block[offset + 2]) << 8) |
                    static_cast<std::uint32_t>(block[offset + 3]);
    }
    for (std::size_t i = 16; i < schedule.size(); ++i) {
      const auto s0 = rotate_right(schedule[i - 15], 7) ^
                      rotate_right(schedule[i - 15], 18) ^
                      (schedule[i - 15] >> 3);
      const auto s1 = rotate_right(schedule[i - 2], 17) ^
                      rotate_right(schedule[i - 2], 19) ^
                      (schedule[i - 2] >> 10);
      schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }
    auto working = state_;
    for (std::size_t i = 0; i < schedule.size(); ++i) {
      const auto s1 = rotate_right(working[4], 6) ^
                      rotate_right(working[4], 11) ^
                      rotate_right(working[4], 25);
      const auto choose =
          (working[4] & working[5]) ^ ((~working[4]) & working[6]);
      const auto temp1 =
          working[7] + s1 + choose + kRoundConstants[i] + schedule[i];
      const auto s0 = rotate_right(working[0], 2) ^
                      rotate_right(working[0], 13) ^
                      rotate_right(working[0], 22);
      const auto majority = (working[0] & working[1]) ^
                            (working[0] & working[2]) ^
                            (working[1] & working[2]);
      const auto temp2 = s0 + majority;
      working[7] = working[6];
      working[6] = working[5];
      working[5] = working[4];
      working[4] = working[3] + temp1;
      working[3] = working[2];
      working[2] = working[1];
      working[1] = working[0];
      working[0] = temp1 + temp2;
    }
    for (std::size_t i = 0; i < state_.size(); ++i) {
      state_[i] += working[i];
    }
  }

  std::array<std::uint32_t, 8> state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                         0xa54ff53a, 0x510e527f, 0x9b05688c,
                                         0x1f83d9ab, 0x5be0cd19};
  std::array<unsigned char, 64> block_{};
  std::size_t buffered_ = 0;
  std::uint64_t bit_count_ = 0;
};

} // namespace

std::string md5_hex(const void *data, std::size_t len) {
  MD5_CTX ctx;
  MD5_Init(&ctx);
  MD5_Update(&ctx, data, static_cast<unsigned long>(len));
  std::array<unsigned char, 16> digest{};
  MD5_Final(digest.data(), &ctx);
  return digest_to_hex(digest);
}

std::string md5_hex(const std::string &data) {
  return md5_hex(data.data(), data.size());
}

std::optional<std::string> md5_of(const std::string &path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  MD5_CTX ctx;
  MD5_Init(&ctx);
  std::array<char, 65536> buf{};
  for (;;) {
    input.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto count = input.gcount();
    if (count > 0) {
      MD5_Update(&ctx, buf.data(), static_cast<unsigned long>(count));
    }
    if (input.eof()) {
      break;
    }
    if (!input) {
      return std::nullopt;
    }
  }
  std::array<unsigned char, 16> digest{};
  MD5_Final(digest.data(), &ctx);
  return digest_to_hex(digest);
}

// ---------------------------------------------------------------------------
// SHA-1 (ADR-006 M5) — byte-identical to Python hashlib.sha1

std::string sha1_hex(const std::string &data) {
  SHA1_CTX ctx;
  SHA1_Init(&ctx);
  SHA1_Update(&ctx, data.data(), static_cast<unsigned long>(data.size()));
  std::array<unsigned char, 20> digest{};
  SHA1_Final(digest.data(), &ctx);
  return digest_to_hex(digest);
}

std::string sha256_hex(const std::string &data) {
  Sha256 context;
  context.update(data.data(), data.size());
  return "sha256:" + digest_to_hex_32(context.final());
}

std::string sha256_hex(const std::span<const std::byte> data) {
  Sha256 context;
  context.update(data.data(), data.size());
  return "sha256:" + digest_to_hex_32(context.final());
}

std::optional<std::string> sha256_of(const std::string &path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  Sha256 context;
  std::array<char, 65536> buffer{};
  for (;;) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      context.update(buffer.data(), static_cast<std::size_t>(count));
    }
    if (input.eof()) {
      break;
    }
    if (!input) {
      return std::nullopt;
    }
  }
  return "sha256:" + digest_to_hex_32(context.final());
}

std::optional<std::string> sha256_of_fd(const int fd) {
  struct stat file_stat{};
  if (fd < 0 || ::fstat(fd, &file_stat) != 0 || !S_ISREG(file_stat.st_mode) ||
      file_stat.st_size < 0) {
    return std::nullopt;
  }
  Sha256 context;
  std::array<char, 65536> buffer{};
  std::size_t offset = 0;
  const auto size = static_cast<std::size_t>(file_stat.st_size);
  while (offset < size) {
    const ssize_t count =
        ::pread(fd, buffer.data(), buffer.size(), static_cast<off_t>(offset));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return std::nullopt;
    }
    context.update(buffer.data(), static_cast<std::size_t>(count));
    offset += static_cast<std::size_t>(count);
  }
  return "sha256:" + digest_to_hex_32(context.final());
}

std::optional<std::string> sha256_of_fd_prefix(const int fd,
                                               const std::uint64_t byte_count) {
  struct stat file_stat{};
  if (fd < 0 || ::fstat(fd, &file_stat) != 0 || !S_ISREG(file_stat.st_mode) ||
      file_stat.st_size < 0 || std::cmp_less(file_stat.st_size, byte_count)) {
    return std::nullopt;
  }
  Sha256 context;
  std::array<char, 65536> buffer{};
  std::uint64_t offset = 0;
  while (offset < byte_count) {
    const auto remaining = byte_count - offset;
    const auto request = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, buffer.size()));
    const ssize_t count =
        ::pread(fd, buffer.data(), request, static_cast<off_t>(offset));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return std::nullopt;
    }
    context.update(buffer.data(), static_cast<std::size_t>(count));
    offset += static_cast<std::uint64_t>(count);
  }
  return "sha256:" + digest_to_hex_32(context.final());
}

// Helper: append flags (joined by \0) and optional driver to SHA-1 context.
static void sha1_add_flags(SHA1_CTX &ctx, const std::vector<std::string> &flags,
                           const std::optional<std::string> &driver) {
  for (std::size_t i = 0; i < flags.size(); ++i) {
    if (i > 0) {
      const char sep = '\0';
      SHA1_Update(&ctx, &sep, 1);
    }
    SHA1_Update(&ctx, flags[i].data(),
                static_cast<unsigned long>(flags[i].size()));
  }
  if (driver) {
    // Python: b"\0drv\0" + driver.encode()
    constexpr std::string_view prefix("\0drv\0", 5);
    SHA1_Update(&ctx, prefix.data(), static_cast<unsigned long>(prefix.size()));
    SHA1_Update(&ctx, driver->data(),
                static_cast<unsigned long>(driver->size()));
  }
}

std::string sha1_cache_key(const AstCacheKey &k) {
  SHA1_CTX ctx;
  SHA1_Init(&ctx);
  // abspath + "\0"
  SHA1_Update(&ctx, k.abspath.data(),
              static_cast<unsigned long>(k.abspath.size()));
  const char sep = '\0';
  SHA1_Update(&ctx, &sep, 1);
  // flags [+ "\0drv\0" + driver]
  sha1_add_flags(ctx, k.flags, k.driver);
  std::array<unsigned char, 20> digest{};
  SHA1_Final(digest.data(), &ctx);
  return digest_to_hex(digest);
}

std::string sha1_flags_hash(const AstCacheKey &k) {
  SHA1_CTX ctx;
  SHA1_Init(&ctx);
  sha1_add_flags(ctx, k.flags, k.driver);
  std::array<unsigned char, 20> digest{};
  SHA1_Final(digest.data(), &ctx);
  return digest_to_hex(digest);
}

} // namespace cidx
