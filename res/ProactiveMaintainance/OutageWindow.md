Tech — this is **OmniX Controlled Shutdown / Maintenance Orchestration**.

This is the other side of Thresholds.

Thresholds answer:

> “When is the system drifting toward failure?”

Controlled Shutdown answers:

> “How do we intentionally bring the system down without creating failure?”

This is a major OmniX lifecycle concept.

---
# 1. Alert Operator To Outage Window First

THEN; 
# 1. Plain English Version

A production system cannot be shut down randomly.

Servers, databases, queues, mounts, detached block storage, Apache/HTTPS hosts, load balancers, RabbitMQ clusters, consumers, app services, and sensor-classed servers all have dependency order.

A five-minute mistake can create a twenty-four-hour outage.

The reason is simple:

> **If the database goes down while applications, connection pools, queues, or front-end hosts are still accepting work, the system can corrupt state, lose writes, duplicate transactions, orphan messages, or accept client input that cannot be safely committed.**

That mistake can force terabytes or petabytes of backup restoration.

For a company that loses millions per hour, the shutdown sequence is not a checklist.

It is a **controlled operational doctrine**.

---

# 2. The Core Problem

A normal person thinks shutdown means:

```text
Turn off the servers.
Patch them.
Turn them back on.
```

Engineer Infra knows the real version:

```text
Stop new client traffic.
Drain active sessions.
Stop producers.
Drain queues.
Stop consumers.
Close app connection pools.
Verify databases have no active unsafe writers.
Stop databases cleanly.
Unmount filesystems.
Detach block storage.
Shutdown server classes in dependency order.
Mute internal sensors so monitoring does not create an alarm storm.
Document every step.
Bring everything back in reverse dependency order.
Validate end-to-end.
```

That is the OmniX concept.

---

# 3. Why Order Matters

## Bad shutdown order

```text
1. Stop database.
2. App servers still running.
3. Connection pools remain open.
4. Apps keep retrying.
5. Queues keep accepting messages.
6. Clients keep submitting requests.
7. Writes fail halfway.
8. Data becomes inconsistent.
9. Restore from backup.
10. Outage expands from 5 minutes to 24+ hours.
```

That is catastrophic.

---

## Correct shutdown order

```text
1. Enter maintenance mode.
2. Mute expected internal alarms.
3. Remove external entry points from service.
4. Stop new HTTPS/client input.
5. Drain front-end sessions.
6. Stop queue producers.
7. Let consumers finish safe in-flight work.
8. Drain RabbitMQ queues to safe depth.
9. Stop application services.
10. Confirm connection pools closed.
11. Stop databases cleanly.
12. Flush and unmount filesystems.
13. Detach block storage.
14. Shutdown/reboot/update servers by class and tier.
15. Start everything back up in reverse order.
16. Validate health before reopening client traffic.
```

That is disciplined infrastructure.

---

# 4. Maintenance Mode

Before shutdown begins, OmniX needs a global maintenance state.

```text
SYSTEM_STATE = MAINTENANCE_PENDING
```

This means:

```text
Operators know the shutdown is intentional.
Monitoring knows alarms are expected.
Load balancers know traffic will be drained.
Applications know new writes should stop.
Queues know producers should pause.
Engineers know this is a controlled operation, not an outage.
```

Maintenance mode prevents the system from treating intentional shutdown as uncontrolled failure.

---

# 5. Sensor Muting

This is critical.

When you intentionally reboot/update production infrastructure, every sensor will scream unless muted correctly.

Without muting:

```text
Server down
Queue down
Consumer missing
Database unavailable
Mount unavailable
Apache unavailable
Load balancer target failed
RabbitMQ node missing
Connection refused
Disk disappeared
```

That creates an **alarm broadcast storm**.

So OmniX needs controlled sensor muting.

Not blind muting.

Controlled muting.

```text
Mute reason: Scheduled maintenance
Mute scope: Internal sensors only
Mute duration: 90 minutes
Mute owner: Engineer Infra
Mute ticket: CHG-20491
External client-impact alarms: remain active
P1 override: still allowed
```

The difference matters.

You do not silence reality.

You suppress known expected noise while preserving true emergency detection.

---

# 6. Shutdown Doctrine: Front Door, Middle, Data Core

The clean way to think about this is:

```text
Ingress Layer
Application Layer
Queue / Message Layer
Data Layer
Storage Layer
Host Layer
```

You do not randomly shut these down.

