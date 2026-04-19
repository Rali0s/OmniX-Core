# Epic 1: Source-Driven TZE Execution

## Title
Source-Driven TZE Execution

## Why This Epic Exists
Omni currently exposes the right TZE stages, but the main execution path is still largely scripted in [src/session_coordinator.cpp](../../src/session_coordinator.cpp) instead of being driven from [res/tze.txt](../../res/tze.txt). This epic closes the gap between “TZE-shaped” orchestration and a true TZE-native stage graph.

## Current Gap In Omni Today
- Stage ordering is emitted from C++ logic first and source structure second.
- Parsed TZE sections exist, but execution does not yet consume a source-backed stage graph as the primary runtime plan.
- Replay, diff, and reporting describe TZE stages correctly, but they are not yet proving that the runtime came from parsed source graph data.

## Target Behavior
- Omni compiles a stage graph from parsed TZE source and executes through that graph.
- Runtime traces show where a stage originated in the source.
- Existing stable stage ids remain unchanged so current replay, diff, report, and CLI flows stay compatible.

## In-Scope / Out-Of-Scope
- In scope:
  - stage graph extraction from parsed source
  - graph nodes, edges, labels, storage effects, and dispatch metadata
  - runtime dispatch through source-backed stage graph
  - replay/diff/report compatibility
- Out of scope:
  - Ollama or other provider activation
  - making blocked security branches executable
  - replacing existing CLI contracts

## Dependencies
- Existing parse/index stack in `xpp`
- Stable TZE runtime reporting in [src/session_coordinator.cpp](../../src/session_coordinator.cpp)
- Persisted replay/diff/report behavior in the memory store

## User Stories
- As Omni, I want to derive execution stages from parsed TZE source so orchestration stays aligned with the real source of truth.
- As an operator, I want replay and diff output to show that runtime stages came from source-backed graph data.
- As an implementer, I want stable stage ids so existing commands and tests do not need a parallel naming scheme.

## Technical Work Items
- Add a stage-graph representation to the parsed TZE lowering layer.
- Extract stage nodes and transitions from the Build CMake flow in [res/tze.txt](../../res/tze.txt).
- Represent source spans, stage ids, labels, storage effects, and dispatch hints in the graph.
- Refactor session orchestration so source-backed graph execution is the default path.
- Keep a narrowly defined bootstrap fallback only if graph extraction fails, and label it explicitly.
- Attach source provenance to replay, diff, chain, and report outputs.

## Acceptance Criteria
- Runtime can show source-backed stage graph origin for executed stages.
- `xProcessingCache`, `x.Define.Low`, `x.DisplayPriorityProcessingGate`, `x.DisplayFeedBackLoop`, and `x.Store` remain stable public stage ids.
- Replay and diff continue to work after graph-driven execution lands.
- Hardcoded fallback stage ordering is removed or clearly limited to bootstrap-only behavior.
- Existing deterministic CLI flows still return usable next actions and reports.

## Demo / Validation Commands
- `./build/omnix map /Users/premise/Documents/github/OmniX-Core/res/tze.txt`
- `./build/omnix tze replay latest`
- `./build/omnix tze diff-latest`
- `./build/omnix tze explain-change-latest`

## Risks / Notes
- Stage extraction can drift if graph semantics are inferred too loosely from the source.
- Runtime compatibility depends on preserving current stage ids and ledger shape.
- This epic should land before deeper semantic work so later epics build on a source-native execution spine.
