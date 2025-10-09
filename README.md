# OmniX-Core

OmniX-Core provides the foundational analysis pipeline for comparing function outputs
and steering towards the broader threat-analysis roadmap captured in `README 2.md`.

## Agile-ready architecture

The codebase is now organised into small, testable components that can evolve
incrementally:

| Component | Responsibility |
| --- | --- |
| `ExpressionUtils` | Normalise, trim, and evaluate arithmetic expressions. |
| `FunctionAnalyzer` | Classify raw function output into high-level operation types. |
| `FunctionProfile` | Capture a canonical view of a function's expression, including optional numeric evaluation. |
| `VersusComparator` | Produce both boolean and rich comparison artefacts between two functions. |
| `AgileWorkflow` | Orchestrate comparisons and summarise results as actionable analysis reports. |

Running `main.cpp` produces a human-readable report that underpins iterative
Agile development. Each module exposes a clear API surface, allowing future
stories—such as MITRE integration, decision logic, or telemetry exporters—to be
delivered without rewriting existing behaviour.
