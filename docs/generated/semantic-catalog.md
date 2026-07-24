# Generated CIDX semantic catalog

- Catalog version: `1`
- Catalog hash: `38453dfc66a3cb7c2e31483cb711cb8a99231a531557e30a46c7d81d0e84ef7b`

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

## Compatibility

Stable IDs are locked in `catalogs/compatibility.json`. A rename or reuse requires a migration entry in the source and a new catalog version.
