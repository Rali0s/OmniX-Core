# OmniX(1)

## Name

`omnix` - local-first analyst console, TZE runtime, and guarded assist shell

## Synopsis

```bash
omnix ask <prompt> [--assist]
omnix shell [--assist]
omnix provider probe
omnix api status|doctor|configure <openai|ollama>|template <openai|ollama|huggingface>
omnix why [latest|run-id]
omnix link install|remove|doctor [--with-tze] [--with-gg]
omnix tool <name> -- <args...>
omnix build <project-or-path> [--assist]
omnix recipe author <source-path> [--assist]
omnix persona mode <premise|cynic|professional|neutral>
omnix tview port <port> [--out packets.jsonl]
omnix defend diag <cpu|memory|logs|pid|port> [target]
```

## Command Dictionary

OmniX is deterministic-first. Ollama or OpenAI may help with bounded planning, but they do not own execution. The canonical guarded command surface is:

- `build <project-or-path>`
- `recipe author <source-path>`
- `preflight <project-or-path>`
- `doctor <project-or-path>`
- `ingest <path-or-command>`
- `analyze <case-or-source>`
- `decide <case-or-source>`
- `case list`
- `case search <term>`
- `case timeline <id-or-source>`
- `case <id-or-source>`
- `incident list`
- `incident report <id>`
- `incident <id>`
- `define <symbol-or-term>`
- `explain <command-or-symbol>`
- `review <path-or-module>`
- `patch-proposal <path-or-module>`
- `provider probe`
- `api status`
- `api doctor`
- `api configure <openai|ollama>`
- `api template <openai|ollama|huggingface>`
- `why <run-id|latest>`
- `link install|remove|doctor`
- `persona mode <premise|cynic|professional|neutral>`
- `tview port <port>`
- `tview pcap <file> --port <port> --out <file>.jsonl`
- `tview doctor`
- `defend diag <cpu|memory|logs|pid|port> [target]`
- `memory <history|prefs|definitions|language|security|uac|cases|runs|tze|legacy|persona|operator|assist>`
- `tze latest`
- `tze replay <run-id|latest>`
- `tze chain <run-id|latest>`
- `tze diff <left-run-id> <right-run-id>`
- `tze diff-latest`
- `tze explain-change <left-run-id> <right-run-id>`
- `tze explain-change-latest`
- `tze report <run-id|latest>`
- `tool list`
- `tool locate <name>`
- `tool doctor <name>`
- `tool <name> -- <args...>`

TView emits readable packet summaries and optional JSONL events using `omnix.tview.packet.v1`.
Simplex analysis codes use the `NET.TCP.*` namespace, including `NET.TCP.HTTP_PLAINTEXT`,
`NET.TCP.TEXT_UTF8`, and `NET.TCP.TLS_OPAQUE`.

Defense diagnostics are non-destructive. They collect CPU, memory, log, PID, or port evidence and
suggest manual next actions without killing processes or changing firewall/service state.

Persona modes are display-only behavior profiles. `Premise`, `Cynic`, `Professional`, and `Neutral`
may shape identity and tone readouts, but never change command authority, routing, guardrails, or safety checks.

## Command Families

- Analyst flows: `ingest`, `analyze`, `decide`, `case`, `incident`
- Build flows: `doctor`, `preflight`, `build`, `recipe author`
- Tool flows: `tool list`, `tool locate`, `tool doctor`, `tool <name> -- <args...>`
- TZE replay flows: `tze latest`, `tze replay`, `tze chain`, `tze diff`, `tze report`, `tze explain-change`
- Recursive explanation flow: `why latest` or shell `/why`
- Next-action flow: `next latest`, shell `next`, or shell `/next`
- Context reset flow: `context reset`, `memory reset-context`, or `memory prune-expired`
- API/link UX flows: `api status`, `api configure`, `link install`
- Definition flows: `define <symbol-or-term>`, `explain <command-or-symbol>`

## Shell Idioms

The interactive shell accepts plain OmniX language and a few convenience aliases:

- `ask what is the sun`
- `provider`
- `Ollama, secure my system`
- `Run NMAP with a local /24 scan`
- `/assist on`
- `/provider`
- `/api`
- `/api openai`
- `/api ollama`
- `/why`
- `/next`
- `/reset`
- `/reset memory`

