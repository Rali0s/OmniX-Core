# Immediate Goals Completion: Simplex, Ingestion, Defense, Soak, Persona

## Summary
- Reframe packet analysis as native, Wireshark-like local primitives rather than direct Wireshark library binding.
- Make `omnix tview` useful to both humans and ingestion pipelines through verbose text previews plus JSONL packet events.
- Treat DeepNimSec/Ollama as a provider connector to verify, while MITRE JSON and SIEM mapping remain a separate local data pipeline.
- Keep defensive Course-of-Action diagnostic-first: collect evidence and propose manual actions, but do not kill, close, or mutate system state.
- Stabilize `x.Preprocessor` / `x.PostProcessor` through soak and regression runs, then keep persona behavior playful but safety-bounded.

## Runtime Contract
- TView packet events use `omnix.tview.packet.v1` JSONL and include flow metadata, payload lengths, bounded previews, provenance, and Simplex codes.
- Simplex codes start with `NET.TCP.CONTROL`, `NET.TCP.HTTP_PLAINTEXT`, `NET.TCP.TEXT_UTF8`, `NET.TCP.TLS_OPAQUE`, `NET.TCP.OPAQUE_PAYLOAD`, and `NET.TCP.PARSE_ERROR`.
- Defensive diagnostics use `omnix defend diag <cpu|memory|logs|pid|port> [target]` and natural-language routing for safe evidence collection.
- Persona records may affect display language and operator identity responses, but never command authority, safety gates, routing, or guardrails.
- Persona modes start with `Premise`, `Cynic`, `Professional`, and `Neutral`, exposed through `omnix persona mode <mode>` and shell shortcuts like `premise mode`.

## Acceptance Checks
- `omnix tview pcap <file> --port <port> --out <file>.jsonl --verbose` emits readable text and versioned JSONL.
- Ingesting TView JSONL creates packet capture, flow, and payload evidence objects.
- `omnix defend diag cpu` and related modes complete without destructive action and print manual next steps.
- `omnix persona mode premise` persists display-only persona settings and `Who am I?` reports the active mode.
- `./scripts/soak_omnix.sh` runs repeatable pre/post, definition, provider, TView fixture, and defensive diagnostic checks.
- DeepNimSec remains optional: `./scripts/omnix_deepnimsec.sh --probe --compact` verifies connector health without making deterministic behavior model-dependent.

## Deferred
- Direct Wireshark/TShark library binding.
- Automatic PID kill, firewall changes, or service shutdown.
- MITRE ATT&CK JSON ingestion and SIEM export object modeling.
- Persona-driven authority or autonomous behavior changes.
