# OmniX

OmniX is a local-first C++ analyst console and TZE runtime for investigation,
deterministic orchestration, native tool execution, and guarded model-assisted
shell workflows.

"Aside from my lofy goals - I wrote this over 20 years ago in PseudoCode and named X++. When GiGo become Real and you realized the code was actually okay still a mess back then." [`res/tze.txt`](res/tze.txt)

## What OmniX Does

- Runs a deterministic TZE pipeline backed by [`res/tze.txt`](res/tze.txt)
- Ingests and analyzes local evidence into cases, incidents, reports, and run history
- Discovers and reuses native tools like `nmap`, `tshark`, `grep`, `awk`, and `ssh`
- Replays, diffs, reports, and explains TZE runs with persistent memory
- Exposes a compact interactive shell with optional guarded Ollama or OpenAI assist

## Current Status

- `v1`: shipped
- `v2`: deterministic TZE completion
- `v3`: guarded Ollama/OpenAI-assisted shell and command routing are available behind explicit assist enablement and remain deterministic-first

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
- OmniXTView packet viewer:
  - `tview doctor`, `tview port <port>`, `tview pcap <file>`
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

Make the current build available on your shell path:

```bash
./scripts/link_omnix.sh
```

By default this creates `omnix` and a convenience `tze` shim in `~/.local/bin`. If your shell does not already include that directory, add it once:

```bash
export PATH="$HOME/.local/bin:$PATH"
```

Then commands such as `omnix gg search --port 11434 --compact` and `tze replay latest` work without the `./build/` prefix. Use `./scripts/link_omnix.sh --with-gg` if you also want `gg search ...` as a direct shim.

OmniX also has native link/shim creation under the built-in tool runtime:

```bash
./build/omnix link install --with-tze --prefix ~/.local/bin
./build/omnix tool symlink -- create ./build/omnix ~/.local/bin/omnix --force
./build/omnix tool symlink -- shim tze tze --prefix ~/.local/bin --bin ./build/omnix --force
```

Authoritative command dictionary:

```bash
man -l docs/man/omnix.1
```

Mirror for repo browsing:

- [`docs/man/omnix.1.md`](docs/man/omnix.1.md)

Inspect provider readiness:

```bash
./build/omnix provider probe
./build/omnix api status --compact
./build/omnix api template huggingface
```

Explain the last run with the Recursive Why/Diff engine:

```bash
./build/omnix why latest --compact
./build/omnix next latest --compact
```

When a failed definition route can be repaired from learned definitions, prior TZE runs, compact history, or `res/local_glossary.tsv`, normal `ask` now consumes that Recursive Route Learning path before returning unresolved output. `why` backtraces the route, and `next` prints only the next recommended action.

Reset volatile learned/runtime context when a temporary association gets sticky:

```bash
./build/omnix context reset --compact
./build/omnix memory reset-context --compact
./build/omnix memory prune-expired --compact
```

This clears cached definitions, history, language/uAC state, and assist learning while keeping source glossary entries, TZE ledgers, cases, recipes, tools, and persona intact.

Author a local-path build recipe through the X++ module phase:

```bash
OMNIX_REASONING_PROVIDER=ollama OMNIX_OLLAMA_MODEL=fixture ./build/omnix recipe author /path/to/project --no-install
```

Run the interactive shell:

```bash
./build/omnix shell
```

Run the guarded Ollama shell:

```bash
OMNIX_REASONING_PROVIDER=ollama OMNIX_OLLAMA_MODEL=deepnimsec-omni:latest ./build/omnix shell --assist
```

Run the guarded OpenAI shell:

```bash
OMNIX_REASONING_PROVIDER=openai OPENAI_API_KEY=... OPENAI_MODEL=gpt-4.1-mini ./build/omnix shell --assist
```

Run OpenAI assist through repo-local `./.env`:

```bash
./scripts/omnix_openai.sh shell --assist
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
./build/omnix recipe author /path/to/project --no-install
./build/omnix doctor nmap
./build/omnix build tshark --assist
./build/omnix tview doctor
./build/omnix tview port 5000 --interface lo0
./build/omnix tview pcap /path/to/capture.pcap --port 5000 --out /tmp/omnix-tview.jsonl
./build/omnix defend diag cpu
./build/omnix defend diag port 5000
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

## Provider Notes

OmniX uses Ollama or OpenAI as an assistive layer, not as the execution authority.

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

For the OpenAI profile, copy `.env.example` to `.env`, fill in the key and model, then use the wrapper. The wrapper only reads approved OpenAI/OmniX keys and does not print secrets:

```bash
./scripts/omnix_openai.sh provider probe --compact
./scripts/omnix_openai.sh ask --assist "What should I do next?"
./scripts/omnix_openai.sh "Define Turning Scale"
./scripts/omnix_openai.sh omnix tview port 5000
./scripts/omnix_openai.sh defend diag cpu
./scripts/omnix_openai.sh defend diag port 5000
```

OpenAI freeform answers are a final `ask --assist` fallback after local memory, definitions, command routing, and guarded tool planning miss. Freeform answers may explain, calculate, or propose OmniX commands, but they do not claim local evidence was collected unless OmniX actually ran the command.

TView JSONL exports are local SIEM-ready packet events (`omnix.tview.packet.v1`) with stable `NET.TCP.*`
analysis codes. Defensive commands are diagnostic-first: OmniX may recommend a manual PID kill, service stop, or
port-closure path, but it does not perform destructive action in this slice.

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
