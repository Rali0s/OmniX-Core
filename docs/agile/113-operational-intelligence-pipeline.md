# Vuplus Gate Operational Intelligence Pipeline

## Summary

This slice defines **Vuplus Gate**, the OmniX operational intelligence segment named after *Vulpes vulpes*, the red fox. The command sequence class is `vg`.

Vuplus Gate is the fox-gate for operations evidence: fast local sensing, cautious route-finding, dependency-aware correlation, and legible next-action guidance.

```sh
omnix vg doctor
omnix vg shape <ops-artifact.json> [--out shaping.json] [--compact|--verbose]
omnix vg explain <ops-artifact.json> [--out report.json] [--compact|--verbose]
omnix vg explain <ops-artifact.json> --learn-shape [--out report.json]
omnix vg correlate <ops-artifact.json> [--dependency-map file] [--compact|--verbose]
omnix vg compare <recovery-comparison.json> [--compact|--verbose]
omnix vg cab <ops-artifact.json> [--out alarm-cab.json] [--compact|--verbose]
```

The pipeline is:

```text
Logs / Alerts / Notes
-> Normalized Events
-> Threshold + Dependency Correlation
-> Incident Explanation
-> NEXT Recommendation
-> Recovery Comparison
-> Operational History
```

The goal is not another monitoring dashboard. The goal is a local-first Engineer Infra layer that turns messy logs, alerts, notes, thresholds, dependency maps, and recovery history into disciplined incident explanation and next-action guidance.

V1 is read-only and recommendation-only. No remediation execution, external SIEM API calls, package installs, service restarts, shutdown actions, or packet mutation are part of Vuplus Gate V1.

Phase 2 adds Windows Event Viewer retention inspection, syslog/lastlog session correlation, local heuristic/RUM signal encoding, execution-topology metadata, and Alarm CAB JSON artifacts.

Phase 3 adds SIEM shaping intelligence and encrypted-evidence custody mapping.

## Doctrine

OmniX should work from observable operations evidence:

- logs from syslog, auth logs, journal output, lastlog, macOS unified-log excerpts, and future Windows Event Viewer adapters
- Windows Event Viewer channel metadata, including whether critical channels meet the 1GB evidence-retention floor
- Elastic-style SIEM JSON shaped into local events and alerts
- encoded heuristic/RUM-like behavior signals for latency, error bursts, session anomalies, endpoint failure patterns, and user impact
- existing Thresholds and GSMg policies
- dependency maps across services, queues, databases, mounts, package/runtime dependencies, ingress, producers, and consumers
- outage-window shutdown order and data-loss guards
- operator notes, commands observed, validation notes, and remediation outcomes
- TZE replay, Recursive Why/Diff, `omnix next`, and future Versus Comparator outputs

The pipeline should answer:

```text
What happened?
Which asset, user, service, queue, DB, mount, package, or workflow changed?
Which threshold or dependency boundary did it cross?
What prior incident or recovery does this resemble?
What should the operator do next?
What evidence should be preserved for escalation?
```

## Explainability Contract

Every Vuplus Gate result should make the operational answer reviewable:

- `why`: concise incident explanation grounded in local evidence.
- `based on what signals`: exact signals, log excerpts, alerts, threshold matches, dependency surfaces, or recovery-path deltas.
- `confidence`: deterministic score from signal quality, source authority, threshold match, dependency match, and historical match.
- `historical correlation`: prior case, threshold fixture, GSMg pattern, TZE diff, Recursive Why/Diff, or future Versus Comparator match.
- `operational blast radius`: affected asset graph such as app, queue, DB, mount, ingress, host, package/runtime surface, customer, and site.
- `rollback impact`: what gets safer or riskier if the operator waits, reverses, restarts, shuts down, or escalates.

V1 output uses:

```json
{
  "event_type": "omnix.vg.explain.v1",
  "segment": "Vuplus Gate",
  "status": "vg_explained",
  "why": "...",
  "signals": [],
  "confidence": 0.0,
  "historicalCorrelation": "...",
  "operationalBlastRadius": "...",
  "rollbackImpact": "...",
  "nextAction": "...",
  "remediationMode": "recommendation_only",
  "executionTopology": "standalone_local_node",
  "keyPairs": [],
  "eventViewerRetention": [],
  "sessionCorrelations": [],
  "heuristicSignals": [],
  "shapedFields": [],
  "shapingRules": [],
  "keyCustody": null,
  "alarmCab": null
}
```

## Event Viewer Retention

Vuplus Gate and `defend detect eventviewer` inspect Windows Event Viewer channel metadata when available. Critical channels should retain at least 1GB:

