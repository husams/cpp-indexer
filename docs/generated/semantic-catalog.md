# Generated CIDX semantic catalog

- Catalog version: `1`
- Catalog hash: `a36486447a5b4d5cb1ad6e1a7248be623e56650ed44469e8e8131cd44eb2dc5e`

## Relations

| Layer | ID | Name | Inverse | Evidence | Completeness |
|---|---:|---|---|---|---|
| symbol | 1 | `calls` | `called_by` | call_site | partial |
| symbol | 2 | `inherits` | `subclasses` | declaration | complete |
| symbol | 3 | `contains` | `contained_by` | declaration | complete |
| symbol | 4 | `specializes` | `specialized_by` | declaration | partial |
| symbol | 5 | `instantiates` | `instantiated_by` | declaration | partial |
| symbol | 6 | `overrides` | `overridden_by` | declaration | complete |
| symbol | 7 | `uses` | `used_by` | reference_site | partial |
| symbol | 8 | `field_of` | `fields` | declaration | complete |
| symbol | 9 | `method_of` | `methods` | declaration | complete |
| symbol | 10 | `construct-value` | `constructed_by` | call_site | partial |
| symbol | 11 | `construct-temp` | `constructed_by` | call_site | partial |
| symbol | 12 | `construct-heap` | `constructed_by` | call_site | partial |
| symbol | 13 | `construct-copy` | `constructed_by` | call_site | partial |
| symbol | 14 | `construct-move` | `constructed_by` | call_site | partial |
| symbol | 15 | `factory-construct` | `constructed_by` | call_site | partial |
| symbol | 16 | `destroy` | `destroyed_by` | call_site | partial |
| symbol | 17 | `friend` | `befriended_by` | declaration | complete |
| symbol | 18 | `dispatch_calls` | `dispatch_callers` | call_site | partial |
| symbol | 19 | `alias_of` | `aliased_by` | declaration | complete |
| symbol | 20 | `of_type` | `typed_by` | declaration | partial |
| entity | 1 | `generalizes` | `specialized_by` | derived | complete |
| entity | 2 | `implements` | `implemented_by` | derived | partial |
| entity | 3 | `specializes` | `specialized_by` | derived | partial |
| entity | 4 | `composes` | `composed_by` | derived | partial |
| entity | 5 | `aggregates` | `aggregated_by` | derived | partial |
| entity | 6 | `associates` | `associated_by` | derived | partial |
| entity | 7 | `creates` | `created_by` | derived | partial |
| entity | 8 | `uses` | `used_by` | derived | partial |
| entity | 9 | `destroys` | `destroyed_by` | derived | partial |
| entity | 10 | `befriends` | `befriended_by` | derived | complete |
| entity | 11 | `instantiates` | `instantiated_by` | derived | partial |
| entity | 12 | `declares` | `declared_by` | derived | complete |

## Compatibility

Stable IDs are locked in `catalogs/compatibility.json`. A rename or reuse requires a migration entry in the source and a new catalog version.
