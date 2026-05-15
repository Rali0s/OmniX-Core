# Native C++ Neural Signal Router

## Summary

This phase chooses **C++ ML/runtime first** for OmniX. Core runtime stays dependency-free: no Python, TensorFlow, Eigen, ONNX, LibTorch, or new ML package is required.

The first shipped neural-style runtime is `NeuralSignalRouter`, a pure C++ scorer over TView JSONL events. It answers the guiding question: **How can math better help decide my data decisions?**

Every score must be backtraceable. A label is not enough; OmniX records the evidence, weighted factors, source, and rationale that produced the label.

## Public Interface

```sh
omnix nn route tview <file.jsonl> [--out routes.jsonl] [--compact|--verbose]
```

The command reads `omnix.tview.packet.v1` JSONL emitted by `omnix tview`, extracts local numeric features, and emits an advisory route report. If `--out` is provided, OmniX writes compact `omnix.nn.route.v1` JSONL artifacts for downstream ingestion.

## Runtime Types

- `MathAttribution`: named factor with raw value, weight, contribution, source, and rationale.
- `NeuralFeatureVector`: normalized TView-derived feature counts.
- `NeuralRoutePrediction`: advisory label, confidence, rationale, and math attributions.
- `NeuralRouteReport`: status, input path, packet count, flow count, predictions, warnings, and optional route artifact path.

The same `MathAttribution` structure also applies to definition answers and decision candidates so replay/report can show why an answer or action ranked the way it did.

## Feature Extraction

The router consumes local evidence only:

- Simplex code counts: `NET.TCP.CONTROL`, `NET.TCP.HTTP_PLAINTEXT`, `NET.TCP.TEXT_UTF8`, `NET.TCP.TLS_OPAQUE`, `NET.TCP.OPAQUE_PAYLOAD`, `NET.TCP.PARSE_ERROR`
- Payload packet count and total payload bytes
- Plaintext present or absent
- Known local service port versus unknown port
- Control-only flow versus payload flow
- Flow count and repeated packet totals

## Classifier

Phase 1 uses a fixed-weight linear scorer / softmax-style ranker. Labels are advisory:

- `benign_control`
- `plaintext_http`
- `tls_opaque`
- `unknown_service`
- `suspicious_port`
- `needs_human_review`

The router never performs defensive actions automatically. It may inform later `defend diag`, case review, SIEM mapping, or MITRE work, but deterministic safety gates remain authority.

## Backtrace Contract

`tze replay` and `tze report` show:

- `x.Neural.SignalRouter`
- input artifact path
- top label and confidence
- weighted contributors
- source evidence summary
- memory writes

Definition and decision reports also show math attribution so an operator can see when source authority, exact matching, domain match, retrieval score, validity, prior success, or confidence shaped the result.

## Persistence

OmniX stores compact summaries and math factors only. It does not store raw packet payloads, full TView JSONL contents, or full reasoning chains in history.

## Future Bridge

Future Python/TensorFlow labs may emit compatible `omnix.nn.route.v1` route artifacts. C++ remains the runtime consumer and authority for this phase.

## Acceptance

- `omnix nn route tview fixture.jsonl --compact` returns a route summary.
- Control-only packet fixtures classify as `benign_control`.
- Plaintext HTTP fixtures classify as `plaintext_http`.
- TLS or opaque fixtures classify as `tls_opaque` or `unknown_service`.
- Parse errors or unusual unknown-port payloads classify as `needs_human_review` or another evidence-bound warning label.
- `tze replay latest` includes `x.Neural.SignalRouter` and math attribution.
- Existing `omnix nn math perceptron` behavior remains green.
- `cmake --build build -j4` and `./build/omnix_tests` pass.
