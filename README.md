# OmniX

OmniX is a local-first C++ analyst console and TZE runtime for investigation,
deterministic orchestration, native tool execution, and guarded Ollama-assisted
shell workflows.

## What OmniX Does

- Runs a deterministic TZE pipeline backed by [`res/tze.txt`](res/tze.txt)
- Ingests and analyzes local evidence into cases, incidents, reports, and run history
- Discovers and reuses native tools like `nmap`, `tshark`, `grep`, `awk`, and `ssh`
- Replays, diffs, reports, and explains TZE runs with persistent memory
- Exposes a compact interactive shell with optional guarded Ollama assist

## Current Status

- `v1`: shipped
- `v2`: deterministic TZE completion
- `v3`: guarded Ollama-assisted shell and command routing

Project planning and architecture tracks live in:

- [`Goal.md`](Goal.md)
- [Prime-Arc-A roadmap](docs/agile/00-roadmap.md)
- [Prime-Arc-B roadmap](docs/agile/B-00-prime-arc-b-roadmap.md)
- [Prime-Arc-C reference map](docs/agile/C-00-prime-arc-c-reference-map.md)

## Core Capabilities

- Analyst console:
  - `ingest`, `analyze`, `decide`, `case`, `incident`
- Deterministic TZE runtime:
  - `tze runs`, `tze replay`, `tze diff`, `tze report`, `tze explain-change`
- Native tooling:
  - `tool list`, `tool locate`, `tool doctor`, `tool <name> -- <args...>`
- Managed builds:
  - `doctor`, `preflight`, `build`
- Local shell:
  - `omnix shell --assist`

## Build

```bash
cmake -S . -B build
cmake --build build -j4
```

## Test

```bash
ctest --test-dir build --output-on-failure
cmake --build build --target validate_generated_xpp
cmake --build build --target validate_tze_conformance
```

## Quick Start

Show the command surface:

```bash
./build/omnix --help
```

Inspect provider readiness:

```bash
./build/omnix provider probe
```

Run the interactive shell:

```bash
./build/omnix shell
```

Run the guarded Ollama shell:

```bash
OMNIX_REASONING_PROVIDER=ollama OMNIX_OLLAMA_MODEL=deepnimsec-omni:latest ./build/omnix shell --assist
```

### Example Shell Session

```text
provider probe
Run NMAP with a local /24 scan
nmap results
Ollama, secure my system
what should I do next
```

## Common Commands

```bash
./build/omnix ask "Build TShark" --assist
./build/omnix ingest /path/to/log.txt
./build/omnix analyze /path/to/log.txt
./build/omnix decide <case-id>
./build/omnix case timeline <case-id>
./build/omnix incident list
./build/omnix tze latest
./build/omnix tze explain-change-latest --assist
./build/omnix tze report latest --assist
./build/omnix tool list
./build/omnix doctor nmap
./build/omnix build tshark --assist
```

## Repository Layout

- [`main.cpp`](main.cpp): CLI entrypoint and interactive shell
- [`src/session_coordinator.cpp`](src/session_coordinator.cpp): routing, guarded command flow, TZE execution
- [`src/tool_flow_interpreter.cpp`](src/tool_flow_interpreter.cpp): native and built-in tool execution
- [`src/build_flow_interpreter.cpp`](src/build_flow_interpreter.cpp): preflight and managed builds
- [`src/analyst_flow_interpreter.cpp`](src/analyst_flow_interpreter.cpp): ingest, analyze, decide, case and incident flows
- [`src/reasoning_provider.cpp`](src/reasoning_provider.cpp): guarded provider seam
- [`res/tze.txt`](res/tze.txt): TZE source material
- [`docs/agile`](docs/agile): roadmap, epics, and architecture planning

## Ollama Notes

OmniX uses Ollama as an assistive layer, not as the execution authority.

- deterministic execution stays in OmniX
- assist output must validate before use
- command routing is allowlisted
- tool execution and build selection remain guarded

For the local DeepNimSec profile:

```bash
./scripts/create_deepnimsec_ollama_model.sh
./scripts/omnix_deepnimsec.sh --probe --compact
./scripts/omnix_deepnimsec.sh
```

## GitHub Readiness

This repository now ignores:

- build output
- WebApp local artifacts
- macOS `.DS_Store` files
- editor swap files and local scratch logs

If previously tracked macOS metadata exists in your index, remove it once with:

```bash
git rm --cached .DS_Store include/.DS_Store docs/.DS_Store
```

## License

[`LICENSE`](LICENSE)
