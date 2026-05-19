# Vuplus Gate Vertical Scale Fixtures

These artifacts simulate the post-SIEM verticals OmniX needs to understand before it reaches real cloud or Kubernetes accounts.

They are offline fixtures only. They do not call Google Cloud, Terraform, Ansible, Kubernetes, Docker, load balancers, or remote hosts.

## Fixture Set

- `scale-verticals-combined.json`: one larger mixed artifact with network, Kubernetes, load balancer, Ansible, Terraform/GCP, storage, and OmniX master/minion fields.
- `network-map-scale.json`: server, subnet, redundancy, dependency, and OmniX master/minion topology.
- `kubernetes-docker-context.json`: nested host-to-Kubernetes-to-container context.
- `load-balancer-front-back.json`: frontend/backend routing, health, drain, and rollback impact.
- `ansible-inventory-scale.json`: inventory, group, playbook, role, host variables, and queue/service mapping.
- `terraform-gcp-compute-plan-shape.json`: future Google Compute/Terraform creation analysis shape, with no cloud credentials.
- `omnix-minion-neighbor-map.json`: CDP-style neighbor relationships between OmniX nodes.
- `storage-cloud-local-map.json`: local mount, volume, persistent volume, bucket, and shutdown-order risk.

## Test Patterns

```sh
./build/omnix vg shape res/ops/verticals/scale-verticals-combined.json --compact --out /tmp/vg-scale-shape.json
./build/omnix vg explain res/ops/verticals/network-map-scale.json --learn-shape --compact
./build/omnix vg explain res/ops/verticals/kubernetes-docker-context.json --learn-shape --compact
./build/omnix vg correlate res/ops/verticals/load-balancer-front-back.json --dependency-map res/ops/verticals/network-map-scale.json --compact
./build/omnix vg shape res/ops/verticals/terraform-gcp-compute-plan-shape.json --compact --out /tmp/vg-terraform-shape.json
./build/omnix vg explain res/ops/verticals/omnix-minion-neighbor-map.json --learn-shape --compact
./build/omnix vg cab res/ops/verticals/scale-verticals-combined.json --out /tmp/vg-scale-cab.json --compact
```

## Future Runtime Targets

- `infra map`: turn shaped verticals into a network/dependency graph.
- `node/master`: connect approved OmniX minions back to an explicit OmniX master.
- `terraform inspect`: parse HCL and plan JSON without applying changes.
- `kubernetes inspect`: read kube artifacts and pod/container evidence without mutating clusters.
- `loadbalancer inspect`: shape front-end/back-end routing and drain evidence into CAB-ready output.
