# B-20: Transforms, Normalization, and Re-Indexing

## Title
Transforms, Normalization, and Re-Indexing

## Why This File Exists
The notes emphasize that the value is not only taking raw data in, but repeatedly reshaping it, resorting it, and rerunning logic over it. This is the Foundry-style transform layer for Omni.

## Current Omni Alignment
- Omni already parses several operational formats into normalized objects.
- Omni already has TZE/X++ parse and index layers and can re-run deterministic flows over stored state.

## Target Architecture Or Capability
- Omni repeatedly transforms raw inputs into stable normalized forms.
- Re-indexing is a first-class behavior, not a side effect.
- Multiple transform passes are supported so data can be refined across iterations.

## In-Scope / Out-Of-Scope
- In scope:
  - transforms
  - normalization
  - repeated re-indexing
  - transform metadata and lineage
- Out of scope:
  - arbitrary external ETL systems
  - non-deterministic transform logic
  - hidden “magic” enrichment steps

## Dependencies
- [B-10: Raw Data Ingestion](B-10-raw-data-ingestion.md)
- [res/tze.txt](../../res/tze.txt)

## User Stories
- As Omni, I want to repeatedly reshape data so later decision stages can work over consistent objects.
- As an operator, I want to understand how a raw record became a normalized object and then a re-indexed decision input.
- As an implementer, I want transform and re-index loops to remain deterministic and replayable.

## Technical Work Items
- Define staged transforms from raw observations to normalized objects.
- Track normalization passes and re-index metadata.
- Preserve lineage between raw payloads, transforms, and indexed results.
- Align re-index behavior with existing TZE parsing and decision flows where possible.

## Acceptance Criteria
- Arc-B treats transforms and re-indexing as a core layer, not a background helper.
- The file connects repeated transform loops to decision logic and memory.
- Deterministic replay and provenance remain explicit.

## Demo / Validation Commands
- `./build/omnix analyze /Users/premise/Documents/github/OmniX-Core/res/tze.txt`
- `./build/omnix decide /Users/premise/Documents/github/OmniX-Core/res/tze.txt`

## Risks / Notes
- Re-indexing can become vague unless linked to explicit object and lineage models.
- This file should emphasize repeatable refinement rather than generic ETL language.
