# OmniX V2-V3 Roadmap

## Summary
- Status: `v1` done, `v2` in progress, `v3` deferred
- Source of truth for TZE behavior is [res/tze.txt](../../res/tze.txt).
- Parsing and indexing already exist through the X++ layer.
- Runtime orchestration is still primarily hardcoded in [src/session_coordinator.cpp](../../src/session_coordinator.cpp), so Omni is TZE-shaped but not yet fully TZE-native.

## Repo Truth
- Parsed TZE source currently covers 4 sections: Build CMake, X++ Language Engine, X++ Security Engine, and X++ Self Pre-Processor Engine.
- Generated section outputs already exist and no longer rely on the old generic fallback stub.
- The stable deterministic backbone keeps legacy source keys for compatibility while rendering HumanReadable labels from [Legacy X++ Translation Map](99-legacy-xpp-translation-map.md):
  - `Cache.PrepareWorkspace` (`xProcessingCache`)
  - `Intent.DecodeInstruction` (`x.Define.Low`)
  - `Knowledge.EvidenceRanking` (`x.DisplayPriorityProcessingGate`)
  - `Memory.FeedbackReview` (`x.DisplayFeedBackLoop`)
  - `Memory.StoreArtifact` (`x.Store`)

## Milestones
| Milestone | Status | Definition |
| --- | --- | --- |
| `v1 shipped` | done | CLI analyst console, native tooling, reporting, case management, TZE replay/diff/report, and deterministic planning are usable. |
| `v2 deterministic TZE completion` | in progress | Omni executes source-backed TZE behavior with deeper semantics, richer safe simulation, and conformance coverage. |
| `v3 optional Ollama assist` | deferred | Assistive provider support for summaries, clustering help, and natural-language review after `v2` stabilization. |

## Delivery Sequence
1. [Epic 1: Source-Driven TZE Execution](10-epic-source-driven-execution.md)
2. [Epic 2: Stateful Query Runtime](20-epic-stateful-query-runtime.md)
3. [Epic 3: Language Engine Probability Loop](30-epic-language-engine-probability-loop.md)
4. [Epic 4: Self Pre-Processor / uAC Runtime](40-epic-self-preprocessor-uac.md)
5. [Epic 5: Security Simulation and Audit](50-epic-security-simulation-audit.md)
6. [Epic 6: TZE Conformance Tests](60-epic-tze-conformance-tests.md)
7. [V2 Pre/Post Runtime, Teaching Precedence, Ollama Hardening, and Local Retrieval Foundations](97-v2-prepost-teaching-and-retrieval-foundations.md)
8. [X++ Module Recipe Authoring Phase](98-xpp-module-recipe-authoring.md)
9. [Legacy X++ Translation Map](99-legacy-xpp-translation-map.md)
10. [Immediate Goals Completion: Simplex, Ingestion, Defense, Soak, Persona](100-immediate-goals-simplex-defense-soak-persona.md)
11. [Neuromorphic Backtrace, Backtest, and Back-add Research Track](101-neuromorphic-backtrace-backtest-backadd.md)
12. [NeuralNetwork + TensorFlow Math Track](102-neuralnetwork-tensorflow-math-track.md)
13. [Native C++ Neural Signal Router](103-native-neural-signal-router.md)
14. [Proactive Infrastructure Thresholds](104-proactive-infrastructure-thresholds.md)
15. [Ghostline `gg` Integration](105-ghostline-gg-integration.md)
16. [Recursive Why/Diff and API/Link UX](106-recursive-why-diff-and-api-link-ux.md)

## V2 Completion Criteria
- Runtime stage execution is derived from source-backed TZE graph data instead of a primarily hardcoded stage pipeline.
- Query operators maintain deterministic state and affect behavior beyond string tagging.
- Language-resolution flows produce candidate sets, bounded reruns, and stored outcomes with visible reasoning.
- Self-preprocessor state is persisted, replayable, and bounded for low-disk environments.
- Security coverage remains safe while producing richer audit and simulation artifacts.
- Conformance tests fail when source stages or expected behaviors drift from runtime implementation.

## V3 Gate
- `v3` is blocked on `v2` stabilization.
- No Ollama-dependent behavior should be required to satisfy any `v2` acceptance criterion.
- Assistive providers stay out of the `v2` execution authority, planning authority, and conformance expectations.
