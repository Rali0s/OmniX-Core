# Epic 3: Language Engine Probability Loop

## Title
Language Engine Probability Loop

## Why This Epic Exists
The language section in [res/tze.txt](../../res/tze.txt) describes real OS and language resolution with fallback parsing, probability calculation, narrowing, reruns, and manual confirmation. The current implementation in [src/language_engine.cpp](../../src/language_engine.cpp) is still mostly wrappers and tagged strings.

## Current Gap In Omni Today
- OS detection is shallow.
- Candidate language and OS reasoning is not modeled as a probability loop.
- Manual confirmation and rerun behavior are not implemented as a deterministic workflow.

## Target Behavior
- Omni gathers OS and language evidence, constructs candidate sets, scores them, narrows them, and stores the chosen result.
- The loop is bounded, deterministic, and visible to the operator.
- Manual confirmation remains available without requiring any model provider.

## In-Scope / Out-Of-Scope
- In scope:
  - OS detection inputs
  - candidate language and OS set construction
  - bounded scoring and narrowing loop
  - manual/operator confirmation path
  - persistence of final language/system context
- Out of scope:
  - LLM-backed translation
  - web-based knowledge lookups
  - open-ended language synthesis

## Dependencies
- [Epic 1: Source-Driven TZE Execution](10-epic-source-driven-execution.md)
- [Epic 2: Stateful Query Runtime](20-epic-stateful-query-runtime.md)
- Existing memory store and CLI explain/report flows

## User Stories
- As Omni, I want to refine OS and language candidates deterministically until confidence is sufficient.
- As an operator, I want to see why a language/OS result was chosen and when manual confirmation was used.
- As an implementer, I want stored candidates and scores to be replayable and testable.

## Technical Work Items
- Gather OS and language evidence from local environment and parsed source context.
- Build candidate sets with explicit scores and narrowing thresholds.
- Implement a bounded rerun loop with deterministic stopping rules.
- Add manual confirmation storage and replay support.
- Persist chosen language context into Omni memory for later flows.
- Expose the reasoning trail through replay, explain, and reports.

## Acceptance Criteria
- Runtime produces candidate sets with visible scores.
- Narrowing behavior can run over multiple deterministic passes.
- Final reasoning trail is visible to operators in reports or replay output.
- No model dependency is required.

## Demo / Validation Commands
- `./build/omnix define x.detectNativeLanguage /Users/premise/Documents/github/OmniX-Core/res/tze.txt`
- `./build/omnix define x.determineOSLanguage /Users/premise/Documents/github/OmniX-Core/res/tze.txt`
- `./build/omnix explain x.DNLIO /Users/premise/Documents/github/OmniX-Core/res/tze.txt`

## Risks / Notes
- Probability loops must stay bounded to avoid unstable or noisy replay output.
- Manual confirmation should be stored as a deterministic branch, not an ad hoc override.