You quiesce them. <- This is Key.

---

## Phase 1 — Preflight

Before touching anything, OmniX validates the environment.

```text
Change ticket exists
Backups verified
Replication healthy
RabbitMQ clusters healthy
Queue depths normal or acceptable
No active P1 incident
All server classes identified
All app ownership mappings loaded
All fstab mounts mapped
All detached block storage mapped
All database dependencies mapped
Rollback plan exists
Operator has access
Maintenance window approved
```

OmniX should block or warn if the system is already unstable.

---

## Phase 2 — Controlled Alarm Suppression

Mute internal operational sensors tied to expected maintenance events.

Examples:

```text
CPU unavailable
host down
service stopped
consumer count zero
queue reporting stopped
mount unavailable
RabbitMQ node unavailable
Apache stopped
```

But do **not** fully mute:

```text
unexpected client traffic
unauthorized writes
database corruption
replication failure
backup failure
storage detach failure
data-loss indicators
```

OmniX should know the difference between expected maintenance noise and actual danger.

---

## Phase 3 — Stop New Client Input

Before databases or queues are touched, stop new external work from entering.

This may mean:

```text
remove app hosts from load balancer
enable maintenance page
stop Apache/HTTPS hosts at the correct point
disable public write endpoints
disable scheduled jobs
disable cron producers
pause API ingress
```

The client should see controlled maintenance, not a half-alive system.

The goal:

> **No customer should be able to enter data into a system that cannot safely commit it.**

---

## Phase 4 — Drain Front-End and App Sessions

Let active requests finish.

Do not hard-kill the app while it is holding transactions.

OmniX should watch:

```text
active HTTP requests
open sessions
app worker activity
connection pool usage
in-flight jobs
database transaction count
```

The target state is:

```text
No new input
No unsafe active writes
No rising queue depth
No long-running transactions
```

---

## Phase 5 — Stop Producers

Producers are services that create work.

Examples:

```text
front-end submitters
API workers
batch jobs
cron jobs
sensor ingest services
file importers
message publishers
```

They must stop before consumers and databases are taken down.

Otherwise the queue keeps filling while the system is trying to shut down.

---

## Phase 6 — Drain Queues and Consumers

RabbitMQ cannot just be killed casually.

OmniX needs to know:

```text
which queues exist
which apps produce to them
which apps consume from them
which queues are durable
which messages are persistent
which consumers are safe to stop
which queues must drain to zero
which queues can retain messages safely
```

For Queue `XXB`, OmniX may require:

```text
Queue XXB depth <= 0
or
Queue XXB depth <= approved safe threshold
and
oldest message age stable
and
no unacked messages
and
consumer shutdown acknowledged
```

Important queue states:

```text
ready messages
unacked messages
consumer count
publish rate
ack rate
redeliveries
dead-letter count
oldest message age
```

A queue with 0 ready messages but 500 unacked messages is not safe.

---

## Phase 7 — Stop Application Services

After producers are stopped and queues are drained or safe, stop application services.

Example:

```bash
sudo systemctl stop app-worker.service
sudo systemctl stop app-api.service
sudo systemctl stop app-scheduler.service
```

Then validate:

```text
service inactive
no active app process
no active DB connection pool
no queue consumers attached
no retry storm
no app logs showing write failures
```

This protects the database.

---

## Phase 8 — Close Connection Pools

This is the key failure point you described.

If the database is shut down while application connection pools are still open, the applications may:

```text
retry writes
hold stale transactions
corrupt state
double-submit after reconnect
partially commit work
fail to acknowledge queue messages
generate duplicate records
```

So OmniX needs a validation gate:

```text
Database cannot be stopped until app connection pools are confirmed closed.
```

That should be a hard rule.

---

## Phase 9 — Stop Databases Cleanly

Only after apps are quiet, queues are safe, and connection pools are closed should databases stop.

Database shutdown should verify:

```text
no active unsafe sessions
replication healthy or intentionally paused
write-ahead logs flushed
transactions complete
backup checkpoint acceptable
database service stopped cleanly
```

This is the difference between maintenance and disaster recovery.

---

## Phase 10 — Filesystems, Mounts, FSTAB, Block Storage

Detached block storage and mounted filesystems come after databases and apps are stopped.

OmniX must map:

```text
/etc/fstab entries
mount points
attached block devices
database data directories
application data directories
backup volumes
NFS/EFS/iSCSI/EBS-style detached storage
```

