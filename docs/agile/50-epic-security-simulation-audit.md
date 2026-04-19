# Epic 5: Security Simulation and Audit

## Title
Security Simulation and Audit

## Why This Epic Exists
The security section in [res/tze.txt](../../res/tze.txt) includes both defensive logic and blocked offensive branches. The current implementation in [src/security_manager.cpp](../../src/security_manager.cpp) is safe, but still shallow for the defensive side.

## Current Gap In Omni Today
- Safe branches are mostly abstracted with lightweight placeholders.
- Audit and simulation artifacts are thinner than the source suggests.
- The blocked vs safe distinction is correct, but reports do not yet provide rich simulated defensive reasoning.

## Target Behavior
- Safe security branches produce stronger audit, trace, classification, and containment artifacts.
- Unsupported branches remain blocked and inert.
- Reports make the difference between `abstracted`, `simulated`, and `blocked` behavior explicit.

## In-Scope / Out-Of-Scope
- In scope:
  - defensive detection flow
  - trace and scope logging
  - threat classification brackets
  - isolate/contain simulation
  - security evidence and report generation
- Out of scope:
  - exploit execution
  - penetration behavior
  - persistence or anti-forensic branches

## Dependencies
- [Epic 1: Source-Driven TZE Execution](10-epic-source-driven-execution.md)
- [Epic 2: Stateful Query Runtime](20-epic-stateful-query-runtime.md)
- Existing case, report, and incident flows

## User Stories
- As Omni, I want to simulate safe defensive flows without executing blocked branches.
- As an operator, I want reports that clearly show what was simulated, what was only abstracted, and what stayed blocked.
- As an implementer, I want safety boundaries to be stable and non-negotiable.

## Technical Work Items
- Expand safe detection, classification, isolation, trace, and log semantics.
- Emit richer audit artifacts into case and incident reports.
- Preserve explicit blocked-path markers for unsafe branches.
- Attach security simulation outcomes to replay, diff, and incident views.
- Keep all blocked branches inert in both runtime and generated output.

## Acceptance Criteria
- No unsupported branch becomes executable.
- Safe branches produce stronger audit artifacts than the current placeholders.
- Reports distinguish `abstracted`, `simulated`, and `blocked` behavior clearly.
- Security traces remain replayable and deterministic.

## Demo / Validation Commands
- `./build/omnix define xXOmni::Detection /Users/premise/Documents/github/OmniX-Core/res/tze.txt`
- `./build/omnix define x.classify /Users/premise/Documents/github/OmniX-Core/res/tze.txt`
- `./build/omnix incident list`
- `./build/omnix incident report <incident-id>`

## Risks / Notes
- Safety policy must not drift because of implementation convenience.
- The most useful outcome here is better simulation and reporting, not broader execution power.