```text
Security, System, Application >= 1073741824 bytes
```

If a channel is below that floor, OmniX emits a CAB recommendation only. It must not change channel settings automatically because commands such as `wevtutil sl Security /ms:1073741824` require elevated Administrator or SYSTEM authority and change-control approval.

On non-Windows hosts, `defend detect eventviewer` returns an unsupported-platform warning unless a local fixture is supplied with `--source`.

## Syslog And Lastlog Correlation

Syslog, auth logs, `who -a`, `last`, and `lastlog` can disagree. Vuplus Gate should join them into `sessionCorrelations` so an operator can see actor, source, tty/session, first seen, last seen, evidence refs, confidence, and anomaly rationale.

Odd-hour admin activity is not automatically compromise. It is verification evidence: “my admin is usually asleep right now” becomes a confidence/rationale factor before containment.

## RUM And Heuristic Signals

RUM starts as local encoded behavior data, not live web beacons. `heuristicSignals` can describe p95 latency spikes, error bursts, session drop patterns, navigation anomalies, endpoint failures, and user-impact indicators.

Citizen-AI may later classify these heuristic bundles, but deterministic Vuplus Gate remains the evidence authority.

## Analytics Pipe And Topology

OmniX remains one primary binary. The first analytics scaling model is a bounded internal worker pool for parsing/correlation/artifact shaping, defaulting to deterministic single-thread operation until explicitly enabled.

Every report includes `executionTopology`, defaulting to `standalone_local_node`. Other future values are `approved_node`, `master`, and `siem_adjacent_collector`; none are assumed without configuration.

## Alarm CAB JSON

`omnix vg cab <artifact> --out alarm-cab.json` writes a GUI/Web-friendly Change/Alarm Control Board recommendation. CAB output is advisory and contains alarm id, affected assets, signals, threshold window, proposed change, approval requirement, blast radius, rollback impact, validation checks, owner, timestamps, and recommendation status.

## SIEM Delimiter And Meta-Tag Gotchas

Industry SIEM evidence is often only partly normalized. One field may be valid JSON while another field is a 250-character dump such as `[data] queue=XXB consumer_count=0 error=EYZ-47281 service=abc-worker.service`.

Vuplus Gate must therefore treat SIEM input as two layers:

- JSON boundary extraction: locate JSON keys, their matching values, and the start/end offsets of the value span in the source artifact.
- Embedded pair extraction: inspect blob fields such as `data`, `meta`, `message`, `original`, `event.original`, and similar tags for delimiter-separated `key=value` pairs.
- Evidence preservation: keep the extracted key, value, source field, and value boundary so reports can explain why a hidden meta-tag changed the operational answer.
- No blind trust in SIEM shaping: if the SIEM sorted top-level fields but buried critical details inside `[data]`, OmniX should still surface those details before threshold, dependency, or recovery comparison logic runs.

This is intentionally narrow in V1. It favors common `key=value` operational blobs and valid local JSON artifacts. Future adapters can add escaped payload recovery, multiline event fragments, CEF/LEEF parsing, Windows Event Viewer XML, and vendor-specific SIEM shapes.

## SIEM Shaping Intelligence

`omnix vg shape <artifact>` turns extracted key/value evidence into typed, lineage-preserving fields:

```json
{
  "field": "consumer_count",
  "value": "0",
  "type": "integer",
  "source": "embedded:event.original",
  "lineage": "embedded:event.original[32..33]",
  "semanticMeaning": "queue consumer count",
  "mappedSignal": "consumer.count.zero",
  "confidence": 0.91
}
```

`--learn-shape` on `vg explain` includes reusable local shaping-rule proposals in the report. It does not silently mutate SIEM pipelines or install hidden global mappings.

Reusable rules are auditable JSON:

```json
{
  "match": {
    "source": "embedded:event.original",
    "field": "consumer_count"
  },
  "type": "integer",
  "semanticMeaning": "queue consumer count",
  "mappedSignal": "consumer.count.zero"
}
```

## Encrypted SIEM And Key Custody

OmniX may map encrypted evidence, but it must not hunt, dump, print, or exfiltrate private keys.

When encrypted evidence is detected, Vuplus Gate emits `keyCustody` with:

- visible anchor such as server or log source
- route hypothesis across server, queue, consumer, app, and log-source
- allowed metadata such as certificate fingerprint, mtime, owner, path class, and key reference id
- forbidden evidence such as private key material, secret contents, and decrypted payload without approval
- next action requiring an approved operator or approved local decryptor workflow

This keeps SOC routing useful while preserving secret-handling boundaries.

## Local Schemas