Before unmount:

```text
no process is writing
no database is active
no app has open file handles
filesystem buffers flushed
```

Then:

```bash
sync
sudo umount /data/app
sudo umount /data/db
```

If a mount refuses to unmount, that is not an annoyance.

That means something still has a handle open.

OmniX should treat that as a stop condition.

---

## Phase 11 — Server Class Shutdown

Servers should be shut down by class and tier.

Example classes:

```text
Sensor-class servers
HTTPS / Apache front-end hosts
Application hosts
Queue / RabbitMQ nodes
Database servers
Storage controllers
Backup servers
Management / bastion hosts
```

Example tiers:

```text
Tier 0: External client entry
Tier 1: Web / HTTPS / Apache
Tier 2: Application services
Tier 3: Queue / messaging
Tier 4: Database
Tier 5: Storage / block volumes
Tier 6: Monitoring / management
```

Shutdown order is dependency-driven.

Startup order is usually the reverse.

---

# 7. Startup Order

Startup is not simply “turn everything back on.”

Correct startup order:

```text
1. Attach block storage.
2. Mount filesystems from fstab.
3. Validate mounts and storage health.
4. Start databases.
5. Validate database health and replication.
6. Start RabbitMQ clusters.
7. Validate cluster quorum and queue metadata.
8. Start application services.
9. Reattach consumers.
10. Validate queue processing.
11. Start Apache/HTTPS hosts.
12. Add hosts back to load balancer.
13. Disable maintenance page.
14. Unmute sensors.
15. Confirm client path works end-to-end.
```

The system should not accept client writes until the back end is confirmed healthy.

---

# 8. OmniX Concepts

## Controlled Shutdown Plan

A **Controlled Shutdown Plan** is an ordered, validated procedure for bringing infrastructure down safely.

```text
ControlledShutdownPlan
  maintenanceWindow
  affectedSystems
  shutdownOrder
  startupOrder
  sensorMutePolicy
  dataLossGuards
  rollbackPlan
  evidenceBundle
```

---

## Dependency Graph

OmniX needs a dependency graph.

Example:

```text
Client
  depends_on Load Balancer
Load Balancer
  depends_on Apache HTTPS Hosts
Apache HTTPS Hosts
  depends_on App API Services
App API Services
  depends_on RabbitMQ Producers
App Workers
  depend_on RabbitMQ Queues
App Workers
  depend_on Database Connection Pools
Database
  depends_on Mounted Filesystems
Mounted Filesystems
  depend_on Detached Block Storage
Detached Block Storage
  depends_on Physical / Cloud Host
```

The dependency graph tells OmniX what can stop safely and what cannot.

---

## Quiescence Gate

A **Quiescence Gate** is a required safe-state checkpoint.

Example:

```text
Do not stop database until:
  - no active app writers
  - no unsafe transactions
  - no open app connection pools
  - no unacked critical queue messages
  - no client write path active
```

This is one of the most important concepts.

---

## Drain Policy

A **Drain Policy** defines how a layer exits service.

Examples:

```text
Load balancer drain:
  remove host from pool
  wait for active requests = 0

Queue drain:
  pause producers
  wait for ready messages <= threshold
  wait for unacked messages = 0

App drain:
  stop accepting new work
  finish in-flight work
  close DB pools
  detach consumers
```

---

## Sensor Mute Policy

A **Sensor Mute Policy** defines which alarms are suppressed during maintenance.

```text
SensorMutePolicy
  scope
  duration
  owner
  ticket
  mutedSensors
  unmutedCriticalSensors
  autoUnmuteTime
```

OmniX should never allow indefinite muting without ownership.

---

## Data-Loss Guard

A **Data-Loss Guard** is a hard safety check.

Example:

```text
Database shutdown blocked because:
  app-api-03 still has 42 open DB connections
  queue XXB has 17 unacked messages
  load balancer still receiving external writes
```

Data-loss guards should stop the procedure.

---

## Server Class

A **Server Class** groups machines by operational role.

```text
Sensor Class
Web Class
App Class
Queue Class
Database Class
Storage Class
Management Class
```

This lets OmniX create class-based shutdown order instead of treating every server as equal.

---

## Maintenance Evidence Bundle

Every controlled shutdown should produce an evidence bundle:

```text
change ticket
operator
start time
end time
systems affected
sensor mutes applied
commands executed
shutdown order
startup order
validation gates
blocked steps
overrides
final health state
```

