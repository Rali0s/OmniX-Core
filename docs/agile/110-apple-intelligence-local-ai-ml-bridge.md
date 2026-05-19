# Apple Intelligence + Local AI/ML Bridge

## Summary

Apple Intelligence is a future OmniX mobile/Apple edge bridge, not the immediate runtime target. The near-term path remains local tensor literacy, `mlp-lens`, DeepNimSec as a fully local operator model, and deterministic C++ inference surfaces. Apple-supported APIs can later add mobile context, approval workflows, summaries, and on-device assistance without replacing OmniX as the evidence and execution authority.

## Supported Future Surfaces

Use public Apple-supported integration points only:

- App Intents for exposing approved OmniX actions to Siri, Shortcuts, Spotlight, widgets, and controls.
- Shortcuts/Siri flows for operator-visible approval and mobile-side handoff.
- Writing Tools-compatible text surfaces for draft/report review where appropriate.
- Foundation Models for on-device language understanding or summarization when available.
- Foundation Models adapter training as a later advanced track, subject to Apple entitlement and packaging requirements.

No private APIs, background command execution, secret transfer, or cloud-default behavior belongs in this track.

## Immediate Build Order

1. **Tensor Ground Truth:** add native tensor inspection and validation before larger model formats.
   Target surface: `omnix tensor inspect <bundle.json>`, `omnix tensor validate <bundle.json>`, `omnix tensor run mlp <bundle.json> --input "text"`, and `omnix tensor doctor`.
2. **MLP Lens Upgrade:** evolve `mlp-lens` into a reusable tensor inspection layer for `embedding -> W1/b1 -> activation -> W2/b2 -> logits/softmax`.
3. **DeepNimSec Local Runtime:** tighten Ollama/DeepNimSec local commands for `doctor`, `probe`, `serve`, `run`, `chat`, and `models`.
4. **Local Model Artifact Bridge:** support JSON tensor bundles first, GGUF metadata/read-only inspection second, ONNX later, and safetensors later.
5. **Neural Signal Router Training Path:** keep C++ runtime inference authoritative while allowing optional Python/TensorFlow lab tooling to train or export compatible local artifacts.

## Authority Model

Apple Intelligence may provide user context, summarized mobile-side notes, draft text, operator approvals, or signal handoff. OmniX remains responsible for deterministic evidence, TZE replay, tensor validation, DeepNimSec local provider state, and guarded command authority.

Do not claim Apple Intelligence integration until OmniX has an actual Apple-supported app, intent, shortcut, or Foundation Models bridge. Until then, this is a roadmap and architecture goal.

## Safety Constraints

- Local-first by default; no cloud-required path.
- No secrets in TZE runs, memory, node grains, mobile payloads, or reports.
- No automatic mobile-triggered command execution.
- No Apple private APIs or unsupported system hooks.
- Apple Intelligence output is advisory; OmniX deterministic evidence remains the authority.

## References

- [Apple Intelligence](https://developer.apple.com/apple-intelligence/)
- [Apple Intelligence Get Started](https://developer.apple.com/apple-intelligence/get-started/)
- [App Intents](https://developer.apple.com/documentation/appintents)
- [Foundation Models](https://developer.apple.com/documentation/foundationmodels/generating-content-and-performing-tasks-with-foundation-models)
- [Foundation Models adapter training](https://developer.apple.com/apple-intelligence/foundation-models-adapter/)