`ask ...` in shell is normalized into the same request path as `omnix ask`.

`/reset` clears only the current shell run/case observer state. `/reset memory` also clears volatile learned/runtime caches.

## Guarded Assist

- `omnix shell --assist` enables guarded assist inside the shell.
- `omnix ask <prompt> --assist` enables guarded assist for one request.
- `provider probe` verifies whether `ollama`, `openai`, or `null` is active and ready.
- `api status` checks environment and repo-local `.env` without printing secrets.
- `api configure openai` writes repo-local `.env` with restrictive permissions where supported.
- `api template huggingface` prints a placeholder curl command only; HuggingFace is not a runtime provider yet.
- For `deepnimsec-omni:latest`, a stale custom model should be repaired with `./scripts/omnix_deepnimsec.sh --refresh-model`.
- `./scripts/omnix_openai.sh` loads repo-local `./.env` for explicit OpenAI assist runs without printing secrets.
- OpenAI freeform answers are a final `ask --assist` fallback after local memory, definitions, command routing, and guarded tool planning miss.
- `recipe author <source-path>` is the local-path-only X++ module authoring surface for model-authored build recipes.
- Assist output is validated before use.
- Command routing is allowlisted.
- Tool execution stays guarded.
- External Nmap targets are rejected. Guarded Nmap scanning is loopback-only.

## Tool Guardrails

- `Run NMAP` defaults to a safe version probe.
- `Run NMAP Scan` is limited to loopback.
- `Run NMAP with a local /24 scan` maps to `127.0.0.0/24`.
- Requests such as scanning `192.168.1.1-254` are blocked instead of being silently rewritten.

## Definitions

General concept definitions resolve in this order:

1. operator-authored memory or `res/local_glossary.tsv` with matching domain
2. operator-authored memory or `res/local_glossary.tsv` without a domain
3. stored final artifacts marked `memory_artifact`
4. macOS Dictionary Services
5. opt-in Merriam-Webster fallback
6. unresolved local-definition response

`res/local_glossary.tsv` is the hand-authored operator teaching dictionary. Exact taught matches win over system-dictionary cache entries. Use the format:

```text
term|domain|definition
sun|science|The Sun is the star at the center of the Solar System.
```

If a temporary learned association starts overriding the expected source truth, use `omnix context reset` or `omnix memory reset-context`. These commands clear cached definitions, compact history, language/uAC state, and assist learning without deleting the source glossary, TZE ledger, cases, recipes, tools, or persona. Use `omnix memory prune-expired` to remove expired temporary runtime entries without a full reset.

## Environment

- `OMNIX_REASONING_PROVIDER=ollama`
- `OMNIX_REASONING_PROVIDER=openai`
- `OMNIX_OLLAMA_MODEL=<model>`
- `OPENAI_API_KEY=<key>`
- `OPENAI_MODEL=<model>`
- `OMNIX_DISABLE_SYSTEM_DICTIONARY=1`
- `OMNIX_ENABLE_WEBSTER_FALLBACK=1`
- `OMNIX_WEBSTER_FIXTURE_FILE=/path/to/webster.html`

## Examples

```bash
./build/omnix provider probe
./scripts/omnix_deepnimsec.sh --refresh-model
./scripts/omnix_openai.sh provider probe --compact
./build/omnix api status --compact
./build/omnix api template huggingface
./build/omnix why latest --compact
./build/omnix next latest --compact
./build/omnix memory prune-expired --compact
./build/omnix link install --with-tze --prefix ~/.local/bin
./scripts/omnix_openai.sh shell --assist
./scripts/omnix_openai.sh "Define Turning Scale"
./scripts/omnix_openai.sh omnix tview port 5000
./build/omnix shell
OMNIX_REASONING_PROVIDER=ollama OMNIX_OLLAMA_MODEL=deepnimsec-omni:latest ./build/omnix shell --assist
OMNIX_REASONING_PROVIDER=ollama OMNIX_OLLAMA_MODEL=fixture ./build/omnix recipe author /path/to/project --no-install
./build/omnix ask "What is the Sun"
./build/omnix ask --assist "Run NMAP"
```