`OperationalEvent`

- timestamp
- source
- host
- actor
- service
- event type
- severity
- evidence excerpt
- content hash
- confidence

`OperationalAlert`

- alarm id
- severity
- asset
- customer/site
- trigger
- thresholds
- source system

`OperationalIncident`

- linked events
- linked alerts
- matched thresholds
- dependency map reference
- likely cause
- evidence summary
- next action

`OperationalNote`

- operator
- timestamp
- note text
- commands observed
- reasoning
- validation notes

`RemediationRecommendation`

- action id
- action type
- recommendation text
- safety gates
- data-loss risk
- validation checks
- escalation requirement

`DependencyMap`

- services
- queues
- databases
- mounts
- package/runtime dependencies
- ingress
- producers
- consumers

`ShutdownSequencePlan`

- maintenance state
- mute scope
- ingress drain
- queue drain
- app stop
- DB stop
- mount detach
- restart order

`RecoveryComparison`

- successful path
- failed path
- missing step
- wrong order
- skipped dependency
- outcome delta

`AlarmCabRecommendation`

- alarm id
- recommendation status
- proposed change
- approval requirement
- affected assets
- signals
- threshold window
- blast radius
- rollback impact
- validation checks
- owner
- timestamp

## Correlation Flow

1. Ingest raw evidence through current `omnix ingest`, `defend detect`, TView/Ghostline artifacts, threshold outputs, or operator notes.
2. Normalize evidence into operational events and alerts without losing source provenance.
3. Correlate events against threshold rules, GSMg policies, dependency maps, outage gates, and prior outcomes.
4. Score operational history with value weights: severity, source authority, recency, recurrence, customer/site context, data-loss risk, and confirmed outcome.
5. Explain the incident in operator language: timeline, evidence, threshold boundary, dependency impact, likely cause, confidence, and next action.
6. Route the immediate recommendation through existing `omnix next`.
7. Compare successful and failed recoveries through TZE diff, Recursive Why/Diff, and a future Versus Comparator.

## Dependency Maps

Dependency maps must cover live incidents and planned outage windows. They should connect:

- external ingress and load balancers
- Apache/HTTPS hosts and front-end services
- app services, schedulers, workers, and cron producers
- queues, consumers, producers, unacked messages, and dead-letter paths
- DB services, connection pools, active writers, and replication state
- mounts, FSTAB entries, detached block storage, data directories, and backup volumes
- package/runtime surfaces such as `apt`, `npm`, `pnpm`, `wget`, `curl`, `brew`, PowerShell, and Windows package manager

Shutdown sequences use the same map in reverse safety order:

```text
maintenance pending
-> controlled internal sensor mute
-> ingress drain
-> stop producers
-> drain queues
-> stop consumers/apps
-> close connection pools
-> stop databases
-> flush/unmount storage
-> shutdown hosts
```

Startup validates the reverse order before client traffic is reopened.

## Remediation Boundary

Vuplus Gate V1 is recommendation-only.

OmniX may recommend a runbook, validation gate, escalation packet, or shutdown guard, but it must not run package installs, service restarts, remote commands, packet mutation, firewall changes, database shutdowns, or storage detaches from this pipeline.

Future remediation execution must remain allowlisted, exact-confirmation gated, validation-bound, and TZE-replayable.

## Fixtures

The first fixture set lives under `res/ops/`:

- `elastic-siem-rabbitmq-xxb.json`: Elastic-style SIEM event and alert shaping.
- `elastic-siem-metatag-blob.json`: SIEM/meta-tag blob fixture where queue, consumer, service, memory, and error-signature facts are packed into one `event.original` string.
- `windows-eventviewer-retention-low.json`: Windows Event Viewer fixture where one or more channels are below the 1GB retention floor.
- `windows-eventviewer-retention-healthy.json`: Windows Event Viewer fixture where critical channels meet the 1GB retention floor.
- `syslog-lastlog-correlation.json`: syslog/auth evidence paired with lastlog/who-style session evidence.
- `rum-heuristic-behavior.json`: local encoded RUM/behavioral heuristic fixture for future Citizen-AI pattern recognition.
- `encrypted-siem-custody.json`: encrypted SIEM evidence fixture with server anchor, queue/consumer route, certificate-reference metadata, and no private key material.
- `local-auth-syslog-lastlog-sample.log`: bounded local syslog/auth/lastlog-style evidence.
- `package-manager-activity.json`: package and script activity across `apt`, `npm`, `pnpm`, `wget`, PowerShell, Windows package manager, and `brew`.
- `dependency-map-outage-window.json`: dependency and shutdown ordering for ingress, app, queue, DB, mount, storage, and package/runtime surfaces.
- `recovery-comparison-worker-restart.json`: successful vs failed recovery comparison for a queue/worker incident.

