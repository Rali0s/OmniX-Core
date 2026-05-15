Tech — this is the missing OmniX Threshold layer.

What you described is **not just alerting**.

It is:

> **Alarm → Context → Triage → Remediation → Evidence → Developer Escalation → Threshold Tuning**

That becomes the OmniX concept.

---

# 1. Plain English Version

## END-State Workflow: Engineer Infra Night Alarm

An alarm fires:

> **RabbitMQ 2B on `XYZ_CLASSC1_CUST8` — Queue `XXB` stopped reporting.**

The Engineer Infra is not trying to understand the entire application.
The goal is **restore service, preserve data, capture evidence, and escalate cleanly.**

The engineer wakes up, authenticates through VPN, logs into the environment, and starts from operational context:

### Step 1 — Identify the queue owner

The engineer needs to know:

> “Which application feeds Queue `XXB`?”

This should not require tribal knowledge. OmniX needs a map:

```text
Queue XXB → Fed by App ABC → Runs on Server XYZ → Service name abc-worker.service → Logs at /var/log/abc/
```

Without that map, the engineer wastes time.

---

### Step 2 — Locate the app and logs

The engineer does **not** open code.

This is a black-box operational workflow.

They inspect:

```text
systemd service status
RabbitMQ queue state
consumer state
message count
oldest message age
application logs
heap/memory errors
disk/mount health
node health
```

The engineer is asking:

> “Is the app dead, degraded, stuck, out of memory, blocked, or simply backed up?”

---

### Step 3 — Diagnose from logs, not source code

The engineer sees something like:

```text
ERROR EYZ-47281: Java heap exhausted while processing Queue XXB batch
```

That error may mean:

```text
The app is not bad.
RabbitMQ is not bad.
The queue is not corrupt.
The app worker ran out of heap memory and stopped consuming.
```

So the issue is operationally diagnosable without source-code access.

---

### Step 4 — Decide severity

This is where OmniX Thresholds matter.

The engineer needs to decide:

```text
Is this a P1?
Can infra remediate?
Does this require immediate developer contact?
Can this wait until morning?
Will a restart lose data?
Will waiting cause larger business/system failure?
```

Example decision:

```text
Queue XXB is built up.
The app is not consuming.
RabbitMQ is healthy.
Messages are persistent.
Restarting the app may lose 0–3 packets.
Waiting 4–6 hours will create a larger outage.
Decision: restart app service.
```

That is not “guessing.”

That is **threshold-backed triage**.

---

### Step 5 — Remediate

The engineer restarts the app service:

```bash
sudo systemctl restart abc-worker.service
```

Then validates:

```text
RabbitMQ node is healthy
Queue XXB is reporting again
Consumers reattached
Message count is draining
Message rate normalized
No critical data loss observed
```

The engineer does not stop at “service restarted.”

The engineer verifies that the system recovered.

---

### Step 6 — Document everything

The engineer captures:

```text
alarm time
login time
server
queue
app owner
commands run
logs observed
error signatures
decision made
restart time
post-restart queue state
message loss estimate
developer escalation notes
```

This becomes the Jira ticket / papertrail.

---

### Step 7 — Escalate to Dev correctly

The developer should not receive:

> “Queue broke.”

The developer should receive:

```text
Queue XXB stopped reporting at 02:14.
RabbitMQ node was healthy.
Consumers dropped to 0.
App ABC showed Java heap error EYZ-47281.
Memory was at 91%.
Queue depth reached 642.
Service abc-worker.service was restarted at 02:37.
Consumers restored.
Queue drained to normal by 02:48.
Estimated loss: 0–3 packets during restart.
Recommended dev review: heap handling / batch size / retry behavior.
```

That is useful.

That is engineering-grade escalation.

---

# 2. Monitoring Workflow in Plain English

The monitoring system should not only ask:

> “Is this thing up?”

It should ask:

> “Is this thing behaving normally for this queue, this app, this customer, this season, and this time window?”

The local agent should monitor:

```text
CPU
RAM
mounts
storage
data bricks
RabbitMQ nodes
RabbitMQ queues
consumers
message counts
message age
publish rate
consume rate
ack rate
connections when useful
```

Syslog and lastlog can be separate.
This layer is about **operational health and queue behavior**.

---

## Normal behavior must be mapped

For each queue, OmniX needs to learn or define:

```text
normal message rate
normal queue depth
normal consumer count
normal processing delay
normal error signatures
normal seasonal spikes
normal maintenance windows
normal customer-specific behavior
```

Example:

```text
Queue XXB normally has 0–40 messages.
At 200 messages, it is abnormal.
At 500 messages, it is critical.
At 1,000 messages, it is P1.
```

But then reality enters:

```text
During NY home heating oil cold season, Queue XXB commonly reaches 200–400 messages before repairs.
```

So OmniX must understand:

```text
200 messages in July = abnormal
200 messages in January cold season = expected
500 messages in January = warning
1,000 messages in January = critical
```

