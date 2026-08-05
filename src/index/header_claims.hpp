// Scheduler-owned deterministic owned-header assignment for parallel
// translation-unit extraction (S-074).
//
// Serial indexing amortises a shared header by side effect: the first
// translation unit to reach it indexes it, and every later translation unit
// reads the committed row and counts it "already". Concurrent extraction
// destroys that, because every in-flight worker reads the same pre-publication
// state and would extract the same header K times -- the shard-style
// duplication this story explicitly rejects.
//
// This oracle restores the serial contract without a second benchmark of work:
// the scheduler, not the worker, decides ownership, and it decides in the exact
// legacy apply order `(component.path, directory.path, file.name)`. A worker
// asks once, after its parse, and blocks until every lower-ranked translation
// unit has asked. The answer is therefore identical to the serial answer for
// every completion interleaving, and a header is extracted exactly once.
//
// The claim key is `(header path, normalized configuration hash)` because that
// is exactly the predicate the serial engine applies: a header counts as
// already indexed only when a `file_config` row registers it as a header under
// *this* translation unit's normalized configuration. Two translation units
// compiled under different configurations both legitimately index the same
// header, in parallel exactly as in serial.
//
// The key is the configuration HASH, not the numeric configuration id. Before
// publication a configuration id is a transient negative value derived from
// that hash; after the first publication the same configuration resolves to a
// real positive row id. Keying on the id would therefore hand the same header
// to a second translation unit the moment the row was minted. The hash is
// stable across that transition.
//
// This header is Clang-free so the LibTooling engine can include it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cidx::index {

// One header this translation unit could own, in the engine's discovery order.
struct HeaderClaimCandidate {
  std::string path;
  // Digest of the bytes the parse actually saw.
  std::optional<std::string> parsed_md5;
  // The serial "already indexed" answer as read from the committed database.
  // True short-circuits the claim: the header was current before this run.
  bool already_indexed_in_database = false;
};

class HeaderClaimOracle {
public:
  HeaderClaimOracle() = default;
  HeaderClaimOracle(const HeaderClaimOracle &) = delete;
  auto operator=(const HeaderClaimOracle &) -> HeaderClaimOracle & = delete;
  HeaderClaimOracle(HeaderClaimOracle &&) = delete;
  auto operator=(HeaderClaimOracle &&) -> HeaderClaimOracle & = delete;
  virtual ~HeaderClaimOracle() = default;

  // Blocks until every translation unit with a lower legacy rank has claimed,
  // then returns one flag per candidate: true = this translation unit extracts
  // it, false = a lower-ranked translation unit already owns it (counted
  // "already", exactly as in serial).
  //
  // Normally one call per translation unit. A RETRY of the same rank may call
  // again: the oracle re-grants that rank's own previous grants, so a retried
  // translation unit does not lose the headers it already owns to itself.
  [[nodiscard]] virtual auto
  claim(std::size_t rank, std::string_view configuration_identity,
        const std::vector<HeaderClaimCandidate> &candidates)
      -> std::vector<bool> = 0;

  // Releases a rank that will never claim: a failed parse, a source that
  // changed under the parse, a cancelled or crashed worker, or a translation
  // unit served from the fact cache. Without it the ordered gate stalls on a
  // translation unit that never asks. Idempotent, and safe to call after the
  // rank has already been released.
  virtual void release_unclaimed(std::size_t rank) = 0;

  // Drops every grant held by `rank`, making those headers claimable again.
  //
  // Required when a translation unit is granted headers and its PUBLICATION
  // then fails. The grant would otherwise outlive the work: a later
  // translation unit sharing the header would be denied as "in-flight owned"
  // and would report it "already", while no row was ever written. Serially the
  // next unit finds no committed row and indexes it, and this restores that.
  virtual void revoke_grants(std::size_t rank) = 0;
};

// Counters the runner publishes so an operator can prove amortisation survived.
struct HeaderClaimMetrics {
  std::uint64_t candidates = 0;
  std::uint64_t granted = 0;
  // Denied because the committed database already had the header current.
  std::uint64_t denied_already_indexed = 0;
  // Denied because a lower-ranked translation unit in THIS run owns it. This is
  // the counter that must equal the serial "already" amortisation.
  std::uint64_t denied_in_flight_owner = 0;
  // Re-granted to the same rank on a retry.
  std::uint64_t regranted_on_retry = 0;
  // Grants dropped because their owner's publication failed.
  std::uint64_t revoked_after_publish_failure = 0;
  // Longest time a worker spent waiting for its turn at the ordered gate.
  double max_gate_wait_seconds = 0.0;
  double total_gate_wait_seconds = 0.0;
};

// The production oracle. Construct one per index run.
class SequencedHeaderClaimOracle final : public HeaderClaimOracle {
public:
  SequencedHeaderClaimOracle();
  ~SequencedHeaderClaimOracle() override;

  [[nodiscard]] auto claim(std::size_t rank,
                           std::string_view configuration_identity,
                           const std::vector<HeaderClaimCandidate> &candidates)
      -> std::vector<bool> override;

  void release_unclaimed(std::size_t rank) override;
  void revoke_grants(std::size_t rank) override;

  // Unblocks every waiter and makes all subsequent claims non-owning. Used on
  // cancellation and on scheduler teardown so no worker can outlive the run
  // holding the gate.
  void cancel();

  [[nodiscard]] auto metrics() const -> HeaderClaimMetrics;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cidx::index
