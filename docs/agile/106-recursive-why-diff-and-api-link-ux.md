# Recursive Why/Diff And API/Link UX

## Summary
This phase adds a deterministic v1 of the OmniX Recursive Why/Diff Solver and pairs it with friendlier operator setup surfaces for API providers and user-local command links.

The runtime goal is simple: `/why` and `omnix why` should explain the last observed path, infer the intended success path, compare the two, and return a next action without requiring provider reasoning.

## Recursive Why/Diff
The solver creates four reasoning slots:

- `SLOT_1`: initial problem state, prompt, intent, target, and known facts.
- `SLOT_2`: intermediate logs, TZE stages, failures, and attempted transitions.
- `SLOT_3`: inferred target state, success criteria, and possible answer shape.
- `SLOT_4`: present-time observer state, current run, memory count, provider posture, and shell context.

It then builds two deterministic paths:

- `Recursive_B`: a generated success path for tool, build, provider, TView, thresholds, Ghostline, neural, and TZE flows.
- `Recursive_A`: the observed path from persisted TZE run evidence.

The diff is classified as `missing step`, `wrong order`, `bad assumption`, `skipped dependency`, `broken threshold`, `missing context`, `false goal`, `corrupted reasoning branch`, or `no blocking diff`.

## Operator Output
`omnix why latest` and shell `/why` use the same eight-section format:

- Current State
- Likely Goal
- What the Logs/Reasoning Show
- Successful Path Pattern
- Difference Found
- Best Estimate Answer
- Why This Matters
- Next Action

The engine stores compact recursive metadata in the TZE ledger and exposes the `x.Recursive.WhyDiff` stage during replay/report. It does not store provider chains, secrets, or full recursive search trees.

When an observed route fails but local memory contains a likely successful route, OmniX emits `x.Recursive.RouteLearning`. Recursive Route Learning mines operating memory caches: learned definitions, prior TZE definition runs, compact history, and `res/local_glossary.tsv`. Its output is a Back-add candidate, not an automatic mutation.

## Tower Of Hanoi Doctrine
The reference notes in `res/RecursionANDTroubleshooting` map Tower of Hanoi to OmniX state transitions:

```text
T(n)=2T(n-1)+1
```

The point is not the disks. The point is that success is often an order-of-operations problem. Infrastructure recovery, outage windows, queue draining, database shutdown order, provider setup, and tool execution all become safer when OmniX asks which dependency must move first.

## API UX
New API surfaces are local-first:

```sh
omnix api status --compact
omnix api doctor
omnix api configure openai
omnix api configure ollama
omnix api template huggingface
```

`configure openai` writes repo-local `./.env` by default and masks secrets in output. `configure ollama` writes local Ollama settings and recommends `ollama serve` plus `ollama run <model>`. HuggingFace remains a template-only future provider with curl guidance and placeholder variables.

Shell shortcuts:

```text
/api
/api openai
/api ollama
/api template huggingface
/provider
```

## Link UX
OmniX keeps the shell script and built-in symlink tool, then adds the top-level wrapper:

```sh
omnix link doctor
omnix link install --with-tze --with-gg --prefix ~/.local/bin
omnix link remove --prefix ~/.local/bin
```

The default target is user-local on macOS/Linux. Future Windows behavior should create `.cmd` shims in a user-writable directory rather than relying on privileged symlink rights.

## Safety
- Recursive Why/Diff v1 is deterministic only.
- Secrets are not printed into reports, memory history, or TZE stages.
- Provider setup writes local config only.
- Link removal removes only OmniX-managed command names.
- HuggingFace is curl/template-only and does not perform network calls in this phase.