That is the difference between dumb alerting and operational intelligence.

---

# 3. Core OmniX Threshold Concept

OmniX Thresholds should be built around this principle:

> **A threshold is not just a number. A threshold is a decision boundary tied to context, consequence, and action.**

A weak alert says:

```text
Memory > 85%
```

A strong OmniX threshold says:

```text
Memory > 85%
AND App ABC owns Queue XXB
AND Queue XXB depth is rising
AND consumer count is below normal
AND known heap error EYZ appears
THEN alert Engineer Infra before Queue XXB stops reporting.
```

That is proactive.

That is the NORTH layer.

The END layer is:

```text
When alarm happens, guide the engineer to triage and remediation.
```

The NORTH layer is:

```text
Detect the failure pattern before the alarm becomes a full P1.
```

---

# 4. OmniX Concepts

## 1. Signal

A **Signal** is a raw thing OmniX can observe.

Examples:

```text
cpu_usage
memory_usage
mount_available
rabbitmq_node_status
queue_depth
consumer_count
oldest_message_age
publish_rate
ack_rate
error_signature_detected
service_status
```

---

## 2. Asset

An **Asset** is the thing being monitored.

Examples:

```text
Server XYZ_CLASSC1_CUST8
RabbitMQ Node 2B
Queue XXB
App ABC
systemd service abc-worker.service
Customer CUST8
Data-brick DBRICK-04
```

---

## 3. Ownership Map

The **Ownership Map** connects infrastructure to application responsibility.

Example:

```text
Queue XXB
  fed_by: App ABC
  consumed_by: App ABC Worker
  service: abc-worker.service
  server: XYZ_CLASSC1_CUST8
  logs: /var/log/abc/worker.log
  dev_owner: Payments Platform Team
  infra_owner: Engineer Infra
```

This is critical. Without it, OmniX cannot guide triage.

---

## 4. Baseline Profile

A **Baseline Profile** defines normal behavior.

Example:

```text
Queue XXB normal:
  depth: 0–40
  warning: 100
  abnormal: 200
  critical: 500
  p1: 1000
  normal_consumers: 2
  max_oldest_message_age: 180 seconds
```

---

## 5. Seasonal Override

A **Seasonal Override** modifies the baseline during known business cycles.

Example:

```text
NY Home Heating Oil Season:
  applies_to: Queue XXB
  months: November–March
  expected_depth: 200–400
  abnormal_depth: 500
  critical_depth: 900
```

This prevents false P1 alarms when the system is behaving as expected for seasonal load.

---

## 6. Error Signature

An **Error Signature** maps known log patterns to likely causes.

Example:

```text
Error: EYZ-47281
Pattern: Java heap exhausted
Likely cause: App worker memory exhaustion
Related metric: memory_usage > 85%
Likely impact: queue consumers drop
Recommended action: restart app service if queue persistence is valid
Developer action: review heap allocation / batch processing
```

---

## 7. Decision Policy

A **Decision Policy** tells Engineer Infra what to do.

Example:

```text
IF RabbitMQ is healthy
AND Queue XXB is backed up
AND App ABC consumers are dead
AND messages are persistent
AND restart loss estimate <= 3 packets
THEN restart abc-worker.service
```

Another:

```text
IF RabbitMQ node is unhealthy
AND multiple queues stopped reporting
THEN escalate RabbitMQ infrastructure incident
```

Another:

```text
IF unknown error signature appears
AND no safe runbook exists
THEN capture logs and contact developer directly
```

---

## 8. Runbook Action

A **Runbook Action** is the safe remediation command or procedure.

Example:

```text
Action: Restart App Worker
Command: sudo systemctl restart abc-worker.service
Validation:
  - systemctl status returns active
  - consumer count returns to 2
  - queue depth begins draining
  - no new heap errors appear
Risk:
  - possible 0–3 packet loss during stop/start
```

---

## 9. Evidence Bundle

An **Evidence Bundle** is what OmniX packages for Jira/dev escalation.

Example:

```text
Alarm ID
server
queue
app
service
metric snapshots
log snippets
commands executed
operator decision
before/after queue depth
before/after consumer count
estimated packet/message loss
recommended developer review
```

This becomes the papertrail.

---

## 10. P1 Direct Developer Contact

Developer contact is required when:

```text
safe infra remediation fails
unknown application error appears
queue corruption is suspected
message loss exceeds acceptable threshold
restart does not restore consumers
same error repeats after restart
business-critical SLA is breached
```

Developer contact is **not** required every time a known service restart clears a known infra-remediable failure.

That distinction matters.

---

# 5. OmniX Threshold Engine Model

The engine should evaluate four layers:

```text
1. Raw Metrics
2. Context
3. Policy
4. Action
```

Example:

