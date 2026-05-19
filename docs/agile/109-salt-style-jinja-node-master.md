# Salt-Style Jinja, Node, And Master Layer

## Summary

OmniX borrows the useful shape of Salt without becoming a Salt clone: templates, node facts, explicit trust, job plans, and master coordination. The OmniX version stays local-first, file-spooled, small, and replayable. No remote shell, no automatic enrollment, and no background daemon are required in this phase.

## Operator Surface

```sh
omnix jinja inspect <file.j2> [--vars vars.json]
omnix jinja render <file.j2> --vars vars.json [--out rendered.txt]
omnix jinja plan <file.j2> --vars vars.json
omnix jinja execute <file.j2> --vars vars.json --confirm

omnix node doctor
omnix node id
omnix node status
omnix node heartbeat [--out heartbeat.json]
omnix node enroll [--out enrollment.json]

omnix master doctor
omnix master init
omnix master node list
omnix master node approve <fingerprint>
omnix master job plan <job-type> --target <node-id>
omnix master job dispatch
omnix master job status
```

## Jinja Passthrough

Phase 1 treats Jinja as an explicit local artifact workflow:

- `inspect` extracts simple `{{ variable }}` references and risk markers without executing anything.
- `render` performs a narrow passthrough replacement from flat JSON values for local templates.
- `plan` classifies rendered output as `text_artifact`, `config_plan`, `runbook_plan`, `shell_plan`, or `unknown_manual_review`.
- `execute` refuses arbitrary rendered shell text. Future execution must map to allowlisted local runbook action types with exact confirmation and evidence capture.

This is not a general Salt renderer and does not expose Salt execution modules. If rendered output contains shell-like text, OmniX stores it as a proposed plan rather than running it.

## Node Design

An OmniX node is a local agent profile, not a required daemon. Node identity reuses `omnix id` and adds local grains such as platform, CPU architecture, OmniX version, tool capabilities, service manager hints, package manager hints, and defensive detection support.

Heartbeat artifacts use `omnix.node.heartbeat.v1` and intentionally avoid secrets. Enrollment is explicit: a node writes an enrollment request, and a master must approve the fingerprint before jobs can be trusted.

## Master Design

Master mode is opt-in. The first implementation is file-spool only under the OmniX memory root:

- `master.json` records local master configuration.
- `nodes.jsonl` records approved fingerprints.
- `jobs/*.json` records planned jobs.

Initial job types are read-only or planning-oriented:

- `defend.detect`
- `tview.doctor`
- `gg.search`
- `thresholds.evaluate`
- `jinja.render`
- `jinja.plan`

Network transport, remote mutation, rollback, and orchestration are deferred until trust policy, confirmation, and validation evidence are stronger.

## Safety Defaults

- No arbitrary remote shell execution.
- No automatic node enrollment.
- No secrets in grains, job cache, TZE history, or reports.
- No mutation jobs in v1.
- File-spool semantics come before network transport.

## Salt Inspiration, OmniX Boundary

Salt provides the inspiration: master/minion topology, grains, targeting, renderers/Jinja, events/reactor, jobs, and orchestration. OmniX keeps the boundary tighter: evidence first, explicit trust, local artifacts, and operator-visible plans before action.

