# B-40: Decision Logic and Repeatable Functions

## Title
Decision Logic and Repeatable Functions

## Why This File Exists
The notes point toward billions of repeated iterations over sorted and re-sorted data. In Omni, that means a deterministic function-and-decision layer that can be rerun over object sets, not one-off command suggestions.

## Current Omni Alignment
- Omni already ranks decision candidates deterministically.
- Decision feedback and outcome storage already exist and feed later planning.
- Current decision logic is still local and bounded rather than platform-scale.

## Target Architecture Or Capability
- Omni executes repeatable scoring and decision functions over object state.
- Function results can be rerun as inputs change or feedback accumulates.
- Deterministic function execution becomes a core platform behavior rather than only a case-specific feature.

## In-Scope / Out-Of-Scope
- In scope:
  - repeatable deterministic functions
  - decision scoring and ranking
  - iterative reprocessing over object state
  - evidence-backed next actions
- Out of scope:
  - non-deterministic planning
  - provider-dependent decision authority
  - remote automation claims

## Dependencies
- [B-30: Ontology Object Layer and Lineage](B-30-ontology-object-layer-and-lineage.md)
- [docs/agile/20-epic-stateful-query-runtime.md](20-epic-stateful-query-runtime.md)

## User Stories
- As Omni, I want to rerun decision logic over the same object space as evidence or feedback changes.
- As an operator, I want ranked actions to be explainable, repeatable, and linked to evidence.
- As an implementer, I want functions to be reusable building blocks across cases, incidents, and workflows.

## Technical Work Items
- Describe deterministic decision functions as first-class architecture elements.
- Tie function execution to object state, lineage, and feedback inputs.
- Clarify how repeated execution changes recommendations while preserving reproducibility.
- Connect the decision layer to later workflow and action surfaces.

## Acceptance Criteria
- Arc-B presents decisions as repeatable functions over objects, not only UI-facing recommendations.
- Function reruns and feedback influence are explicit.
- Deterministic reasoning and explainability remain central.

## Demo / Validation Commands
- `./build/omnix decide case-12882007449234465353`
- `./build/omnix decide feedback case-12882007449234465353 decision-172350044359818463 helpful --note "worked"`

## Risks / Notes
- The largest risk is describing a compute layer that sounds abstract but is disconnected from current Omni planning behavior.
- This file should stay rooted in deterministic scoring, replay, and feedback.
