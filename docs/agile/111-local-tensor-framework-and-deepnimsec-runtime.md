# Local Tensor Framework And DeepNimSec Runtime

## Summary

OmniX tensor work starts with local literacy: load, validate, inspect, and trace small tensor bundles before adopting larger model formats. DeepNimSec and Citizen-AI are treated as local operator model profiles that can answer against OmniX local knowledge context and produce supervised training examples, while deterministic OmniX evidence and TZE replay remain the authority.

## Command Surface

```sh
omnix tensor doctor
omnix tensor inspect <bundle.json>
omnix tensor validate <bundle.json>
omnix tensor run mlp <bundle.json> --input "text"
omnix tensor ask --model deepnimsec-omni:latest --kb res/local_glossary.tsv "question"
```

`tensor ask` can capture JSONL training examples with `--training-log <file.jsonl>`. It uses local context only, fixture-backed answers for tests, and optional local Ollama only when the operator passes `--allow-ollama`.

## Model Order

1. JSON tensor bundles for stable local validation.
2. Reusable `mlp-lens` tensor tracing.
3. DeepNimSec/Citizen-AI local model profiles through Ollama.
4. GGUF metadata/read-only inspection.
5. ONNX and safetensors adapters later.

## Vendor API Study Gate

Vendor-specific tensor backend work is blocked until Mike / Operator studies tensor architecture and records notes in `docs/study/tensor-architecture-notes.md`. This gate applies to CUDA, Metal, Apple tensor-hardware paths, Vulkan, ROCm, and similar hardware/backend targets.

Allowed work remains: JSON tensor bundle literacy, `mlp-lens`, `tensor ask`, local KB answers, supervised training capture, and adapter planning. GGUF, ONNX, and safetensors should stay read-only or adapter-first until reviewed.

## Safety

- No remote API calls.
- No claim of full model training from tensor inspection.
- No secrets in training examples.
- Local model answers are advisory and must cite or preserve local context.
