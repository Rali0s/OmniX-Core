# B-90: Post-B Extensions

## Title
Post-B Extensions

## Why This File Exists
Prime-Arc-B should end with a clear boundary: what belongs to the core Foundry/Gotham-aligned architecture for Omni, and what belongs to later platform/application expansion.

## Current Omni Alignment
- Omni has a strong local deterministic core and a growing architecture story.
- It does not yet provide the full platform, UI, app-distribution, or model-augmented ecosystem implied by broader Foundry/Gotham language.

## Target Architecture Or Capability
- Capture the work that should remain after the core Arc-B architecture is defined.
- Keep this file as a deferred-extension document, not a second roadmap.

## In-Scope / Out-Of-Scope
- In scope:
  - deferred platform/application layers
  - future UI and broader orchestration posture
  - future assistive augmentation
- Out of scope:
  - immediate execution planning for Arc-A or current v2 work
  - deterministic TZE completion details already covered elsewhere

## Dependencies
- [B-00: Prime-Arc-B Roadmap](B-00-prime-arc-b-roadmap.md)
- [90-backlog-post-v2.md](90-backlog-post-v2.md)

## User Stories
- As Omni, I want a clean separation between core architecture planning and later platform ambitions.
- As an operator, I want to know what remains outside the current architecture arc.
- As an implementer, I want deferred items captured without muddying current priorities.

## Technical Work Items
- List broader platform and application-layer extensions as deferred work.
- Keep future assistive layers, richer UI surfaces, and broader orchestration here.
- Maintain separation from deterministic Arc-A and core Arc-B commitments.

## Acceptance Criteria
- B-90 reads as deferred-extension planning only.
- It does not duplicate Arc-A or current v2 execution work.
- It keeps future platform expansion visible without changing current priorities.

## Demo / Validation Commands
- `./build/omnix tze latest`
- `./build/omnix incident list`

## Risks / Notes
- This file should not become a dumping ground for every future idea.
- Keep it focused on real post-core extensions.
