# B-80: Scale, Iteration, and Deterministic Execution

## Title
Scale, Iteration, and Deterministic Execution

## Why This File Exists
The notes explicitly point toward resorting and re-running logic for huge numbers of iterations. This file defines how Omni should describe that scale posture without claiming platform capabilities it does not yet have.

## Current Omni Alignment
- Omni already supports repeated deterministic runs, replay, diff, explain-change, and pruning on local systems.
- Current execution is local-first and bounded, not distributed or Foundry-scale.

## Target Architecture Or Capability
- Omni is framed as a deterministic execution engine that can repeatedly iterate over transformed data and object state.
- The local-first posture remains explicit.
- Future distributed posture is acknowledged as a later extension, not a current claim.

## In-Scope / Out-Of-Scope
- In scope:
  - deterministic iteration
  - repeated execution framing
  - bounded local runtime
  - future scale posture language
- Out of scope:
  - distributed compute implementation
  - claiming current billion-scale throughput
  - model-centric execution

## Dependencies
- [B-20: Transforms, Normalization, and Re-Indexing](B-20-transforms-normalization-and-reindexing.md)
- [B-60: Feedback, Memory, and Reprocessing Loops](B-60-feedback-memory-and-reprocessing-loops.md)

## User Stories
- As Omni, I want to rerun workflows and decision functions repeatedly without losing determinism.
- As an operator, I want to understand how Omni could scale conceptually without confusing that with what it currently does.
- As an implementer, I want a clear architectural statement separating present local execution from future broader scale.

## Technical Work Items
- Define Omni’s current deterministic iteration posture.
- Clarify bounded local execution and low-disk constraints.
- Describe future distributed or higher-scale execution as a later extension only.
- Tie scale language back to repeatable transforms, object workflows, and decision loops.

## Acceptance Criteria
- Arc-B describes repeatable large-iteration thinking without overstating current runtime scale.
- Local deterministic execution remains central.
- The file clearly distinguishes present capability from future posture.

## Demo / Validation Commands
- `./build/omnix tze runs`
- `./build/omnix tze replay latest`
- `./build/omnix memory prune --keep 8 --important-only`

## Risks / Notes
- The biggest risk is implying Foundry-scale implementation already exists inside Omni.
- This file should keep present capability and future direction clearly separated.