```text
Raw Metric:
  Queue XXB has 642 messages.

Context:
  Normal is 0–40.
  Current season allows 200–400.
  Consumers are 0.
  App memory is 91%.
  Known heap error appears.

Policy:
  This is critical.
  RabbitMQ is valid.
  App worker is likely failed.
  Restart is allowed.

Action:
  Alert Engineer Infra.
  Recommend restart.
  Generate evidence bundle.
  Prepare Jira escalation.
```

---

# 6. Codex Prompt

Use this as the working prompt for OmniX.

```text
Build an OmniX Thresholds concept module for proactive infrastructure triage and remediation.

Goal:
Convert real operational alarm workflows into a local-first threshold engine that helps Engineer Infra respond to RabbitMQ/app/server incidents using metrics, ownership maps, error signatures, decision policies, runbooks, and escalation evidence.

Core Scenario:
A P1 alarm fires:
RabbitMQ 2B on server XYZ_CLASSC1_CUST8 reports that Queue XXB stopped reporting.

Engineer Infra must:
1. Identify which application feeds or consumes Queue XXB.
2. Locate the application service and log paths.
3. Diagnose using logs and metrics only, not source code.
4. Determine whether the issue is RabbitMQ, app worker, server resource, storage, memory, or known seasonal load.
5. Decide whether to remediate, wait, or contact the developer directly.
6. Execute safe runbook actions such as restarting a systemd service.
7. Validate that the queue, consumers, and message processing recovered.
8. Capture all commands, logs, metrics, and decisions into an evidence bundle.
9. Generate a Jira-ready developer escalation report.

Important Operating Rule:
Engineer Infra performs black-box diagnostics. They do not inspect application code. They use service status, logs, metrics, queue state, and known error signatures.

Implement the concept using these domain entities:

- Asset
- Signal
- QueueProfile
- AppOwnershipMap
- BaselineProfile
- SeasonalOverride
- ErrorSignature
- ThresholdRule
- DecisionPolicy
- RunbookAction
- EvidenceBundle
- EscalationPacket

Required Monitoring Signals:
- CPU usage
- RAM usage
- mount health
- storage usage
- data-brick health
- RabbitMQ node health
- RabbitMQ queue depth
- RabbitMQ consumer count
- RabbitMQ publish rate
- RabbitMQ ack/consume rate
- oldest message age
- application service status
- known application log error signatures

Do not treat thresholds as simple numbers.
A threshold must be modeled as a decision boundary that combines:
- metric value
- asset context
- queue ownership
- customer/site context
- time window
- seasonal business behavior
- known error signatures
- acceptable data-loss risk
- remediation action
- escalation requirement

Example Threshold:
If Queue XXB depth exceeds 200 messages, this is abnormal during normal season.
But during NY home heating oil cold season, Queue XXB may normally reach 200–400 messages before repairs.
Therefore the threshold engine must support seasonal overrides.

Example Error Signature:
Error EYZ-47281 indicates Java heap exhaustion.
If memory usage is above 85%, Queue XXB depth is rising, and consumer count drops to 0, alert Engineer Infra before the queue fully stops reporting.

Example Decision Policy:
If RabbitMQ is healthy, Queue XXB is backed up, App ABC consumers are down, messages are persistent, and estimated loss during restart is <= 3 packets, recommend restarting the app systemd service.

Example Runbook:
Command:
sudo systemctl restart abc-worker.service

Validation:
- service returns active
- consumers reconnect
- queue depth begins draining
- oldest message age decreases
- no new heap error appears

Example Escalation Packet:
Generate a Jira-ready report containing:
- alarm ID
- server
- queue
- app owner
- service name
- timestamps
- metric snapshots
- log snippets
- commands executed
- remediation decision
- before/after queue depth
- before/after consumer count
- data-loss estimate
- developer recommendation

Output Requirements:
1. Create a clean TypeScript domain model for the entities.
2. Create sample JSON fixtures for the RabbitMQ Queue XXB incident.
3. Create a threshold evaluation function.
4. Create a decision policy evaluator.
5. Create a runbook recommendation output.
6. Create an evidence bundle generator.
7. Create a Jira escalation packet formatter.
8. Keep this local-first and file-based for now.
9. Do not integrate external APIs yet.
10. Make the code readable, modular, and suitable for later integration into the OmniX lifecycle.

Design Tone:
This is not a generic monitoring dashboard.
This is an Engineer Infra operational intelligence layer designed to turn alarms into disciplined remediation, clean triage, and developer-ready evidence.
```

---

# 7. The Key OmniX Principle

The real product insight is this:

> **OmniX should not merely detect that something is broken. OmniX should know what the thing is, who owns it, what normal looks like, what abnormal means, what action is safe, what evidence is needed, and when a developer must be contacted.**

That is the threshold system.

Not “CPU high.”

Not “queue high.”

But:

```text
This queue is abnormal for this customer, on this server, during this season, because this app stopped consuming, likely due to this known error, and the safest infra action is this runbook.
```

That is OmniX Thresholds.
