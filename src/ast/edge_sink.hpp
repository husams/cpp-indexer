// Compatibility composition of the focused extraction ports.
#pragma once

#include "ast/fact_emitters.hpp"

namespace cidx::ast {

// Existing visitors consume one sink while they migrate to focused ports.
// Keeping this composition preserves their behavior without preserving a
// monolithic responsibility in the individual services.
class EdgeSink : public DeclarationIdentityResolver,
                 public RelationFactEmitter,
                 public TypeFactEmitter,
                 public DefinitionScopeEmitter,
                 public EvidenceEmitter,
                 public IndexingLifecycle,
                 public PresentationNormalizer {};

} // namespace cidx::ast
