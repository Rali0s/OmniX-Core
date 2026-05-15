# V2 Pre/Post Runtime, Teaching Precedence, Ollama Hardening, and Local Retrieval Foundations

## Summary
- This file is the active follow-on contract for the current `v2` completion slice.
- The delivery order is:
  1. `x.Preprocessor` / `x.PostProcessor`
  2. operator teaching protocol + source precedence
  3. Ollama reliability hardening
  4. local C++ retrieval and reranking
- The slice stays deterministic-first, keeps Ollama optional, and does not add a new teaching CLI.

## Runtime Contract
- `x.Preprocessor` runs before dispatch and produces:
  - normalized prompt
  - token set
  - instruction-family hint
  - bounded `uAC` traits
  - cache budget and epoch markers
  - recovery-relevant hints
- `x.PostProcessor` runs after dispatch and produces:
  - `PostSuccess`, `PostFail`, `PostBlocked`, or `PostClarify`
  - compact final artifact summary
  - provenance
  - retention decision
  - explicit transient-chain pruning metadata
- Replay, chain, and report views must show both the preprocessor state and postprocessor outcome.

## Teaching and Definition Contract
- `res/local_glossary.tsv` remains the primary operator teaching surface.
- Exact-match teaching precedence is:
  1. operator-authored memory or glossary with matching domain
  2. operator-authored memory or glossary without domain
  3. stored final artifacts marked `memory_artifact`
  4. macOS system dictionary
  5. opt-in Webster fallback
  6. unresolved / clarification
- Persisted system-dictionary and Webster results are stored as `reference_cache`.
- `reference_cache` may help later retrieval, but it must never outrank later operator teaching.
- Bare-query domain behavior:
  - one scoped operator meaning and no unscoped meaning: use it
  - multiple scoped operator meanings and no unscoped meaning: clarify with domains

## Ollama Reliability Contract
- `provider probe` must distinguish:
  - model missing
  - model listed but unusable
  - ready
- `deepnimsec-omni:latest` should recommend `./scripts/omnix_deepnimsec.sh --refresh-model` when stale or missing.
- `scripts/create_deepnimsec_ollama_model.sh` must validate the base model from the Modelfile and print the exact recovery pull command if absent.
- `scripts/omnix_deepnimsec.sh --refresh-model` must rebuild and then run `provider probe`.

## Local Retrieval Contract
- Retrieval remains local C++ only in this phase.
- Candidate sources are:
  - `res/local_glossary.tsv`
  - persisted definitions
- Retrieval uses normalized tokens plus hashed character trigrams to build sparse local similarity signals.
- Retrieval runs only after exact operator, artifact, system-dictionary, and Webster checks miss.
- Retrieval may recover near local matches, but it must not override exact operator or system hits.

## Validation Targets
- `x.Preprocessor` and `x.PostProcessor` appear in replay and report output.
- History stores only compact final artifacts, not recursive chain text.
- A taught `Steve Jobs|Biography|...` entry beats the system dictionary on exact bare-query lookup when it is the only taught meaning.
- Multiple scoped taught meanings for the same bare term produce clarification.
- `provider probe` reports non-ready for a listed-but-unusable custom Ollama model.
- Local retrieval can recover a close taught concept miss without introducing model dependence.
