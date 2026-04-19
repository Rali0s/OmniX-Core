# Epic 6: TZE Conformance Tests

## Title
TZE Conformance Tests

## Why This Epic Exists
Omni already has strong unit and CLI coverage, but `v2` needs source-vs-runtime conformance checks so the implementation can be proven against [res/tze.txt](../../res/tze.txt), not just against current code behavior.

## Current Gap In Omni Today
- Existing tests confirm many behaviors, but they do not fully enforce source-backed stage and section conformance.
- Generated outputs and runtime behavior can still drift independently if coverage stays too local.

## Target Behavior
- Conformance tests fail when source stages or expected behaviors are missing, bypassed, or silently downgraded.
- Generated section outputs and runtime semantics stay aligned.
- Replay, diff, explain, and report flows remain covered as first-class TZE outputs.

## In-Scope / Out-Of-Scope
- In scope:
  - section-level coverage expectations
  - stage graph conformance checks
  - symbol coverage thresholds
  - runtime-vs-source assertions
  - replay/diff/explain/report regression checks
- Out of scope:
  - model-provider test work
  - unrelated web UI testing
  - non-TZE feature expansion

## Dependencies
- [Epic 1: Source-Driven TZE Execution](10-epic-source-driven-execution.md)
- [Epic 2: Stateful Query Runtime](20-epic-stateful-query-runtime.md)
- [Epic 3: Language Engine Probability Loop](30-epic-language-engine-probability-loop.md)
- [Epic 4: Self Pre-Processor / uAC Runtime](40-epic-self-preprocessor-uac.md)
- [Epic 5: Security Simulation and Audit](50-epic-security-simulation-audit.md)

## User Stories
- As Omni, I want runtime behavior to be checked against the TZE source so drift is caught quickly.
- As an operator, I want replay and reporting to reflect real source-backed execution rather than accidental implementation details.
- As an implementer, I want failing tests when a source stage is skipped, downgraded, or silently abstracted too far.

## Technical Work Items
- Add section-level conformance expectations per TZE section.
- Add stage graph conformance assertions.
- Define symbol coverage thresholds for mapped, abstracted, and blocked behavior.
- Add runtime-vs-source assertions for build, language, self-preprocessor, and safe security flows.
- Extend regression checks for replay, diff, explain-change, report, and incident views.

## Acceptance Criteria
- Source-backed coverage checks are explicit and readable.
- Tests fail when a source stage is unmapped or silently bypassed.
- Generated outputs and runtime behavior stay in sync.
- Conformance coverage becomes part of the normal validation path for `v2`.

## Demo / Validation Commands
- `./build/omnix_tests`
- `ctest --test-dir build --output-on-failure`
- `cmake --build build --target validate_generated_xpp`

## Risks / Notes
- Conformance rules that are too strict too early can block useful incremental work.
- Thresholds should be explicit and versioned so regressions are obvious instead of subjective.
