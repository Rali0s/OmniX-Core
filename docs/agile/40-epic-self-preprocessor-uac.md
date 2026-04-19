# Epic 4: Self Pre-Processor / uAC Runtime

## Title
Self Pre-Processor / uAC Runtime

## Why This Epic Exists
The self-preprocessor section in [res/tze.txt](../../res/tze.txt) describes a persistent `uAC` trait, cache, and recovery system. The current implementation in [src/preprocessor_runtime.cpp](../../src/preprocessor_runtime.cpp) is still a lightweight placeholder.

## Current Gap In Omni Today
- Preprocessor functions mostly emit labeled strings.
- There is no real persisted `uAC` cache model.
- Recovery flows and trait indexing are not represented as a bounded runtime subsystem.

## Target Behavior
- Omni persists a real `uAC`-oriented preprocessor state model aligned with the TZE section.
- Trait indexing, epoch-based persistence, and recovery hints become replayable runtime behavior.
- Storage stays bounded for low-disk environments.

## In-Scope / Out-Of-Scope
- In scope:
  - persistent `uAC` cache structure
  - trait indexing and epoch metadata
  - recovery/restore hints
  - bounded local persistence and pruning integration
- Out of scope:
  - filesystem-wide uncontrolled recovery actions
  - opaque binary caches with no replay visibility
  - unrelated case clustering changes

## Dependencies
- [Epic 1: Source-Driven TZE Execution](10-epic-source-driven-execution.md)
- [Epic 2: Stateful Query Runtime](20-epic-stateful-query-runtime.md)
- Existing memory-root and pruning behavior

## User Stories
- As Omni, I want to persist `uAC` preprocessor state so later TZE flows can reuse it.
- As an operator, I want recovery-relevant preprocessor state to be visible and replayable.
- As an implementer, I want the subsystem to honor disk limits and current pruning policies.

## Technical Work Items
- Define a persisted `uAC` cache schema in Omni memory.
- Store trait indexes, epoch markers, and recovery-related references.
- Model recovery flows as deterministic hints and state transitions.
- Integrate preprocessor state with pruning and export/import behavior.
- Expose replay/report views for `uAC` state transitions and recovery decisions.

## Acceptance Criteria
- Persisted preprocessor state is visible to replay and reporting.
- Recovery/rebuild behavior is deterministic.
- Storage limits and pruning rules are explicitly documented and enforced.
- The runtime no longer relies on prefix-only helpers for core `uAC` behavior.

## Demo / Validation Commands
- `./build/omnix define x.BPP /Users/premise/Documents/github/OmniX-Core/res/tze.txt`
- `./build/omnix define x.reGENx /Users/premise/Documents/github/OmniX-Core/res/tze.txt`
- `./build/omnix tze replay latest`

## Risks / Notes
- This subsystem can become storage-heavy if persisted too naively.
- Recovery semantics should remain operator-visible and bounded, especially on systems with low SSD space.
