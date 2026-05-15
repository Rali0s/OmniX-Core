# Ghostline `gg` Integration

## Summary

This phase vendors Ghostline Gate into OmniX as `gg`. Ghostline remains hackable source inside the OmniX tree, while OmniX wraps it as an internal tool and converts Ghostline audit JSONL into TView-compatible packet/transit evidence.

Ghostline's operating rule remains:

```text
original delivery wins unless modified release is proven safe
```

## Runtime Surface

```sh
omnix gg doctor
omnix gg search --port 1883 --listen-only
omnix gg search --pid mqtt
omnix gg run 7777 127.0.0.1 1883 --protocol-hint mqtt --replace-text patched-payload --audit-json /tmp/gg-audit.jsonl --actions-json /tmp/gg-actions.jsonl
omnix gg audit /tmp/gg-audit.jsonl --out /tmp/gg-tview.jsonl
omnix gg actions /tmp/gg-actions.jsonl
omnix nn route tview /tmp/gg-tview.jsonl
```

`doctor`, `search`, `audit`, and `actions` are non-mutating. Packet mutation only happens through explicit `gg run` arguments or Ghostline rules files.

## Capability Truth

- MQTT, raw-live, and byte-window mutation are active Ghostline paths.
- RabbitMQ, AMQP, Kafka, ActiveMQ, and Azure Service Bus are detection/audit profiles in this slice.
- Ghostline is a relay/control point, not a passive libpcap sniffer.
- TView remains the native packet capture path; `gg audit` turns Ghostline transit decisions into TView-style evidence for neural routing.

## Bridge Codes

Ghostline audit events are mapped into `omnix.tview.packet.v1` JSONL with `source:"ghostline"` and new Simplex-style codes:

- `NET.GHOSTLINE.FRAME_DETECTED`
- `NET.GHOSTLINE.ORIGINAL_RELEASED`
- `NET.GHOSTLINE.MODIFIED_RELEASED`
- `NET.GHOSTLINE.FALLBACK_ORIGINAL`
- `NET.GHOSTLINE.REVIEW_REQUIRED`
- `NET.GHOSTLINE.PARSE_OR_VALIDATION_RISK`

These codes represent queue/consumer packet-transit semantics such as modified release, fallback to original, review-required mutation, and validation risk. GSMg can consume these later, but the first bridge target is TView/Neural routing.
