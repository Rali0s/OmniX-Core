# Proactive Infrastructure Thresholds

## Summary

This phase adds a native OmniX Thresholds layer for **Proactive Infrastructure Triage and Remediation**.

The purpose is not a generic monitoring dashboard. The purpose is to turn an alarm into disciplined Engineer Infra workflow:

```text
Alarm -> Context -> Triage -> Remediation Recommendation -> Evidence -> Escalation -> Threshold Tuning
```

The first fixture is RabbitMQ Queue `XXB` on `XYZ_CLASSC1_CUST8`, but the model is global enough for queues, apps, databases, connection pools, mounts, storage, hosts, CI/CD, and controlled outage windows.

## Runtime Surface

```sh
omnix tool thresholds -- evaluate res/thresholds/rabbitmq-xxb-incident.json
omnix tool thresholds -- evaluate res/thresholds/rabbitmq-xxb-incident.json --out /tmp/evidence.json --jira-out /tmp/escalation.md
omnix tool thresholds -- evaluate res/thresholds/rabbitmq-xxb-incident.json --execute
omnix tool thresholds -- gsmg res/thresholds/gsmg-rabbitmq-xxb.json
omnix tool thresholds -- gsmg res/thresholds/gsmg-outage-window-db-guard.json --out /tmp/gsmg-evidence.json --jira-out /tmp/gsmg.md
```

Default mode is recommendation-only. `--execute` is prompt-gated and limited to explicit allowlisted local runbook action types.
`gsmg` mode is evaluation-only in this slice; it builds the generic signal ground without running runbooks.

## Domain Model

The first implementation models:

- `Asset`
- `Signal`
- `QueueProfile`
- `AppOwnershipMap`
- `BaselineProfile`
- `SeasonalOverride`
- `ErrorSignature`
- `ThresholdRule`
- `DecisionPolicy`
- `RunbookAction`
- `EvidenceBundle`
- `EscalationPacket`

A threshold is a decision boundary, not a number. OmniX evaluates metric value, asset context, queue ownership, customer/site context, time window, seasonal behavior, known error signatures, data-loss risk, remediation action, and escalation requirement together.

## Surgical Scenario

When RabbitMQ `2B` reports Queue `XXB` stopped reporting, OmniX works black-box:

- identify the queue owner and service
- inspect service status, logs, metrics, queue state, and known signatures
- avoid source-code inspection
- decide whether RabbitMQ, app worker, memory, storage, server resource, or seasonal load is most likely
- recommend a safe runbook action only when policy permits
- generate evidence JSON and a Jira-ready escalation packet

## Global Doctrine

The same model applies beyond RabbitMQ:

- DB and connection-pool incidents
- mount and detached block storage safety
- CI/CD degradation
- server-class maintenance
- outage-window preflight
- load balancer and HTTPS ingress control
- sensor muting and alarm-storm avoidance

Controlled shutdown and threshold triage are paired concepts. Thresholds ask when the system is drifting toward failure. Controlled shutdown asks how to intentionally bring the system down without creating failure.

## GSMg: Generic Signal Model Ground

The RabbitMQ Queue `XXB` evaluator remains the surgical example. GSMg builds outward from it by moving the input from RabbitMQ-shaped fields into a generic artifact:

```json
{
  "assets": [
    { "id": "XXB", "kind": "queue" },
    { "id": "abc-worker.service", "kind": "service" }
  ],
  "signals": [
    { "id": "queue.depth", "asset": "XXB", "value": 642, "previousValue": 388, "unit": "messages" },
    { "id": "memory.usage", "asset": "XYZ_CLASSC1_CUST8", "value": 91, "unit": "percent" },
    { "id": "consumer.count", "asset": "XXB", "value": 0 }
  ],
  "policies": [
    {
      "id": "memory.heap-pressure.queue-consumer-stop",
      "if": [
        { "signal": "memory.usage", "operator": ">=", "value": 85 },
        { "signal": "queue.depth", "trend": "rising" },
        { "signal": "consumer.count", "operator": "==", "value": 0 }
      ],
      "then": {
        "likelyCause": "app_worker_memory_exhaustion",
        "severity": "critical",
        "decision": "recommend_runbook",
        "recommendation": "Restart the worker after operator review."
      }
    }
  ]
}
```

This means OmniX evaluates signal IDs and policy boundaries instead of C++ members such as `queueDepth`, `consumerCount`, or `rabbitmqNodeHealthy`.

GSMg is intended to fit future metric ingestion from `ToolFlowInterpreter` and related evidence sources:

- RabbitMQ, Kafka, ASB, MQTT, and other queue/consumer suites
- database connection pools and active writer counts
- load balancer and HTTPS ingress state
- application service status and log signatures
- FSTAB mounts, detached block storage, data-brick health, and backup checkpoints
- CI/CD workflow state, deploy gates, rollback state, and release health
- outage-window quiescence gates, sensor muting, drain policies, and data-loss guards

The outage-window fixture models controlled shutdown as a threshold problem in reverse. Before stopping a database, GSMg checks whether external writes, app connection pools, active DB writers, unacked queue messages, or mount writers are still present. If they are, the policy returns `block_unsafe_action` rather than pretending the shutdown is safe.

## Acceptance

- `thresholds` resolves through `tool locate` and `tool doctor`.
- Queue `XXB` fixture identifies app-worker memory exhaustion when `EYZ-47281`, high RAM, rising depth, and zero consumers appear.
- Seasonal overrides change queue-depth boundaries.
- Safe restart is recommended only when RabbitMQ is healthy, messages are persistent, consumers are down, and data-loss estimate is acceptable.
- Evidence JSON and Jira Markdown are generated locally.
- Unknown signatures escalate instead of recommending blind remediation.
- Prompt-gated execution never runs unless exact confirmation is typed.
- GSMg fixtures evaluate generic `assets[]`, `signals[]`, `policies[]`, and `runbooks[]` without removing or weakening the surgical RabbitMQ fixture.
