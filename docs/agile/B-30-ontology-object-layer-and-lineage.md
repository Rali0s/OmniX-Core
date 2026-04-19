# B-30: Ontology Object Layer and Lineage

## Title
Ontology Object Layer and Lineage

## Why This File Exists
The strongest Gotham/Foundry-aligned interpretation of Omni is that it is moving toward an ontology-backed object layer with lineage, links, and single-source operational truth. This file makes that explicit.

## Current Omni Alignment
- Omni already persists observations, normalized objects, evidence links, case records, related cases, incidents, and TZE runs.
- Current lineage is present, but not yet framed as a broader ontology/object layer.

## Target Architecture Or Capability
- Omni’s stored objects become the operational core that later workflows, permissions, actions, and reports rely on.
- Links, lineage, and evidence relationships are first-class, inspectable, and replayable.
- The object layer acts as the single source of operational truth for local analyst work.

## In-Scope / Out-Of-Scope
- In scope:
  - object model
  - lineage
  - links and relationships
  - single-source-of-truth framing
- Out of scope:
  - full Palantir Ontology parity
  - GUI ontology modeling tools
  - multi-tenant platform semantics

## Dependencies
- [B-20: Transforms, Normalization, and Re-Indexing](B-20-transforms-normalization-and-reindexing.md)
- [res/PLAN.md](../../res/PLAN.md)

## User Stories
- As Omni, I want objects and lineage to be the foundation for workflows and actions.
- As an operator, I want every decision and report to point back to concrete evidence and linked objects.
- As an implementer, I want a clear object-layer framing that can evolve without discarding the current case model.

## Technical Work Items
- Position observations, normalized objects, evidence links, cases, incidents, and TZE runs as one connected object substrate.
- Clarify object relationships and lineage expectations.
- Define the single-source-of-truth role of the persisted object layer.
- Keep the object model compatible with current replay, diff, report, and feedback paths.

## Acceptance Criteria
- Arc-B clearly names the object and lineage layer as Omni’s operational core.
- The document ties current Omni structures to future ontology-style behavior.
- Object lineage remains operator-visible and auditable.

## Demo / Validation Commands
- `./build/omnix case list`
- `./build/omnix case search auth`
- `./build/omnix incident list`

## Risks / Notes
- The risk is drifting into abstract ontology language without anchoring back to current Omni objects.
- This file should keep the term “ontology” practical and operational.
