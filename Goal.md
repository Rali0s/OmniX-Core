# OmniX
 C++ Based Core Analysis Engine

## Delivery Index

### Current Product Status
- `v1`: done
- `v2`: in progress, deterministic TZE completion milestone
- `v3`: in progress, guarded Ollama-assisted layer is available behind explicit assist enablement, but remains bounded and deterministic-first

### Prime-Arc-A: Omni / TZE Execution Arc
- [V2-V3 Roadmap](docs/agile/00-roadmap.md)
- [Epic 1: Source-Driven TZE Execution](docs/agile/10-epic-source-driven-execution.md)
- [Epic 2: Stateful Query Runtime](docs/agile/20-epic-stateful-query-runtime.md)
- [Epic 3: Language Engine Probability Loop](docs/agile/30-epic-language-engine-probability-loop.md)
- [Epic 4: Self Pre-Processor / uAC Runtime](docs/agile/40-epic-self-preprocessor-uac.md)
- [Epic 5: Security Simulation and Audit](docs/agile/50-epic-security-simulation-audit.md)
- [Epic 6: TZE Conformance Tests](docs/agile/60-epic-tze-conformance-tests.md)
- [Post-V2 Backlog](docs/agile/90-backlog-post-v2.md)
- [V3 Guard Policy](docs/agile/95-v3-guard-policy.md)
- [V3 Knowledge Routing and Definition](docs/agile/96-v3-knowledge-routing-and-definition.md)

### Prime-Arc-B: Foundry / Gotham-Aligned Architecture Arc
- [Prime-Arc-B Roadmap](docs/agile/B-00-prime-arc-b-roadmap.md)
- [B-10: Raw Data Ingestion](docs/agile/B-10-raw-data-ingestion.md)
- [B-20: Transforms, Normalization, and Re-Indexing](docs/agile/B-20-transforms-normalization-and-reindexing.md)
- [B-30: Ontology Object Layer and Lineage](docs/agile/B-30-ontology-object-layer-and-lineage.md)
- [B-40: Decision Logic and Repeatable Functions](docs/agile/B-40-decision-logic-and-repeatable-functions.md)
- [B-50: Analyst Workflows, Permissions, and Actions](docs/agile/B-50-analyst-workflows-permissions-and-actions.md)
- [B-60: Feedback, Memory, and Reprocessing Loops](docs/agile/B-60-feedback-memory-and-reprocessing-loops.md)
- [B-70: Reporting, Audit, and Explainability](docs/agile/B-70-reporting-audit-and-explainability.md)
- [B-80: Scale, Iteration, and Deterministic Execution](docs/agile/B-80-scale-iteration-and-deterministic-execution.md)
- [B-90: Post-B Extensions](docs/agile/B-90-post-b-extensions.md)

### Prime-Arc-C: Reference Mapping Arc
- [Prime-Arc-C Reference Map](docs/agile/C-00-prime-arc-c-reference-map.md)

### Roadmap Notes
- `v1` is considered shipped at the product level.
- `v2` is the final deterministic completion milestone for the TZE suite.
- `v3` is intentionally deferred until `v2` is stable and replayable end to end.
- Prime-Arc-B is the Foundry-style data-to-object-to-workflow architecture arc for Omni.
- Prime-Arc-C is mapping-only and answers what part of Gotham / Foundry the current system most closely resembles.

# Immediate Goals:
1. Finish 10 Goals
2. Build TCP Capture & Dump
3. Build Wireshark-like local TCP analysis primitives where useful, deferring direct Wireshark/TShark library binding unless native `libpcap` analysis hits a hard ceiling
4. Output Simplex / Simple Analysis Codes for packet capture, plain-text payload readout, and JSONL ingestion
5. Load MITRE: Connect MITRE-DB ( JSON ) To SIEM as a separate local data pipeline from the DeepNimSec Ollama connector
6. Choose Course of Action For Defense with diagnostic-first prompts for task kill review, CPU diag, memory diag, log view, PID review, and port closure recommendations
7. Build the missing `x.Preprocessor` / `x.PostProcessor` runtime path from `res/tze.txt` pseudo-code, including bounded artifact retention and final artifact storage
8. A [P2-P3] Personality Persona Engine