This is the papertrail.

---

# 9. OmniX Shutdown Order Model

A clean model:

```text
PRECHECK
MUTE_INTERNAL_SENSORS
ENTER_MAINTENANCE_MODE
DISABLE_EXTERNAL_INGRESS
DRAIN_LOAD_BALANCERS
STOP_HTTPS_APACHE
PAUSE_PRODUCERS
DRAIN_QUEUES
STOP_CONSUMERS
STOP_APP_SERVICES
VERIFY_CONNECTION_POOLS_CLOSED
STOP_DATABASES
FLUSH_FILESYSTEMS
UNMOUNT_FSTAB_MOUNTS
DETACH_BLOCK_STORAGE
SHUTDOWN_SERVER_CLASSES
PATCH_OR_REBOOT
STARTUP_REVERSE_ORDER
VALIDATE_END_TO_END
UNMUTE_SENSORS
EXIT_MAINTENANCE_MODE
```

This should become an OmniX state machine.

---

# 10. Codex Prompt

```text
Build an OmniX Controlled Shutdown and Maintenance Orchestration module.

Goal:
Create a local-first operational engine that models the correct shutdown and startup order for production infrastructure to prevent data loss, database corruption, queue corruption, accidental client writes, connection-pool failures, alarm storms, and extended outage windows.

Operational Context:
In production environments, servers, databases, data stores, detached block storage, fstab mounts, RabbitMQ queues, RabbitMQ consumers, RabbitMQ clusters, Apache/HTTPS hosts, load balancers, application servers, sensor-classed servers, and server tiers must be shut down in a specific order.

A wrong shutdown sequence can cause:
- application connection pools to remain open
- databases to be stopped while writers are still active
- queues to accept messages that cannot be consumed
- clients to enter data at the wrong time
- partially committed transactions
- duplicate or orphaned messages
- database corruption
- forced restore from terabytes or petabytes of backups
- 24+ hour outage windows from a 5-minute operational mistake
- millions per hour in business impact

Core Principle:
This is not a generic shutdown script.
This is a controlled maintenance doctrine.

The system must enforce dependency-aware shutdown and startup ordering.

Required Domain Concepts:
- ControlledShutdownPlan
- MaintenanceWindow
- DependencyGraph
- Asset
- ServerClass
- ServerTier
- LoadBalancerTarget
- HTTPSHost
- ApacheHost
- AppService
- QueueCluster
- RabbitMQNode
- RabbitMQQueue
- QueueProducer
- QueueConsumer
- DatabaseService
- ConnectionPool
- DataStore
- MountPoint
- FstabEntry
- DetachedBlockStorage
- Sensor
- SensorMutePolicy
- QuiescenceGate
- DrainPolicy
- DataLossGuard
- ShutdownStep
- StartupStep
- ValidationCheck
- RollbackPlan
- MaintenanceEvidenceBundle

Required Shutdown State Machine:
1. PRECHECK
2. MUTE_INTERNAL_SENSORS
3. ENTER_MAINTENANCE_MODE
4. DISABLE_EXTERNAL_INGRESS
5. DRAIN_LOAD_BALANCERS
6. STOP_HTTPS_APACHE
7. PAUSE_PRODUCERS
8. DRAIN_QUEUES
9. STOP_CONSUMERS
10. STOP_APP_SERVICES
11. VERIFY_CONNECTION_POOLS_CLOSED
12. STOP_DATABASES
13. FLUSH_FILESYSTEMS
14. UNMOUNT_FSTAB_MOUNTS
15. DETACH_BLOCK_STORAGE
16. SHUTDOWN_SERVER_CLASSES
17. PATCH_OR_REBOOT
18. STARTUP_REVERSE_ORDER
19. VALIDATE_END_TO_END
20. UNMUTE_SENSORS
21. EXIT_MAINTENANCE_MODE

Required Startup Order:
1. Attach detached block storage.
2. Mount filesystems from fstab.
3. Validate storage and mount health.
4. Start databases.
5. Validate database health, replication, and transaction state.
6. Start RabbitMQ clusters.
7. Validate cluster quorum and queue metadata.
8. Start application services.
9. Reattach queue consumers.
10. Validate queue processing and message drain.
11. Start Apache/HTTPS hosts.
12. Add hosts back to load balancers.
13. Disable maintenance page.
14. Unmute sensors.
15. Validate client path end-to-end.

Data-Loss Guard Rules:
The system must block database shutdown if:
- any application service still has active database connections
- any connection pool remains open
- any client write path is still active
- any queue has unsafe unacked messages
- any queue producer is still publishing
- any long-running transaction remains active
- any filesystem has active writers
- any required backup checkpoint is missing

Queue Safety Rules:
Before queue consumers are stopped:
- producers must be paused
- queue publish rate must be zero or approved
- ready messages must be below the configured safe threshold
- unacked messages must be zero for critical queues
- dead-letter count must not be increasing
- oldest message age must be stable or decreasing

Sensor Muting Rules:
The system must support maintenance-aware muting of internal sensors to avoid an alarm broadcast storm.

Mute examples:
- host down
- service stopped
- RabbitMQ consumer count zero
- queue stopped reporting
- Apache stopped
- mount unavailable during planned unmount
- RabbitMQ node offline during planned reboot

Do not mute:
- unexpected external client writes
- database corruption indicators
- backup failure
- replication failure
- data-loss indicators
- unauthorized access
- storage detach failure
- critical safety alarms outside the maintenance scope

Apache / HTTPS Rule:
Apache or HTTPS hosts must be stopped only after traffic has been drained or the maintenance page/load balancer path is active. The client must never be allowed to submit data into a partially shut down backend.

Load Balancer Rule:
Load balancer targets must be drained before application hosts are stopped. Hosts must not be removed blindly if active sessions or in-flight writes remain.

FSTAB / Mount Rule:
Fstab mounts must not be unmounted until all dependent applications and databases are stopped and no active file handles remain. If unmount fails, the procedure must halt and report which processes still hold the mount.

Detached Block Storage Rule:
Detached block storage must not be detached until filesystems are unmounted cleanly and buffers are flushed.

Server Class Model:
Support class-based and tier-based shutdown.

Example classes:
- SensorClass
- WebClass
- AppClass
- QueueClass
- DatabaseClass
- StorageClass
- ManagementClass

Example tiers:
- Tier 0: Client ingress and load balancer
- Tier 1: HTTPS / Apache front-end
- Tier 2: Application services
- Tier 3: RabbitMQ / messaging
- Tier 4: Database services
- Tier 5: Mounted filesystems and block storage
- Tier 6: Monitoring and management

Implementation Requirements:
1. Create TypeScript interfaces for all required domain concepts.
2. Create a sample dependency graph for a RabbitMQ-backed production application.
3. Create a shutdown planner that performs dependency-aware ordering.
4. Create a startup planner that reverses the dependency graph safely.
5. Create validation gates that block unsafe steps.
6. Create sensor mute policy logic.
7. Create data-loss guard checks.
8. Create sample JSON fixtures for:
   - server classes
   - load balancer targets
   - Apache hosts
   - app services
   - RabbitMQ clusters
   - queues and consumers
   - databases
   - connection pools
   - fstab mounts
   - detached block storage
   - sensors
9. Create a human-readable runbook generator.
10. Create a maintenance evidence bundle generator.
11. Create a change-ticket summary formatter.
12. Keep the design local-first and file-based for now.
13. Do not integrate real cloud APIs yet.
14. Do not execute real shutdown commands. Generate planned commands and validation steps only.

Expected Output:
- /src/domain/maintenance.ts
- /src/domain/assets.ts
- /src/domain/dependencyGraph.ts
- /src/domain/sensors.ts
- /src/domain/runbooks.ts
- /src/engine/shutdownPlanner.ts
- /src/engine/startupPlanner.ts
- /src/engine/dataLossGuards.ts
- /src/engine/sensorMutePolicy.ts
- /src/engine/validationGates.ts
- /src/fixtures/maintenance-scenario.json
- /src/fixtures/server-classes.json
- /src/fixtures/rabbitmq-cluster.json
- /src/fixtures/storage-map.json
- /src/output/runbook-example.md
- /src/output/evidence-bundle-example.json

Design Tone:
This module should feel like an Engineer Infra command discipline system, not a toy script.

It should model operational reality:
- ordered shutdown
- dependency safety
- queue draining
- database protection
- connection pool closure
- fstab/mount safety
- detached block storage safety
- sensor muting
- alarm storm avoidance
- startup validation
- client-impact prevention
- papertrail generation

The central doctrine:
A controlled shutdown is successful only if no client can submit unsafe data, no queue loses messages, no database is stopped while writers are active, no filesystem is detached while dirty, and all expected alarms are muted without hiding true emergencies.
```