The scale vertical fixture set lives under `res/ops/verticals/`:

- `scale-verticals-combined.json`: mixed scale artifact for network, Kubernetes, load balancer, Ansible, Terraform/GCP, storage, and OmniX master/minion mapping after SIEM shaping.
- `network-map-scale.json`: offline network map with segments, redundancy groups, OmniX master/minion anchors, queues, DBs, mounts, and load-balancer routes.
- `kubernetes-docker-context.json`: nested context for Windows/operator shell to Kubernetes cluster to pod/container runtime.
- `load-balancer-front-back.json`: frontend/backend routing, target group health, drain state, and rollback impact.
- `ansible-inventory-scale.json`: inventory/group/playbook/role mapping for desired-state analysis without Ansible execution.
- `terraform-gcp-compute-plan-shape.json`: future Google Compute/Terraform resource shape for analysis only, with no provider calls or credentials.
- `omnix-minion-neighbor-map.json`: CDP-style neighbor map for future OmniX node/master topology.
- `storage-cloud-local-map.json`: local mount, disk, persistent volume, bucket, and shutdown-order dependency fixture.

These fixtures are explicitly offline. They exist so Vuplus Gate can learn and validate vertical field typing, lineage, blast-radius hints, and future `infra map` readiness before OmniX touches real cloud or cluster APIs.

## Acceptance

- The roadmap links to this spec.
- `omnix vg doctor --compact` reports Vuplus Gate readiness and recommendation-only mode.
- `omnix vg explain res/ops/elastic-siem-rabbitmq-xxb.json --compact` includes why, signals, confidence, historical correlation, blast radius, rollback impact, and next action.
- `omnix vg explain res/ops/elastic-siem-metatag-blob.json --compact` extracts hidden SIEM/meta-tag `key=value` pairs such as `queue=XXB`, `consumer_count=0`, and `error=EYZ-47281`.
- `omnix defend detect eventviewer --source res/ops/windows-eventviewer-retention-low.json --compact` emits a CAB recommendation and does not mutate Windows settings.
- `omnix vg cab res/ops/windows-eventviewer-retention-low.json --out alarm-cab.json --compact` writes GUI-friendly Alarm CAB JSON.
- `omnix vg explain res/ops/syslog-lastlog-correlation.json --compact` includes session correlation evidence.
- `omnix vg explain res/ops/rum-heuristic-behavior.json --compact` includes heuristic/RUM-like behavior signals without live web beacon integration.
- `omnix vg shape res/ops/elastic-siem-metatag-blob.json --compact` emits typed shaped fields and reusable shaping rules.
- `omnix vg explain res/ops/elastic-siem-metatag-blob.json --learn-shape --compact` includes shaped fields and rule proposals while preserving the normal incident explanation.
- `omnix vg explain res/ops/encrypted-siem-custody.json --compact` emits key-custody metadata and does not expose private key material.
- `omnix vg shape res/ops/verticals/scale-verticals-combined.json --compact` maps Kubernetes, container, load-balancer, Ansible, Terraform/GCP, storage, and OmniX node/master fields into typed shaped signals.
- `omnix vg explain res/ops/verticals/kubernetes-docker-context.json --learn-shape --compact` shows orchestration/container context without running `kubectl` or Docker.
- `omnix vg correlate res/ops/verticals/load-balancer-front-back.json --dependency-map res/ops/verticals/network-map-scale.json --compact` correlates routing/blast-radius context without touching a real load balancer.
- `omnix vg shape res/ops/verticals/terraform-gcp-compute-plan-shape.json --compact` recognizes Terraform/GCP resource-shape fields while preserving analysis-only boundaries.
- `omnix vg correlate res/ops/package-manager-activity.json --dependency-map res/ops/dependency-map-outage-window.json --compact` identifies package/runtime surfaces and odd-hour risk context.
- `omnix vg compare res/ops/recovery-comparison-worker-restart.json --compact` reports successful path, failed path, missing steps, wrong order, and next action.
- TZE replay/report includes `x.Vuplus.Gate`.
- The spec names existing OmniX surfaces: `ingest`, `defend detect`, `thresholds`, `case`, `incident`, `why`, `next`, `tze diff`, and the future Versus Comparator.
- Fixtures cover logs, alerts, incidents, notes, dependency maps, remediation recommendations, shutdown sequences, and recovery comparison.
- The remediation stance remains advisory.
- Elastic/SIEM JSON remains a local adapter shape, not an external integration.
