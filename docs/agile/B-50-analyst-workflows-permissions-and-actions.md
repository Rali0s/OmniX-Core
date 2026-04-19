# B-50: Analyst Workflows, Permissions, and Actions

## Title
Analyst Workflows, Permissions, and Actions

## Why This File Exists
The handnotes and AIP-style framing point to a system where objects do not just exist; operators act on them through controlled workflows, roles, and safe actions. This file defines that analyst shell for Omni.

## Current Omni Alignment
- Omni already supports safe local tools, doctor/build flows, case analysis, reports, and permission contexts.
- Current analyst flows are CLI-first and role-aware, but still early compared with a full operations platform.

## Target Architecture Or Capability
- Analysts work over objects and cases through clear workflow stages, safe actions, and permissions.
- Roles control what can be seen, run, and persisted.
- Actions remain deterministic, local-first, and audited.

## In-Scope / Out-Of-Scope
- In scope:
  - analyst shell
  - roles and permissions
  - safe local actions
  - workflow stage framing
- Out of scope:
  - full GUI workflow builder
  - unrestricted automation
  - remote orchestration control plane

## Dependencies
- [B-30: Ontology Object Layer and Lineage](B-30-ontology-object-layer-and-lineage.md)
- [B-40: Decision Logic and Repeatable Functions](B-40-decision-logic-and-repeatable-functions.md)

## User Stories
- As Omni, I want actions and permissions to sit on top of the object layer cleanly.
- As an operator, I want safe workflows that tell me what I can inspect, run, and save.
- As an implementer, I want the analyst shell to map cleanly to existing case/incident/tool/report commands.

## Technical Work Items
- Frame the CLI analyst experience as the current workflow shell.
- Clarify role expectations and safe action categories.
- Tie permissions to visibility, actionability, and feedback storage.
- Connect action surfaces to cases, incidents, and decision outputs.

## Acceptance Criteria
- Arc-B clearly describes Omni’s analyst shell as the workflow surface above the object layer.
- Roles, permissions, and actions are presented as connected architecture pieces.
- The file stays consistent with current CLI-first behavior.

## Demo / Validation Commands
- `./build/omnix case case-12882007449234465353`
- `./build/omnix tool inspect-host -- --linux`
- `./build/omnix tool report-case -- case-12882007449234465353`

## Risks / Notes
- The main risk is overreaching into full Gotham UI territory.
- This file should keep the current CLI shell explicit as the present implementation form.
