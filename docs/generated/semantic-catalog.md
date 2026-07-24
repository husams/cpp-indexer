# Generated CIDX semantic catalog

- Catalog version: `1`
- Catalog hash: `1adb5f6663a2e48dc3a624c79703ceaa5287f2784731a00bbc469dba8d5935d4`

## Relations

| Layer | ID | Name | Source | Target | Inverse | Traversal | Evidence | Evidence capabilities | Completeness |
|---|---:|---|---|---|---|---|---|---|---|
| symbol | 1 | `calls` | `symbol.callable` | `symbol.callable` | `called_by` | out,in | call_site | call_site,declaration | partial |
| symbol | 2 | `inherits` | `symbol.record` | `symbol.record` | `subclasses` | out,in | declaration | declaration | complete |
| symbol | 3 | `contains` | `symbol.scope` | `symbol.declaration` | `contained_by` | out,in | declaration | declaration | complete |
| symbol | 4 | `specializes` | `symbol.declaration` | `symbol.template` | `specialized_by` | out,in | declaration | declaration,reference_site | partial |
| symbol | 5 | `instantiates` | `symbol.declaration` | `symbol.template` | `instantiated_by` | out,in | declaration | declaration,reference_site | partial |
| symbol | 6 | `overrides` | `symbol.method` | `symbol.method` | `overridden_by` | out,in | declaration | declaration | complete |
| symbol | 7 | `uses` | `symbol.declaration` | `symbol.declaration` | `used_by` | out,in | reference_site | reference_site,call_site | partial |
| symbol | 8 | `field_of` | `symbol.member` | `symbol.record` | `fields` | out,in | declaration | declaration | complete |
| symbol | 9 | `method_of` | `symbol.method` | `symbol.record` | `methods` | out,in | declaration | declaration | complete |
| symbol | 10 | `construct-value` | `symbol.callable` | `symbol.record` | `constructed_by` | out,in | call_site | call_site | partial |
| symbol | 11 | `construct-temp` | `symbol.callable` | `symbol.record` | `constructed_by` | out,in | call_site | call_site | partial |
| symbol | 12 | `construct-heap` | `symbol.callable` | `symbol.record` | `constructed_by` | out,in | call_site | call_site | partial |
| symbol | 13 | `construct-copy` | `symbol.callable` | `symbol.record` | `constructed_by` | out,in | call_site | call_site | partial |
| symbol | 14 | `construct-move` | `symbol.callable` | `symbol.record` | `constructed_by` | out,in | call_site | call_site | partial |
| symbol | 15 | `factory-construct` | `symbol.callable` | `symbol.record` | `constructed_by` | out,in | call_site | call_site | partial |
| symbol | 16 | `destroy` | `symbol.callable` | `symbol.record` | `destroyed_by` | out,in | call_site | call_site | partial |
| symbol | 17 | `friend` | `symbol.declaration` | `symbol.declaration` | `befriended_by` | out,in | declaration | declaration | complete |
| symbol | 18 | `dispatch_calls` | `symbol.callable` | `symbol.callable` | `dispatch_callers` | out,in | call_site | call_site,derived | partial |
| symbol | 19 | `alias_of` | `symbol.alias` | `symbol.declaration` | `aliased_by` | out,in | declaration | declaration | complete |
| symbol | 20 | `of_type` | `symbol.declaration` | `type` | `typed_by` | out,in | declaration | declaration | partial |
| entity | 1 | `generalizes` | `entity` | `entity` | `specialized_by` | out,in | derived | derived | complete |
| entity | 2 | `implements` | `entity` | `entity` | `implemented_by` | out,in | derived | derived | partial |
| entity | 3 | `specializes` | `entity` | `entity` | `specialized_by` | out,in | derived | derived | partial |
| entity | 4 | `composes` | `entity` | `entity` | `composed_by` | out,in | derived | derived | partial |
| entity | 5 | `aggregates` | `entity` | `entity` | `aggregated_by` | out,in | derived | derived | partial |
| entity | 6 | `associates` | `entity` | `entity` | `associated_by` | out,in | derived | derived | partial |
| entity | 7 | `creates` | `entity` | `entity` | `created_by` | out,in | derived | derived | partial |
| entity | 8 | `uses` | `entity` | `entity` | `used_by` | out,in | derived | derived | partial |
| entity | 9 | `destroys` | `entity` | `entity` | `destroyed_by` | out,in | derived | derived | partial |
| entity | 10 | `befriends` | `entity` | `entity` | `befriended_by` | out,in | derived | derived | complete |
| entity | 11 | `instantiates` | `entity` | `entity` | `instantiated_by` | out,in | derived | derived | partial |
| entity | 12 | `declares` | `entity` | `entity` | `declared_by` | out,in | derived | derived | complete |
| symbol | 21 | `has_parameter` | `symbol.callable` | `parameter` | `of_callable` | out,in | declaration | declaration,call_site | complete |
| symbol | 22 | `has_template_parameter` | `symbol.template` | `template_parameter` | `of_template` | out,in | declaration | declaration | complete |
| symbol | 23 | `has_template_argument` | `symbol.template` | `template_argument` | `of_template` | out,in | reference_site | reference_site,call_site | partial |
| symbol | 24 | `has_call_edge` | `symbol.callable` | `edge` | `of_caller` | out,in | call_site | call_site | partial |
| symbol | 25 | `has_evidence` | `symbol.callable` | `evidence` | `of_symbol` | out,in | call_site | call_site,declaration | partial |
| parameter | 1 | `of_type` | `parameter` | `type` | `has_parameter` | out,in | declaration | declaration | complete |
| parameter | 2 | `declared_type` | `parameter` | `type` | `has_declared_parameter` | out,in | declaration | declaration | complete |
| parameter | 3 | `adjusted_type` | `parameter` | `type` | `has_adjusted_parameter` | out,in | declaration | declaration | complete |
| parameter | 4 | `references_symbol` | `parameter` | `symbol` | `referenced_by_parameter` | out,in | declaration | declaration | partial |
| parameter | 5 | `has_evidence` | `parameter` | `evidence` | `of_parameter` | out,in | declaration | declaration | partial |
| template_parameter | 1 | `of_type` | `template_parameter` | `type` | `has_template_parameter` | out,in | declaration | declaration | complete |
| template_parameter | 2 | `has_default` | `template_parameter` | `evidence` | `defaulted_by` | out,in | declaration | declaration | partial |
| template_argument | 1 | `of_type` | `template_argument` | `type` | `has_template_argument` | out,in | reference_site | reference_site | partial |
| template_argument | 2 | `references_symbol` | `template_argument` | `symbol` | `referenced_by_template_argument` | out,in | reference_site | reference_site | partial |
| edge | 1 | `has_argument` | `edge` | `call_argument` | `of_edge` | out,in | call_site | call_site | partial |
| edge | 2 | `has_evidence` | `edge` | `evidence` | `of_edge` | out,in | call_site | call_site | partial |
| call_argument | 1 | `of_type` | `call_argument` | `type` | `has_call_argument` | out,in | call_site | call_site | partial |
| call_argument | 2 | `references_symbol` | `call_argument` | `symbol` | `referenced_by_call_argument` | out,in | call_site | call_site | partial |
| evidence | 1 | `of_edge` | `evidence` | `edge` | `has_evidence` | out,in | call_site | call_site | partial |
| evidence | 2 | `of_occurrence` | `evidence` | `call_argument` | `has_evidence` | out,in | call_site | call_site | partial |
| type | 1 | `references_symbol` | `type` | `symbol` | `of_type` | out,in | declaration | declaration | partial |
| type | 2 | `has_type_edge` | `type` | `type` | `of_type_edge` | out,in | derived | derived | partial |

## Compatibility

Stable IDs are locked in `catalogs/compatibility.json`. A rename or reuse requires a migration entry in the source and a new catalog version.
