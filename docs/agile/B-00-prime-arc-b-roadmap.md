# Prime-Arc-B Roadmap

## Title
Prime-Arc-B Roadmap

## Why This File Exists
Prime-Arc-B captures the Foundry/Gotham-aligned architecture path for Omni. Arc-A already covers the deterministic Omni/TZE execution track. Arc-B describes how Omni evolves into a data-to-object-to-workflow system that can repeatedly reshape operational data into decisions, actions, and feedback.

## Current Omni Alignment
- Omni already has deterministic ingest, analyze, decide, case, incident, report, and TZE run flows.
- Omni already stores observations, normalized objects, case links, decision candidates, reports, and feedback.
- Omni is still strongest as a local analyst engine rather than a full Foundry-like data platform.

## Target Architecture Or Capability
- Raw data from logs, command output, native tools, and SIEM-like feeds is transformed, normalized, re-indexed, and stored as operational objects with lineage.
- Decision logic runs repeatedly and deterministically over those objects.
- Analyst workflows, permissions, actions, and audit trails sit on top of the object layer.
- Feedback and reprocessing loops improve later decisions without depending on an external model provider.

## In-Scope / Out-Of-Scope
- In scope:
  - Foundry-style data -> transform -> object -> workflow -> feedback planning for Omni
  - architecture-ready markdowns that complement Arc-A
  - identifying what is already built vs what remains
- Out of scope:
  - full Gotham application claims
  - direct product/code implementation in this document set
  - Ollama activation or model-dependent planning

## Dependencies
- [OmniX V2-V3 Roadmap](00-roadmap.md)
- [res/PLAN.md](../../res/PLAN.md)
- [res/tze.txt](../../res/tze.txt)

## User Stories
- As Omni, I want a second architecture arc that explains the data-to-object-to-workflow future clearly.
- As an operator, I want a planning series that explains how Omni aligns with Foundry/Gotham concepts without overstating current capabilities.
- As an implementer, I want a roadmap that complements Arc-A rather than duplicating it.

## Technical Work Items
- Define Arc-B as the Foundry-style architecture series for Omni.
- Anchor each B-series file to a distinct platform capability area.
- Keep Arc-B grounded in current Omni features and realistic future expansion.
- Explicitly connect Arc-B to Arc-A and keep the relationship clear.

## Acceptance Criteria
- Arc-B clearly reads as a second planning arc rather than a duplicate of Arc-A.
- Arc-B uses Foundry-style data-to-object-to-workflow framing throughout.
- Arc-B stays grounded in current Omni reality and identified future work.
- Arc-B does not claim the repo already implements the whole Gotham application stack.

## Demo / Validation Commands
- `./build/omnix case list`
- `./build/omnix incident list`
- `./build/omnix tze runs`

## Risks / Notes
- The main risk is overstating Omni as Gotham-equivalent instead of Gotham-adjacent.
- Arc-B should stay architecture-oriented and should not weaken Arc-A’s deterministic execution focus.
