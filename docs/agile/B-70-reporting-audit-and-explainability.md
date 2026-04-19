# B-70: Reporting, Audit, and Explainability

## Title
Reporting, Audit, and Explainability

## Why This File Exists
Operational systems need outputs that can be reviewed, saved, explained, replayed, and audited. This file defines that layer for Omni in a Foundry/Gotham-aligned way.

## Current Omni Alignment
- Omni already generates case reports, incident reports, TZE reports, diff reports, and explain-change outputs.
- Current reporting is strong for a local CLI system, but not yet framed as a broader audit and explainability layer.

## Target Architecture Or Capability
- Every important output in Omni is explainable, auditable, and provenance-backed.
- Reports are not side artifacts; they are a first-class platform layer.
- Audit trails tie together objects, actions, runs, outcomes, and operator feedback.

## In-Scope / Out-Of-Scope
- In scope:
  - case, incident, and TZE reporting
  - explainability
  - audit trails
  - provenance-backed outputs
- Out of scope:
  - pure presentation/UI concerns
  - model-polished summaries
  - external governance systems

## Dependencies
- [B-30: Ontology Object Layer and Lineage](B-30-ontology-object-layer-and-lineage.md)
- [B-60: Feedback, Memory, and Reprocessing Loops](B-60-feedback-memory-and-reprocessing-loops.md)

## User Stories
- As Omni, I want every recommendation and action trace to be explainable after the fact.
- As an operator, I want compact saved artifacts that preserve evidence and provenance.
- As an implementer, I want reporting to stay tied to the same objects, runs, and feedback records used by the runtime.

## Technical Work Items
- Frame case, incident, and TZE outputs as one reporting/audit layer.
- Clarify provenance requirements across reports.
- Tie explainability to deterministic reasoning, lineage, and action traces.
- Connect saved artifacts to future import/export and replay behavior.

## Acceptance Criteria
- Arc-B presents reporting and audit as a core architecture layer.
- Explainability is grounded in evidence, lineage, and deterministic reasoning.
- The file stays aligned with current report and TZE trace behavior.

## Demo / Validation Commands
- `./build/omnix tool report-case -- case-12882007449234465353`
- `./build/omnix incident report incident-15496257841581868193`
- `./build/omnix tze report latest`

## Risks / Notes
- The risk is treating reporting as documentation instead of runtime architecture.
- This file should reinforce that reports are operational artifacts, not just summaries.
