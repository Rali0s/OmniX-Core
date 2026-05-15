# Neuromorphic Backtrace, Backtest, and Back-add Research Track

## Summary
- Neuromorphic programming is a future research track for brain-inspired, event-driven, spike-like local computation.
- OmniX does not depend on neuromorphic hardware or SNN frameworks in this phase.
- The first practical step is deterministic simulation of neuromorphic learning-loop ideas through Backtrace, Backtest, and Back-add.

## Core Concepts
- **Backtrace** traces an answer, action, packet observation, or memory artifact backward through source evidence, TZE stages, provider input, and stored provenance.
- **Backtest** replays historical runs or evidence against newer rules, classifiers, definitions, Simplex codes, or guardrails to measure regressions and improvements.
- **Back-add** safely adds validated historical findings back into glossary entries, compact memory artifacts, test fixtures, or rules without re-ingesting full reasoning chains.

## Runtime Direction
- Backtrace should extend TZE replay, report, and provenance views instead of creating a separate lineage store.
- Backtest should reuse soak, regression, and replay infrastructure so old runs can be compared against current behavior without mutating memory.
- Back-add should only persist compact validated artifacts, operator-authored glossary entries, or explicit fixtures.
- Neuromorphic work stays behind local retrieval, reranking, memory pruning, and replay/backtest stability.

## Acceptance Targets
- `define neuromorphic programming`, `define backtrace`, `define backtest`, and `define back-add` resolve from the local glossary.
- Roadmap and goal documents identify this as research-first, not an immediate runtime dependency.
- Future backtrace reports identify source evidence, TZE stages, final artifacts, and memory writes.
- Future backtest reports produce pass/fail deltas without changing persisted memory.
- Future back-add flows prove they store compact validated findings only.

## Research References
- Spiking neural networks and event-driven computation are useful analogies for future local intelligence.
- Frameworks such as Lava, Nengo, Brian, PyNN, Loihi, and SpiNNaker are research references only for now.
- OmniX should prefer deterministic local simulation before binding to immature or hardware-specific neuromorphic stacks.
