# Twilio Alerting And Tenant-Safe Master/Minion Wizard

## Summary

This future slice connects two control-plane needs:

- Twilio as an optional alert/alarm notification transport.
- A tenant-bound Master/Minion enrollment wizard that prevents OmniX nodes from drifting across companies.

This is documentation only. It does not add Twilio API calls, SMS sending, key generation changes, node enrollment changes, master dispatch, or remote mutation.

## Twilio Alert Transport

Twilio is a future notification transport for:

- alarm notifications
- Alarm CAB review nudges
- threshold escalation notifications
- operator approval reminders
- high-priority Vuplus Gate incident summaries

First-phase Twilio behavior must be outbound-only:

- SMS, voice, or webhook notification only.
- No automatic remediation from inbound SMS.
- No inbound SMS as execution approval.
- No secrets, Twilio tokens, phone numbers, or raw message bodies stored in TZE, node grains, job cache, or memory without redaction.

Design-only future commands:

```sh
omnix alert doctor twilio
omnix alert configure twilio
omnix alert test twilio --to <alias>
omnix vg cab <artifact> --notify twilio
```

Twilio is advisory transport. OmniX local evidence, TZE replay, CAB approval, exact confirmations, and validation gates remain the authority.

## Tenant-Safe Master/Minion Wizard

The Salt-style Master/Minion layer already uses explicit node identity and master approval. The next trust boundary is tenant binding: a node approved for one company must not be able to drift into another company’s master job scope.

The company name is not a secret. It is a readable namespace input only. Tenant identity should be derived from:

- normalized company or tenant slug
- generated tenant salt
- OmniX master instance id
- Master Public Key fingerprint
- minion public key fingerprint

The WIF/Electrum-like concept becomes an exportable enrollment bundle, not private key export. It should contain public identity and signed enrollment metadata only:

```json
{
  "event_type": "omnix.node.enrollment_bundle.v1",
  "tenantId": "tenant-v1-example",
  "tenantSlug": "example-company",
  "masterFingerprint": "sha256:...",
  "minionFingerprint": "sha256:...",
  "enrollmentNonce": "...",
  "createdAt": "...",
  "signature": "..."
}
```

Design-only future commands:

```sh
omnix master tenant init --company "<name>"
omnix master tenant list
omnix node enroll --tenant <tenant-id> --out enrollment.json
omnix master node approve <enrollment.json>
omnix master job plan <type> --target <node-id> --tenant <tenant-id>
```

Every future job plan must carry:

- `tenantId`
- `masterFingerprint`
- `nodeFingerprint`

If the tenant, master fingerprint, or node fingerprint does not match the approved registry, OmniX must refuse planning or dispatch with:

```text
tenant_boundary_violation
```

## No-Stray Mutation Rule

No cross-company mutation means:

- A master cannot plan or dispatch jobs to a minion outside its approved tenant.
- A node cannot self-assign into another tenant.
- A job cache entry cannot be replayed into a different tenant.
- Twilio notifications cannot authorize mutation by themselves.
- Remote mutation remains deferred until tenant binding, allowlists, exact confirmation, and validation evidence are implemented.

No private key material may appear in:

- enrollment bundles
- Twilio alerts
- TZE runs
- node grains
- job cache
- memory history
- reports

## Future Integration Points

Tenant binding should connect to:

- `omnix node id`
- `omnix node heartbeat`
- `omnix node enroll`
- `omnix master node approve`
- `omnix master job plan`
- Vuplus Gate Alarm CAB output
- threshold escalation output
- future Twilio alert transport

The implementation order should be:

1. Tenant metadata in docs and fixtures.
2. Tenant-bound enrollment bundle schema.
3. Master registry validation.
4. Job-plan tenant enforcement.
5. Twilio outbound alert configuration.
6. CAB/threshold notification routing.
7. Only later, any remote mutation path.

## Safety Boundary

This slice does not implement runtime behavior. It records the trust design before OmniX grows broader control-plane power.

The safe doctrine is:

```text
Tenant identity before remote reach.
Evidence before alerting.
CAB approval before mutation.
Validation before success.
No private keys in operational artifacts.
No Twilio message can become execution authority.
```

## Acceptance

- The roadmap links to this spec.
- `Goal.md` names Twilio alerting and tenant-safe Master/Minion wizard goals.
- Glossary definitions exist for Twilio alert transport, tenant-bound enrollment, Master Public Key, node enrollment bundle, and no-stray mutation.
- The spec clearly states no Twilio runtime calls and no master/minion runtime mutation in this slice.

## Assumptions

- `MPK` means Master Public Key, not Master Private Key.
- WIF/Electrum is an analogy for portable enrollment identity, not wallet or private-key export.
- Twilio is future alert transport only.
- OmniX remains local-first, deterministic, evidence-bound, and operator-approved.
