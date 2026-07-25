// Compatibility composition of the focused extraction ports.
#pragma once

#include "ast/fact_emitters.hpp"

namespace cidx::ast {

// Existing visitors consume one sink while they migrate to focused ports.
// Keeping this composition preserves their behavior without preserving a
// monolithic responsibility in the individual services.
class EdgeSink : public StatementFactPorts,
                 public DeclarationPassPorts,
                 public NamespacePassPorts,
                 public DefinitionScopeEmitter,
                 public IndexingLifecycle {};

} // namespace cidx::ast
