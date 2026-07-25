// Extension fact records and sink port. These are the ONLY output of the
// extraction engine, and they are structurally distinct from core facts
// (ast::SymbolRecord / ast::EdgeRecord, HSE-63): every record carries the
// producing plan hash, rule id, and producer package/version, and a
// DeclaredCompleteness that can never be silently promoted to "core
// complete" (HSE-64: "An omitted matcher case or incomplete rule never
// permits an extension fact set to claim core completeness").
#pragma once

#include "extract/plan_ir.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cidx::extract {

// Shared provenance every extension fact carries. `plan_hash` identifies the
// RULE/PLAN alone (plan_identity.hpp); `artifact_identity` identifies this
// specific execution -- plan plus the pinned workspace/TU/source input
// (plan_identity.hpp's artifact_identity()) -- so two runs of the same plan
// against different source/configuration are never mistaken for the same
// published artifact even when the matched facts happen to look identical.
struct ExtensionProvenance {
  std::string plan_hash;
  std::string artifact_identity;
  std::string rule_id;
  std::string producer_package;
  std::uint32_t producer_version = 1;
  DeclaredCompleteness completeness = DeclaredCompleteness::complete;

  friend bool operator==(const ExtensionProvenance &,
                         const ExtensionProvenance &) = default;
};

struct ExtensionEvidence {
  std::string file;
  std::int64_t line = 0;
  std::int64_t col = 0;

  friend bool operator==(const ExtensionEvidence &,
                         const ExtensionEvidence &) = default;
};

struct ExtensionNodeFact {
  ExtensionProvenance provenance;
  std::string namespace_name;
  std::string node_kind;
  std::string identity;
  ExtensionEvidence evidence;

  friend bool operator==(const ExtensionNodeFact &,
                         const ExtensionNodeFact &) = default;
};

struct ExtensionRelationFact {
  ExtensionProvenance provenance;
  std::string namespace_name;
  std::string relation_kind;
  std::string from_identity;
  std::string to_identity;
  ExtensionEvidence evidence;

  friend bool operator==(const ExtensionRelationFact &,
                         const ExtensionRelationFact &) = default;
};

struct ExtensionAttributeFact {
  ExtensionProvenance provenance;
  std::string namespace_name;
  std::string attribute_name;
  std::string identity;
  std::string value;
  ExtensionEvidence evidence;

  friend bool operator==(const ExtensionAttributeFact &,
                         const ExtensionAttributeFact &) = default;
};

struct ExtensionUnknownFact {
  ExtensionProvenance provenance;
  std::string namespace_name;
  std::string reason_code;
  std::string identity;
  ExtensionEvidence evidence;

  friend bool operator==(const ExtensionUnknownFact &,
                         const ExtensionUnknownFact &) = default;
};

class ExtensionFactSink {
public:
  virtual ~ExtensionFactSink() = default;

  virtual void emit(const ExtensionNodeFact &fact) = 0;
  virtual void emit(const ExtensionRelationFact &fact) = 0;
  virtual void emit(const ExtensionAttributeFact &fact) = 0;
  virtual void emit(const ExtensionUnknownFact &fact) = 0;
};

// In-memory recorder: canonicalize() sorts and dedups each record family so
// re-running an unchanged plan against an unchanged TU produces a
// byte-identical canonical batch regardless of Clang's traversal order
// (mirrors ast::FactBatch::canonicalize()).
class InMemoryExtensionFactSink final : public ExtensionFactSink {
public:
  void emit(const ExtensionNodeFact &fact) override;
  void emit(const ExtensionRelationFact &fact) override;
  void emit(const ExtensionAttributeFact &fact) override;
  void emit(const ExtensionUnknownFact &fact) override;

  void canonicalize();

  [[nodiscard]] const std::vector<ExtensionNodeFact> &nodes() const {
    return nodes_;
  }
  [[nodiscard]] const std::vector<ExtensionRelationFact> &relations() const {
    return relations_;
  }
  [[nodiscard]] const std::vector<ExtensionAttributeFact> &attributes() const {
    return attributes_;
  }
  [[nodiscard]] const std::vector<ExtensionUnknownFact> &unknowns() const {
    return unknowns_;
  }

  // A canonical, deterministic textual rendering of every record family
  // (used for byte-identical-rerun assertions and as the extension-artifact
  // payload feeding HSE-66's ExtensionFactProvider).
  [[nodiscard]] std::string canonical_text() const;

private:
  std::vector<ExtensionNodeFact> nodes_;
  std::vector<ExtensionRelationFact> relations_;
  std::vector<ExtensionAttributeFact> attributes_;
  std::vector<ExtensionUnknownFact> unknowns_;
};

} // namespace cidx::extract
