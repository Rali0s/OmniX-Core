# Recursive Why/Diff Architecture

## Purpose
The Recursive Why/Diff Engine answers “why?” by comparing the observed path against a deterministic success pattern. It is not a generic chatbot fallback. It is a local provenance explainer over TZE runs.

## Four Slots
OmniX frames the question with four local records:

- `SLOT_1 Initial Problem State`: the original prompt, intent, target, and starting facts.
- `SLOT_2 Intermediate Reasoning / Logs`: TZE stages, tool output, failures, artifacts, and attempted transitions.
- `SLOT_3 Unknown Goal / Target Answer`: inferred success criteria, expected state, and next-action shape.
- `SLOT_4 Present-Time Observer`: current run reference, memory state, shell context, and provider posture.

## Recursive B: Success Path Finder
Recursive B assumes a valid route exists. It creates a deterministic success pattern from command families:

- Tool action: resolve, validate, execute bounded command, capture output, retain compact artifact.
- Packet capture: select interface/filter, capture, summarize flows, cap payload previews, persist packet evidence.
- Provider probe: load config, validate readiness, avoid secrets, report guarded assist state.
- TZE replay/diff/report: resolve run refs, render provenance, compare stages, recommend next inspection.
- Thresholds/Ghostline/neural flows: load local evidence, classify or evaluate, explain factors, store compact summaries.

## Recursive A: Failure Path Analyzer
Recursive A reads the actual persisted run. It does not invent hidden facts. It traces status, stage order, artifacts, and next action from the TZE ledger.

## Diff
The diff layer classifies the gap between the success path and observed path:

- `missing step`
- `wrong order`
- `bad assumption`
- `skipped dependency`
- `broken threshold`
- `missing context`
- `false goal`
- `corrupted reasoning branch`
- `no blocking diff`

## Recursive Route Learning
Recursive Route Learning is the cache-mining half of the recursive solver. When the observed route fails, OmniX searches operating memory caches before finalizing the explanation:

- learned definitions
- prior successful TZE definition runs
- compact memory history
- local glossary entries

If a stronger route is found, OmniX emits `x.Recursive.RouteLearning` and reports a Back-add candidate. This is how a failed prompt like `what is Apple in technology` can be traced back to `Apple` with domain `technology` before the resolver itself is patched.

## Tower Of Hanoi Analogy
The notes in `res/RecursionANDTroubleshooting` use Tower of Hanoi because it teaches dependency-safe transition order:

```text
T(n)=2T(n-1)+1
```

To move `n` safely, the system must first solve `n-1`, move the critical object, then solve `n-1` again. OmniX applies the same shape to operational work: drain consumers before stopping databases, confirm provider readiness before assist, identify a concrete port before TView, and inspect a run before diffing it.

## Output Contract
The operator-facing answer is always:

1. Current State
2. Likely Goal
3. What the Logs/Reasoning Show
4. Successful Path Pattern
5. Difference Found
6. Best Estimate Answer
7. Why This Matters
8. Next Action

## Persistence
TZE stores compact recursive report metadata: slot summaries, success/failure path summaries, diff category, confidence, and next action. It does not persist long chain-of-thought, provider prompts, API keys, or raw unrelated logs.
