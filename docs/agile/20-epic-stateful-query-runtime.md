# Epic 2: Stateful Query Runtime

## Title
Stateful Query Runtime

## Why This Epic Exists
Core operators such as `seek`, `index`, `determine`, `find`, and `rank` are currently represented by shallow helpers in [src/query_runtime.cpp](../../src/query_runtime.cpp). That is enough for mapping and reporting, but not for faithful TZE execution.

## Current Gap In Omni Today
- Query primitives mostly return tagged strings or simple comparisons.
- Candidate ranking and narrowing do not yet carry durable state.
- Runtime decisions are not driven by a reusable query/session model.

## Target Behavior
- Query operators execute against explicit state.
- Indexed values, candidates, and rankings can be inspected, replayed, and reused across stages.
- Deterministic decisions depend on query state, not only on string wrappers.

## In-Scope / Out-Of-Scope
- In scope:
  - query/session state containers
  - indexed values and ranked candidate sets
  - contextual matching and narrowing
  - storage and recall integration with current memory/cache layers
- Out of scope:
  - provider-assisted ranking
  - non-deterministic search behavior
  - unrelated case/report schema changes

## Dependencies
- [Epic 1: Source-Driven TZE Execution](10-epic-source-driven-execution.md)
- Existing cache and memory primitives
- Current planning and reporting flows that consume ranked actions

## User Stories
- As Omni, I want query operators to preserve state so later stages can reason over prior results.
- As an operator, I want ranked candidate sets and narrowing steps to be visible in replay and reports.
- As an implementer, I want query behavior to be deterministic and reproducible across runs.

## Technical Work Items
- Introduce explicit query/session state for active operator chains.
- Store indexed values, candidate lists, ranks, and chosen outputs in runtime state.
- Replace shallow wrappers in the query runtime with stateful primitives.
- Add contextual operators needed by current TZE lowering such as narrowing and candidate matching.
- Feed decision planning from query state rather than standalone labels.
- Persist enough query state for replay and diff without bloating low-disk environments.

## Acceptance Criteria
- Decisions and runtime flows depend on query state rather than string tags alone.
- Operator outputs can be inspected in replay, diff, and reports.
- Deterministic outcomes are reproducible across repeated runs.
- Stored state remains bounded and compatible with pruning behavior.

## Demo / Validation Commands
- `./build/omnix define x.DisplayPriorityProcessingGate /Users/premise/Documents/github/OmniX-Core/res/tze.txt`
- `./build/omnix decide case-12882007449234465353`
- `./build/omnix tze replay latest`
- `./build/omnix tze diff-latest`

## Risks / Notes
- Over-designing query state could make replay heavy on low-SSD systems.
- Query semantics should be introduced in the order required by real TZE source flows, not abstract completeness.
