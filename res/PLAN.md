# Omni Local Analyst Console v1

## Summary
Based on the three note pages and the current OmniX codebase, the strongest product to build next is a **CLI-first local analyst console**: Omni ingests host and log data, normalizes it into objects with lineage, scores decisions deterministically, uses LLM SDKs only for decision assist, and executes only safe local actions through the existing module framework.

The notes map cleanly to a 5-part Omni architecture:
1. **Raw data -> transform/prep -> object layer**
2. **Single source of truth with role-aware access**
3. **Decision engine over objects, comments, rules, and datasets**
4. **Feedback loop and memory for faster later runs**
5. **LLM-assisted classification/summarization/recommendation, not autonomous execution**

The current repo already has good foundations for this:
- request routing, memory, feedback, and definition flow
- native tool discovery and safe local execution
- doctor/build/preflight workflow orchestration
- typed runtime modules and persistent local state

So the next product should **extend Omni from “build-and-tool workflow engine” into “local evidence and decision console.”**

## Key Changes
### 1. Add a host-and-log ingestion pipeline
- Build a new ingestion path for:
  - local text logs
  - JSON logs
  - command output
  - simple OS facts and process snapshots
- Normalize everything into a shared object format with:
  - source path or command
  - timestamp
  - object type
  - extracted fields
  - lineage/provenance metadata
  - confidence
- Keep the first pass local-only and file-based. No remote collectors yet.

### 2. Add an object/lineage store as the “single source”
- Introduce a persistent case/object layer under Omni memory for:
  - observations
  - normalized objects
  - evidence links
  - analyst comments
  - decision history
- Make this the canonical storage layer that the notes imply with “single source,” “data lineage,” and “data -> objects.”
- Preserve raw input references so every decision can point back to original evidence.

### 3. Add a deterministic decision engine
- Build a rules-and-scoring engine that reads normalized objects and outputs:
  - findings
  - ranked interpretations
  - recommended next actions
  - confidence and reason trail
- First action classes should be:
  - search local files/logs
  - run safe native tools already supported by Omni
  - build/inspect local projects
  - generate reports/summaries
- Do not allow destructive or remote actions in this milestone.

### 4. Add role-aware access and analyst workflow state
- Add a permission context around cases, evidence views, and actions.
- First pass only needs:
  - admin
  - analyst
  - read-only
- Use that to control:
  - redacted vs full views
  - which actions are runnable
  - whether feedback can update learned preferences

### 5. Add LLM SDK support as a decision-assist layer
- Add a provider interface for LLM SDK libraries, but keep it optional.
- The model layer should only do:
  - classification
  - summarization
  - explanation
  - candidate action recommendation
- The deterministic engine must remain the execution authority.
- If no model provider is configured, Omni still works with rule-based reasoning only.

### 6. Reuse the current Omni action plane
- Keep the existing `tool`, `doctor`, `build`, and memory flows.
- Treat them as callable safe-action modules from the new analyst workflow.
- `nmap` fits as a later supporting action, not the primary data model.
- `ask` should become the natural entrypoint over these new intents.

## Public Interfaces
- Extend request routing with new intents such as:
  - `IngestData`
  - `AnalyzeCase`
  - `DecideAction`
  - `InspectCase`
- Add new CLI commands:
  - `omnix ingest <path|command>`
  - `omnix analyze <case|source>`
  - `omnix decide <case>`
  - `omnix case <id>`
- Add core types for:
  - `ObservationRecord`
  - `NormalizedObject`
  - `EvidenceLink`
  - `CaseRecord`
  - `DecisionCandidate`
  - `PermissionContext`
  - `ModelAssistRequest` / `ModelAssistResponse`
- Extend memory storage with persistent files for:
  - observations
  - cases
  - decision history
  - analyst comments
- Keep `ask` as a routed interface over the same flows.

## Test Plan
- Ingest tests:
  - plain-text log input
  - JSON log input
  - command-output input
  - repeated ingest preserves provenance without duplicating identical objects incorrectly
- Object/lineage tests:
  - normalized objects link back to raw evidence
  - comments and derived findings attach to the same case cleanly
- Decision tests:
  - the same evidence yields the same ranked actions deterministically
  - feedback updates later recommendations without losing the reason trail
- Permission tests:
  - analyst/admin/read-only views differ correctly
  - restricted actions do not execute outside allowed contexts
- LLM-assist tests:
  - structured model output is accepted only when valid
  - model suggestions never bypass deterministic action gating
  - Omni still functions with no model provider configured
- CLI tests:
  - `ingest`, `analyze`, `decide`, and `case` work end to end
  - `ask` routes correctly into those flows
  - existing `tool`, `doctor`, and `build` commands continue to work

## Assumptions
- V1 is **CLI-first**.
- V1 focuses on **host data and logs first**, not PCAP-first.
- V1 performs **recommendations plus safe local actions**, not broad automation.
- LLM SDKs are **assistive and optional**, not the control plane.
- The current Omni memory and module framework remain the base runtime rather than being replaced.
- Network capture, Wireshark integration, MITRE/SIEM enrichment, and broader web console work are follow-on phases after this analyst-console core exists.
