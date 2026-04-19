# Prime-Arc-C Reference Map

## Summary
Prime-Arc-C is mapping-only. It exists to answer what current Omni most closely corresponds to in Gotham / Foundry / AIP terms, based on the handnotes, Arc-B framing, and reviewed documentation families.

## Mapping Table
| Handnote concept | Prime-Arc-B concept | Current Omni feature | Gotham / Foundry / AIP concept |
| --- | --- | --- | --- |
| Raw data | B-10 raw data ingestion | `ingest`, `inspect-log`, `inspect-host`, native tool capture | Data integration and operational data intake |
| Transform / prep / resort | B-20 transforms and re-indexing | normalization pipeline, parsed objects, TZE/X++ parsing | Transform layer / data shaping |
| Data -> objects | B-30 ontology object layer | observations, normalized objects, evidence links, cases, incidents | Ontology / operational object model |
| Decision making | B-40 decision logic | deterministic `decide`, ranked candidates, feedback scoring | Workflow logic / repeatable function execution |
| Access control | B-50 analyst workflows and permissions | permission context, safe actions, CLI workflows | Governance / secure workflow surface |
| Feedback and loops | B-60 feedback and memory | TZE runs, replay, diff, explain-change, decision feedback | Operational feedback loop / iterative refinement |
| Data + actions + models + feedback | B-70 reporting/audit and B-60 loops | reports, traces, lineage, run ledger | Auditability / explainability / action history |
| Billions of iterations | B-80 scale and deterministic execution | repeated deterministic local execution and replay | High-iteration workflow posture, not current distributed scale |

## What Part Of Gotham / Foundry We Have Effectively Built So Far
The closest honest description is:

- a **Foundry-style data-to-object-to-workflow core**
- plus an **analyst shell**
- plus a **deterministic audit and replay layer**

In Gotham-adjacent language, Omni currently most closely resembles:
- an early ontology-backed investigation and action layer
- a local AIP Analyst-style shell
- a deterministic workflow substrate

It does **not** yet correspond to:
- the full Gotham application stack
- the full Foundry platform
- the full multi-app operational UI

## What Is Still Missing
- broader data platform semantics beyond the current local-first runtime
- richer object/ontology tooling and platform-scale data operations
- deeper workflow application surfaces beyond the CLI analyst shell
- broader app layer, UI layer, and distributed execution posture
- optional assistive model layer after deterministic completion

## Doc Families Reviewed
- AIP overview: platform framing around AI + operations + workflows
- ontology/object layer: closest conceptual analogue for Omni’s object and lineage model
- analyst / assist / workflow surfaces: closest analogue for Omni’s analyst shell
- action / audit / governance concepts: closest analogue for Omni’s safe actions, reports, and deterministic replay

## Notes
- Arc-C is intentionally reference-only.
- Arc-C should not contain implementation stories, acceptance criteria, or backlog items.
- Arc-C is the answer key for “what part of Gotham / Foundry does Omni correspond to right now?”
