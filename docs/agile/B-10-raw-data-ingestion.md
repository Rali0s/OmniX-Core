# B-10: Raw Data Ingestion

## Title
Raw Data Ingestion

## Why This File Exists
Foundry-style systems begin by accepting messy, diverse inputs and treating them as first-class material for later transformation. This file frames Omni’s ingestion future around arbitrary raw inputs, especially SIEM-like text and operational streams.

## Current Omni Alignment
- Omni already ingests plain text, JSON-like input, command output, build logs, SSH/auth logs, and native tool output.
- The current ingest flow creates observations and normalized objects from local sources.

## Target Architecture Or Capability
- Omni accepts arbitrary operational records such as log fragments, alerts, shell output, tool output, structured events, and SIEM-like row sets.
- Ingestion remains deterministic and local-first while being broad enough to accept mixed, messy data.
- Inputs are preserved with provenance so later transforms can run repeatedly.

## In-Scope / Out-Of-Scope
- In scope:
  - arbitrary raw data intake
  - SIEM/log/event-style inputs
  - command and tool output capture
  - provenance-preserving ingest
- Out of scope:
  - remote collectors
  - streaming cluster infrastructure
  - direct third-party SaaS connectors in this arc

## Dependencies
- [Prime-Arc-B Roadmap](B-00-prime-arc-b-roadmap.md)
- [res/PLAN.md](../../res/PLAN.md)

## User Stories
- As Omni, I want to ingest arbitrary raw operational data without losing source fidelity.
- As an operator, I want Omni to accept messy SIEM-style inputs and still make them useful later.
- As an implementer, I want ingestion to preserve enough provenance for repeated transforms and audits.

## Technical Work Items
- Standardize input classes across logs, command output, structured rows, and tool output.
- Preserve source identity, timestamp context, and confidence hints at ingest time.
- Keep raw payloads available for later transforms and audit review.
- Ensure ingested material can be reprocessed without needing to re-collect it.

## Acceptance Criteria
- Arc-B defines ingestion broadly enough to cover arbitrary operational data.
- Provenance and raw-source preservation are explicit.
- The doc ties raw ingest to later transform, object, and workflow stages.

## Demo / Validation Commands
- `./build/omnix ingest /Users/premise/Documents/github/OmniX-Core/res/tze.txt`
- `./build/omnix tool inspect-log -- /Users/premise/Documents/github/OmniX-Core/res/tze.txt`

## Risks / Notes
- The largest risk is designing ingest too narrowly around current sample files.
- This file should keep the door open for SIEM-scale inputs without promising distributed ingestion today.
