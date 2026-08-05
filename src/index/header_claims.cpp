#include "index/header_claims.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

namespace cidx::index {
namespace {

using Clock = std::chrono::steady_clock;

// One granted header. The digest is retained so a later translation unit that
// parsed different bytes for the same path is not silently told the header is
// current -- it re-extracts, exactly as serial would after its content check
// fails.
struct Grant {
  std::optional<std::string> parsed_md5;
};

using ClaimKey = std::pair<std::string, std::string>;

} // namespace

class SequencedHeaderClaimOracle::Impl {
public:
  auto claim(std::size_t rank, std::string_view configuration_identity,
             const std::vector<HeaderClaimCandidate> &candidates)
      -> std::vector<bool> {
    std::vector<bool> owned(candidates.size(), false);
    {
      std::unique_lock lock(mutex_);
      const auto wait_started = Clock::now();
      // `>=` rather than `==` so a rank that was already released can never
      // wait forever; the runner's contract is one claim XOR one release per
      // translation unit, and this keeps a contract violation from deadlocking
      // the whole run.
      ready_.wait(lock, [&] { return cancelled_ || next_rank_ >= rank; });
      const double waited =
          std::chrono::duration<double>(Clock::now() - wait_started).count();
      metrics_.total_gate_wait_seconds += waited;
      metrics_.max_gate_wait_seconds =
          std::max(metrics_.max_gate_wait_seconds, waited);
      if (cancelled_) {
        return owned;
      }
      for (std::size_t i = 0; i < candidates.size(); ++i) {
        const HeaderClaimCandidate &candidate = candidates[i];
        ++metrics_.candidates;
        if (candidate.already_indexed_in_database) {
          ++metrics_.denied_already_indexed;
          continue;
        }
        const ClaimKey key{candidate.path,
                           std::string(configuration_identity)};
        const auto existing = granted_.find(key);
        if (existing != granted_.end() &&
            existing->second.parsed_md5 == candidate.parsed_md5) {
          ++metrics_.denied_in_flight_owner;
          continue;
        }
        granted_.insert_or_assign(key,
                                  Grant{.parsed_md5 = candidate.parsed_md5});
        owned[i] = true;
        ++metrics_.granted;
      }
      release(rank);
    }
    ready_.notify_all();
    return owned;
  }

  void release_unclaimed(std::size_t rank) {
    {
      const std::scoped_lock lock(mutex_);
      if (cancelled_ || rank < next_rank_) {
        return;
      }
      release(rank);
    }
    ready_.notify_all();
  }

  void cancel() {
    {
      const std::scoped_lock lock(mutex_);
      cancelled_ = true;
    }
    ready_.notify_all();
  }

  [[nodiscard]] auto metrics() const -> HeaderClaimMetrics {
    const std::scoped_lock lock(mutex_);
    return metrics_;
  }

private:
  // Advances the ordered gate past `rank` plus every already-released
  // higher rank, so a translation unit that will never claim -- a failed parse,
  // a crashed worker, a cache replay -- can never stall its successors.
  void release(std::size_t rank) {
    released_.insert(rank);
    while (released_.erase(next_rank_) != 0) {
      ++next_rank_;
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::map<ClaimKey, Grant> granted_;
  std::set<std::size_t> released_;
  std::size_t next_rank_ = 0;
  bool cancelled_ = false;
  HeaderClaimMetrics metrics_;
};

SequencedHeaderClaimOracle::SequencedHeaderClaimOracle()
    : impl_(std::make_unique<Impl>()) {}

SequencedHeaderClaimOracle::~SequencedHeaderClaimOracle() { impl_->cancel(); }

auto SequencedHeaderClaimOracle::claim(
    std::size_t rank, std::string_view configuration_identity,
    const std::vector<HeaderClaimCandidate> &candidates) -> std::vector<bool> {
  return impl_->claim(rank, configuration_identity, candidates);
}

void SequencedHeaderClaimOracle::release_unclaimed(std::size_t rank) {
  impl_->release_unclaimed(rank);
}

void SequencedHeaderClaimOracle::cancel() { impl_->cancel(); }

auto SequencedHeaderClaimOracle::metrics() const -> HeaderClaimMetrics {
  return impl_->metrics();
}

} // namespace cidx::index
