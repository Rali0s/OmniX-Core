# Contributing to OmniX

OmniX is a local-first C++ analyst console and TZE runtime.

The project rule is simple:

```text
Evidence first. Deterministic authority first. Assistive models and external services never own execution.
```

## What I'll Expect

- Bug reports with reproductive steps.
- Documentation improvements.
- Small, focused PRs for tools you maintain.

## What I'll Politely Close

- Feature requests for new major capabilities.
- "Why doesn't it do X like Commercial Tool Y?"
- Anything that changes the deterministic-first design.

## Local Build

```bash
cmake -S . -B build
cmake --build build -j4
```

## Local Tests

```bash
./build/omnix_tests
```

Useful optional checks:

```bash
ctest --test-dir build --output-on-failure
cmake --build build --target validate_generated_xpp
cmake --build build --target validate_tze_conformance
```

Docs-only changes do not need a full build unless runtime files are touched.

## Safety Boundaries

- Do not add automatic destructive actions.
- Do not add arbitrary remote shell execution.
- Do not store secrets in TZE runs, memory, node grains, job cache, reports, fixtures, or docs.
- Do not print API keys, private keys, tokens, phone numbers, credentials, or decrypted secret payloads.
- Keep defensive tooling read-only unless an allowlisted runbook requires exact confirmation and validation evidence.

When in doubt, produce an evidence artifact, CAB recommendation, or proposed plan instead of taking action.

## Contribution Shape

- Keep PRs small and reviewable.
- Prefer dependency-free C++ for core runtime work.
- Preserve compact output by default and verbose output when requested.
- Add tests for changed runtime behavior.
- Use fake hosts, fake tenants, fake IDs, and example fingerprints in fixtures.
- Never include live credentials, real customer data, private keys, tokens, or phone numbers.

## Command Surface Changes

If you add or change a command, update the matching docs and tests:

- CLI usage
- README or manpage when user-facing
- roadmap/spec docs when architectural
- compact output tests and failure behavior

## Git Hygiene

- Do not commit build artifacts, local memory, `.env`, generated secrets, or machine-specific output.
- Keep unrelated changes separate when possible.
- Pull/rebase before pushing if the remote has new commits.

## Security Notes

Until a dedicated `SECURITY.md` exists, do not open public issues containing secrets, exploit details, private keys, real customer data, or live infrastructure identifiers. Share a minimal description and reproduction shape without sensitive material.
