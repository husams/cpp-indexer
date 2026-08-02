#include "ast/fact_extraction.hpp"

#include <exception>
#include <stdexcept>

namespace cidx::ast {

auto extract_serial_fact_batch(FrontendSession session,
                               const ExtractionPassRegistry &registry,
                               const IndexingPlan &plan,
                               const SerialFactRoute &route,
                               const PrepublicationProbe &before_publication)
    -> SerialFactExtractionResult {
  if (route.partitions.empty() ||
      route.main_partition >= route.partitions.size()) {
    return {.failure = FactExtractionFailure{
                .kind = FactExtractionFailureKind::invalid_route,
                .detail = "serial extraction requires a valid main partition"}};
  }

  FactBatchRecorder recorder("serial-extraction");
  for (const FactRoutePartition &entry : route.partitions) {
    recorder.set_partition(entry.partition, entry.transient_file_handle);
  }
  const FactRoutePartition &main = route.partitions[route.main_partition];
  recorder.set_partition(main.partition, main.transient_file_handle);
  session.declaration_ports = &recorder;
  session.statement_ports = &recorder;
  session.namespace_ports = &recorder;
  session.definition_ports = &recorder;
  session.evidence = &recorder;
  session.presentation_intents = &recorder;
  session.lifecycle = &recorder;

  SerialFactExtractionResult result;
  bool passes_complete = false;
  try {
    result.report = registry.run(plan, &session);
    passes_complete = true;
    if (before_publication) {
      before_publication(recorder);
    }
    result.batch = recorder.canonical_batch();
    return result;
  } catch (const PassBudgetExceeded &error) {
    result.failure = FactExtractionFailure{
        .kind = FactExtractionFailureKind::budget_exceeded,
        .pass = error.pass_id(),
        .detail = error.dimension()};
  } catch (const std::exception &error) {
    result.failure = FactExtractionFailure{
        .kind = passes_complete
                    ? FactExtractionFailureKind::prepublication_failed
                    : FactExtractionFailureKind::pass_failed,
        .detail = error.what()};
  }
  result.batch.reset();
  return result;
}

} // namespace cidx::ast
