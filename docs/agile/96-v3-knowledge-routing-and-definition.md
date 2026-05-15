# V3 Knowledge Routing and Definition

## Why This File Exists
This file is the summary home and implementation contract for the recent shell and `ask` behavior gap around general knowledge questions. The immediate goal is to stop prompts like `What is the Sun` and `What is matter in terms of science` from collapsing into `unknown_query` or being misread as `what matters`.

## Conversation Summary
- A vague prompt like `What is matter` exposed that Omni could confuse a concept-definition request with a contextual-summary request like `what matters`.
- We agreed that first-guess routing is not enough. Omni needs bounded comparative interpretation before it commits to a response.
- We locked the target behavior: `omnix ask "What is the Sun"` should check memory first, miss cleanly, seek a local definition source, and return the chosen answer as a concise definition.

## Decision Lock
- Summary location: `docs/agile/96-v3-knowledge-routing-and-definition.md`
- Operator teaching source: `res/local_glossary.tsv`
- Operator-authored exact matches outrank system dictionary and cached reference definitions
- Assist role: wording polish only, not the authoritative first definition source

## Routing Contract
### Intent Split
- `what is <concept>` and `define <concept>` default to `GeneralDefinitionQuery` unless the concept is clearly a TZE/runtime symbol.
- `what matters` and related phrases default to contextual-summary routing.
- Explicit runtime-symbol requests such as `define xProcessingCache` stay on the symbol-definition path.

### Comparative Interpretation
- Candidate classes are:
  - `general_definition_query`
  - `context_summary_request`
  - `symbol_definition_query`
  - `tool_or_command_request`
  - `unknown`
- Routing signals include:
  - phrase shape
  - domain hints like `science`, `computing`, `security`, `math`
  - shell lexicon confidence
  - recent run/case/incident context
  - learned corrections
- If the top candidates are too close, Omni asks a short clarifying question instead of guessing.

### Definition Source Ladder
1. operator-authored memory or glossary with matching domain
2. operator-authored memory or glossary without domain
3. stored final artifacts marked `memory_artifact`
4. local system dictionary
5. opt-in Webster fallback
6. unresolved local-definition message

### Expected Answers
- `What is the Sun`
  - `The Sun is the star at the center of the Solar System.`
- `What is matter in terms of science`
  - `In science, matter is physical substance that occupies space and possesses mass.`
- `What matters`
  - should summarize the current local context, not trigger dictionary lookup

## Implementation Notes
- Add `RequestIntent::GeneralDefinitionQuery`
- Persist `DefinitionAnswer` provenance into TZE runs and stored definitions
- Extend shell lexicon coverage for:
  - `what is matter`
  - `what is the sun`
  - `define matter`
  - `matter in science`
  - `what matters`
  - `what matters here`
- Keep the route deterministic even when assist is enabled

## Validation Targets
- `omnix ask "What is the Sun"` resolves as a definition query
- `omnix ask "What is matter in terms of science"` beats the `what matters` route
- `omnix ask "What matters"` stays contextual
- `omnix ask "matter"` asks for clarification instead of guessing