---

# 11. Suggested JSON Skeleton

```json
{
  "maintenanceWindow": {
    "id": "CHG-20491",
    "title": "Routine production reboot and update window",
    "mode": "controlled_shutdown",
    "riskLevel": "high",
    "operatorRole": "EngineerInfra",
    "expectedDurationMinutes": 90
  },
  "assetGraph": {
    "loadBalancers": [
      {
        "id": "lb-prod-01",
        "targets": ["apache-web-01", "apache-web-02"],
        "drainRequired": true
      }
    ],
    "httpsHosts": [
      {
        "id": "apache-web-01",
        "service": "httpd",
        "tier": 1,
        "stopAfter": ["lb-prod-01:drained"]
      }
    ],
    "appServices": [
      {
        "id": "app-api-01",
        "service": "app-api.service",
        "tier": 2,
        "dependsOn": ["db-prod-01", "rmq-cluster-2b"],
        "connectionPool": "pool-app-api-db"
      }
    ],
    "rabbitmq": {
      "clusterId": "rmq-cluster-2b",
      "nodes": ["rmq-2b-01", "rmq-2b-02", "rmq-2b-03"],
      "queues": [
        {
          "name": "XXB",
          "durable": true,
          "requiresDrain": true,
          "safeReadyDepth": 0,
          "safeUnackedDepth": 0,
          "producers": ["app-api-01"],
          "consumers": ["app-worker-01"]
        }
      ]
    },
    "databases": [
      {
        "id": "db-prod-01",
        "service": "postgresql",
        "tier": 4,
        "stopAfter": [
          "app-api-01:stopped",
          "app-worker-01:stopped",
          "pool-app-api-db:closed"
        ],
        "mounts": ["/data/db"]
      }
    ],
    "mounts": [
      {
        "mountPoint": "/data/db",
        "fstabEntry": "UUID=abc-123 /data/db xfs defaults,nofail 0 2",
        "dependsOn": ["vol-db-prod-01"],
        "unmountAfter": ["db-prod-01:stopped"]
      }
    ],
    "blockStorage": [
      {
        "id": "vol-db-prod-01",
        "device": "/dev/xvdf",
        "detachAfter": ["/data/db:unmounted"]
      }
    ]
  },
  "sensorMutePolicy": {
    "scope": "internal_maintenance_only",
    "muteDurationMinutes": 90,
    "mutedSensors": [
      "host_down",
      "service_stopped",
      "consumer_count_zero",
      "queue_stopped_reporting",
      "mount_unavailable",
      "apache_stopped"
    ],
    "neverMute": [
      "database_corruption",
      "backup_failure",
      "replication_failure",
      "unauthorized_access",
      "unexpected_client_write",
      "data_loss_indicator"
    ]
  },
  "dataLossGuards": [
    {
      "id": "guard-db-open-pools",
      "description": "Block database shutdown while app connection pools remain open.",
      "condition": "connectionPool.activeConnections > 0",
      "severity": "blocker"
    },
    {
      "id": "guard-queue-unacked",
      "description": "Block consumer shutdown while critical queue has unacked messages.",
      "condition": "queue.unackedMessages > 0",
      "severity": "blocker"
    },
    {
      "id": "guard-client-write-path",
      "description": "Block backend shutdown while external client writes are still possible.",
      "condition": "ingress.writePathEnabled == true",
      "severity": "blocker"
    }
  ]
}
```

---

# 12. The OmniX Doctrine Statement

This should probably become the opening comment in the module:

```text
A shutdown is not safe because services are stopped.
A shutdown is safe only when dependency order, client ingress, queue state, connection pools, databases, mounts, storage, sensors, and startup validation are controlled as one system.

A five-minute mistake in shutdown order can become a twenty-four-hour restore event.

OmniX Controlled Shutdown exists to prevent that mistake.
```

---

# 13. Naming Recommendation

For OmniX, I would name this module one of these:

```text
OmniX Controlled Shutdown
OmniX Maintenance Orchestrator
OmniX Quiescence Engine
OmniX SafeStop
OmniX Dependency Shutdown Planner
OmniX Zero-Loss Maintenance
```

Best internal technical name:

```text
OmniX Quiescence Engine
```

Best user-facing name:

```text
OmniX Controlled Maintenance
```

Because the real concept is not “shutdown.”

The real concept is:

> **quiescing a live system into a safe state before change.**
