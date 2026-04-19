# B-60: Feedback, Memory, and Reprocessing Loops

## Title
Feedback, Memory, and Reprocessing Loops

## Why This File Exists
The notes emphasize feedback, learning, and repeated resorting over data. This file defines how Omni’s memory and feedback system become a Foundry-style reprocessing loop rather than a passive store.

## Current Omni Alignment
- Omni already stores history, definitions, preferences, projects, native tools, cases, incidents, and TZE runs.
- Decision feedback and outcomes already feed deterministic planning.

## Target Architecture Or Capability
- Feedback and memory become active inputs to later reprocessing.
- Re-indexing and reranking happen over prior outcomes as well as fresh evidence.
- Operators can see how memory influenced current decisions and workflows.

## In-Scope / Out-Of-Scope
- In scope:
  - memory reuse
  - feedback loops
  - reprocessing of prior outcomes
  - deterministic learning
- Out of scope:
  - opaque model training
  - uncontrolled autonomous learning
  - hidden preference mutation

## Dependencies
- [B-40: Decision Logic and Repeatable Functions](B-40-decision-logic-and-repeatable-functions.md)
- current TZE replay/diff/report chain

## User Stories
- As Omni, I want prior outcomes and operator feedback to improve later deterministic planning.
- As an operator, I want to know when memory changed a recommendation or workflow.
- As an implementer, I want learning loops that stay explainable and replayable.

## Technical Work Items
- Describe memory as an active architectural layer rather than passive persistence.
- Connect feedback records to reprocessing and reranking.
- Clarify bounded deterministic learning rules.
- Tie TZE replay, diff, explain-change, and planning feedback into one reprocessing loop.

## Acceptance Criteria
- Arc-B describes memory as a reprocessing engine, not just storage.
- Feedback and reuse are explicit architectural inputs.
- The file stays deterministic and avoids model-training language.

## Demo / Validation Commands
- `./build/omnix tze latest`
- `./build/omnix tze diff-latest`
- `./build/omnix tze explain-change-latest`
- `./build/omnix decide outcome case-12882007449234465353 decision-172350044359818463 success --note "confirmed"`

## Risks / Notes
- Feedback loops can sound like generic AI “learning” unless the deterministic constraints are stated clearly.
- This file should reinforce replayability and explainability.
