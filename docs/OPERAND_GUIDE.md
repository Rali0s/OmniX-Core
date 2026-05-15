# Operand Catalogue (Initial Pass)

This document summarizes the recurring operands and namespaces that appear in
`tze.cpp`. The pseudo code mixes terminology from C/C++, C#, shell scripting,
and bespoke DSL keywords. The goal of this catalogue is to provide a first-pass
interpretation of the vocabulary so that we can begin carving out real library
interfaces around the original intent of the program.

## Methodology

1. Extracted every token that begins with `x` or `X` to surface custom
   operations that are not part of the C++ standard library.
2. Clustered the tokens by the context in which they are used (cache, security,
   networking, storage, etc.).
3. Wrote human-readable descriptions for the most frequently referenced
   operands that form the "spine" of the pseudo program.
4. Proposed initial library responsibilities for each cluster so that future
   refactors can grow into normal C++ projects incrementally.

The catalogue below focuses on the operations that drive the early
`Build Cmake` scenario and the associated security workflow. Follow-up passes
can extend the table with additional domains (language detection, Omni network
controls, etc.).

Current operator-facing names are tracked in
[Legacy X++ Translation Map](agile/99-legacy-xpp-translation-map.md). This
guide keeps the original operands visible as source-history aliases.

## Core cache & storage operands

| Operand | Observed usage | Proposed responsibility |
| --- | --- | --- |
| `xProcessingCache(target)` | Legacy alias for `Cache.PrepareWorkspace`; bootstraps scratch storage before the engine inspects a request such as `"Build CMake"`. | Entry point that prepares cache state and orchestrates subsequent storage calls. Lives in a cache coordination module. |
| `xize(estimatedSize)` | Immediately follows `xProcessingCache` to size working storage based on the amount of context that will be cached. | Utility that estimates byte requirements for a cache cell. Should move into a `tze::storage::Sizing` helper. |
| `xCell_Create(spec)` | Creates either a temporary cell (`xMap_Temp`) or a persistent cell depending on whether the run is a "first run". | Storage allocator responsible for provisioning directories or memory mapped regions. |
| `xProcessingDefine(useStorage)` | Declares how the cache can be consumed after it is created. Often followed by definitions such as `seek_Unbound`. | Configuration API that toggles cache behaviour (retention policy, locking strategy, destruction semantics). |
| `x.Destroy(mode)` | Tears down cache content after a success or failure. Modes such as `PostSuccess`/`PostFail` hint at selective persistence. | Cache cleanup primitive that should live next to `xProcessingCache`. |

## Query & preference operands

| Operand | Observed usage | Proposed responsibility |
| --- | --- | --- |
| `x.Define.Low(aZ::n)` | Reads an indexed instruction (e.g., `aZ::1 == Build`). | Decoder that maps symbolic slots (`aZ::1`) to high-level commands. |
| `x3m::"Build"` | Appears to invoke an external search/knowledge mechanism for the string literal. | Knowledge fetcher (searching Google/Wikipedia) that resolves definitions for the operand. |
| `x.DisplayPriorityProcessingGate()` (`x.DPPG`) | Legacy alias for `Knowledge.EvidenceRanking`; presents ranked reference material and records administrator preferences. | User-facing priority engine that queries, presents, and stores preference orderings. |
| `x.DisplayFeedBackLoop()` (`x.DFBL`) | Legacy alias for `Memory.FeedbackReview`; reviews prior responses to similar questions to improve recall efficiency. | Feedback/learning module that associates cache keys with historical answers. |
| `x.Return(value)` | Returns computed sizing or lookup results back to the cache coordinator. | Generic return wrapper that packages values for storage; ultimately can become `return` statements or strongly typed results. |
| `x.Store(data -> destination)` | Legacy alias for `Memory.StoreArtifact`; persists results into `Storage.Temporary` or `Storage.Permanent`. | Storage API for writing structured records. |

## Security & governance operands

| Operand | Observed usage | Proposed responsibility |
| --- | --- | --- |
| `x.Comms(target)` | Requests administrator intervention (e.g., `PrioritizeNow`). | Communications façade for raising human approvals or alerts. |
| `x.Security` | Validates the caller ("Are You Admin?"). | Authentication guard that integrates with OS or custom identity providers. |
| `xX_Kill.All()` | Emergency kill switch that wipes volatile state on failed authentication. | Security response routine that revokes access and cleans temporary buffers. |
| `x.C_P.1()`, `x.C_P.2()`, `x.C_P.3()` | Legacy sequential phases of an administrative workflow, culminating in feedback loop analysis. | HumanReadable runtime stages such as `SourceIntake`, `EvidenceRanking`, `RecipeDraft`, and `ValidateRepairStore`. |
| `x.superAdmin()` / `x.lockOut()` | Transition to high-security mode when anomalies appear in attribution ranking. | Escalation path that triggers lockdown scripts and secure communication steps. |

## Data map namespaces

The pseudo code references multiple hierarchical maps. These appear to be
persistent stores rather than operations. The most common ones include:

* `xMap_Temp`: Ephemeral working directory used by the cache pipeline.
* `xMap_Perm`: Canonical persistent map. Variants such as
  `xMap_Perm_AdminProcessingGate` and
  `xMap_Perm_Prioritys.SearchExtranet` likely represent logical tables.
* `xMap_Core`: Root of the core system data, accessed only in elevated modes.

These should be translated into typed repositories (e.g.,
`Storage::TempMap`, `Storage::PermanentMap`) with schemas documented in future
iterations.

## Next steps

1. Translate the catalogue into C++ interfaces (see `include/tze/operand.hpp`
   alongside the modular headers in `include/tze`, such as
   `include/tze/processing_engine.hpp`).
2. Back the interfaces with stub implementations that simply log intent. This
   allows tests to validate the orchestration logic before real systems are
   built. The first orchestration pass now lives in the dedicated modules under
   `src/cache_coordinator.cpp`, `src/knowledge_engine.cpp`,
   `src/security_manager.cpp`, and `src/processing_engine.cpp`.
3. Continue expanding the glossary—particularly the language detection and
   Omni-network operators—once the cache and security foundations are stable.

This document will evolve as we perform deeper dives into each subsystem.
