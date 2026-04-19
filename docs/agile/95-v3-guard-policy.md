# V3 Guard Policy

## Why this file exists
This file defines the first allowed Ollama-assisted execution boundary for OmniX. It exists to keep `v3` additive, local, and operator-safe while preserving the deterministic TZE core built in `v2`.

## Current Omni alignment
- `v2` is deterministic and replayable.
- The provider seam already supports `null`, dormant `ollama` probing, guarded summaries for `tze explain-change`, and guarded summaries for `tze report`.
- The first execution-oriented assist surface is **schema-validated safe tool planning**.
- The second execution-oriented assist surface is **schema-validated recipe-bound build selection**.

## Guard policy
- Ollama may propose a tool action.
- Omni remains the execution authority.
- Raw model output is never treated as truth.
- Model proposals must be strict JSON and must pass schema validation.
- A proposal must pass Omni allowlist validation before any action runs.
- When validation fails, Omni falls back to deterministic behavior without failing the user flow.
- All assist usage or bypass is recorded in the TZE ledger.

## Allowed in this phase
- `tze explain-change --assist`
- `tze report --assist`
- `ask --assist` or `omnix shell` with assist enabled, but only when the provider proposes one validated allowlisted tool action
- `build --assist`, `preflight --assist`, or `ask --assist` for build intents, but only when the provider selects one validated allowlisted recipe id

## Allowlisted tool actions in this phase
- `inspect-log`
- `inspect-build`
- `inspect-host`
- `regex-search`
- `deep-grep`

## Explicitly blocked in this phase
- Ollama-driven arbitrary build execution
- Ollama-driven firewall or security mutation
- Ollama-driven unrestricted native tool execution
- Raw freeform model output shown as authoritative truth
- Shell operators, pipelines, redirection, or multi-step plans generated directly by the model

## Validation rules
- Proposed tool name must be on the allowlist.
- Argument count is bounded.
- Arguments must pass basic safety validation.
- Path arguments must already exist locally when required by the selected tool.
- Tool-specific argument shape must match Omni’s local policy.
- Proposed build recipe id must already exist in the alias recipe allowlist.
- Proposed fallback recipe id, when present, must also already exist in the alias recipe allowlist.
- Confidence must be bounded and parsed from strict JSON.
- Omni still owns preflight, acquisition, execution, install staging, verification, and persistence.

## Operator expectations
- Assist is optional and off by default unless explicitly enabled.
- A validated assist plan is visible in normal CLI output, replay, report, and diff.
- A bypassed or rejected assist plan is also visible and does not silently change behavior.
- Deterministic fallback must always remain available.

## Next expansion order
1. Keep summary/explain assist stable.
2. Keep safe tool planning narrow and read-only.
3. Keep recipe-bound build selection narrow and deterministic.
4. Consider guarded security recommendations later.
5. Only much later consider approved mutations with explicit confirmation, audit, and rollback notes.