# Mid-Term Goals:
1. Compare Two Functions Against Eachother - Essentially A <vs> Operator
2. Contextualize & Identify Nature of Functiona A and Function B ( Ie: Mathmatical )
3. Solve Function A - Solve Function B
4. Create: Ternary Decision Logic
5. Create: Choose Function A or Function B
6. Create: Output Solution
7. Create: Log Analysis In Datastore
8. Create: During Identification Process -> See if PreExisting Data Exists
9. Create: Load Data Into Decision Logic -> Add Weight Factors
10. Create: Choose Best Output
11. Add Anti-Drift Discipline: keep docs, shell identity, tests, and runtime behavior aligned so authored intent does not separate from implemented truth
12. Build a Neural Layer Architecture: add local neural retrieval, reranking, and learning-loop support on top of the deterministic OmniX core
13. Modernize legacy X++ nomenclature into HumanReadable runtime names while preserving old labels as source-history mappings
14. Research and design a lightweight onboard native TCP viewer, TShark-like in operator usefulness but small enough to ship with OmniX without the full Wireshark dependency surface; defer GHOSTLINE-GATE to a future capture-source adapter once its source/API is available
15. Build Neuromorphic Programming Research Track: explore event-driven, spike-like local computation, Backtrace, Backtest, and Back-add learning loops for future OmniX intelligence
16. Build C++ / Python Interop Code Segment: define a guarded bridge where OmniX C++ can call bounded Python analysis modules, exchange typed JSON artifacts, and keep Python execution optional, sandbox-aware, and replayable
17. Build NeuralNetwork + TensorFlow Math Track: learn and operationalize perceptron/MLP fundamentals, simulation-first TensorFlow model training, and local neural signal routing without confusing this with neuromorphic/SNN research
18. Build Native C++ Neural Signal Router: classify TView JSONL with dependency-free math, attach weighted attributions to definitions and decisions, and backtrace how math shaped each data decision
19. Build FreeRTOS / Firmware Communications Research Track: keep a lightweight embedded systems path for future low-level communications, buses, firmware traces, and RTOS-aware local diagnostics without adding embedded dependencies to OmniX core yet
20. Build VI / Vim / Neovim Operator Bridge: detect local editor capability, collect explicit buffer/file context, preview OmniX patches, and support future Neovim RPC integration without making any editor a required core dependency
21. Build Salt-Style Jinja, Node, and Master Layer: support local Jinja artifact workflows, explicit node identity/enrollment, and file-spool master job planning while deferring remote shell, mutation jobs, and network transport until trust policy is stronger
22. Build Local Tensor Framework and DeepNimSec Runtime: load and validate JSON tensor bundles, run tiny MLP traces, ask DeepNimSec/Citizen-AI against local OmniX knowledge context, and capture supervised training examples without remote APIs or heavyweight ML dependencies
   - BLOCKER: Do not start vendor-specific tensor backend work until Mike/Operator studies tensor architecture and records notes in `docs/study/tensor-architecture-notes.md`.
23. Build Slash Dedupe Storage Feature: integrate Blackwell-inspired DeviceFabric concepts into OmniX as a future user-space dedupe, manifest, restore, verify, and compact storage layer
24. Build Twilio Alert Transport And Tenant-Safe Master/Minion Wizard: design outbound-only Twilio alarm notifications and tenant-bound node enrollment so future masters cannot plan or dispatch jobs across company boundaries or perform no-stray mutation.

# Grandiose Goals & Additions ( Not Necessary - Can Go This Route ):
1. Wolfram Alpha API
2. OpenAI API: polish the public-release CLI and shell wizard for `api configure openai` with friendlier guidance, masked confirmation, `.env` preview, next commands, and clear assist-only safety language
3. PyTorch - Python Interop as a later specialization of the guarded C++ / Python bridge
4. Execution Of Python Files & Integration Of Maps

# SideQuests:
1. Falcon SDK Platform Integration: research a guarded connector path for Falcon SDK data, detections, incidents, and response actions while keeping credentials, destructive actions, and cloud calls explicitly operator-controlled
2. Apple Intelligence + Local AI/ML Edge Integration: design a local-only Apple/mobile bridge where Apple Intelligence, App Intents, Shortcuts/Siri, Writing Tools surfaces, and Foundation Models can contribute trusted context, summaries, operator approvals, or mobile-side signals to OmniX while immediate engineering stays focused on native tensors, `mlp-lens`, DeepNimSec local runtime, and local model artifact bridges.
3. Real-Time Speech and Translation Guard: research a phone-out assistant that detects likely mistranslations, wrong-language drift, risky tone/formality, names/numbers mistakes, or "the user is about to say the wrong thing" moments, then alerts through subtle haptics, visual cues, earbuds, or watch taps; keep local-only/offline-first speech and glossary checks as the default path, with OpenAI Realtime prompting as an optional assist reference rather than the authority. Example: "English, SIR, your VITALS and patterns suggest you are about to curse" -> response: "VIBRATE 4 TIMES on SMART WATCH"

# Le Grand Mission:
Localized AI && Local Threat Engine & Local Guardian
![Le Dragon Rogue](https://github.com/NyxCipher/OmniX-Core-Experiment/blob/main/LeDragonRogue.jpg?raw=true)

# Elite Grand Vision:
GPU Enablded Hardware Ie: "DAI™" ( Decentralized Autonomous Interop™ )

or 

...
