# X++ Module Recipe Authoring Phase

## Summary
- Status: active follow-on `v2` phase after `97`
- Goal: let OmniX author, validate, repair, and persist build recipes for exact local source paths
- Source model: the legacy `x.C_P.1` through `x.C_P.4` module flow in [res/tze.txt](../../res/tze.txt), translated into current HumanReadable runtime names
- Provider scope: `ollama` only for this phase

## Runtime Shape
- `RecipeAuthoring.SourceIntake`
  - verify the local source path
  - inspect files and detect the build-system family
  - collect trust/context inputs before recipe synthesis
- `RecipeAuthoring.EvidenceRanking`
  - rank evidence from `CMakeLists.txt`, `Makefile`, `configure`, `meson.build`, `package.json`, `pyproject.toml`
  - rank module/toolchain hints and existing output-layout signals
- `RecipeAuthoring.RecipeDraft`
  - request one strict JSON recipe candidate from Ollama
  - keep the provider output bounded to schema only
- `RecipeAuthoring.ValidateRepairStore`
  - insert the schema-valid recipe into the active authored overlay
  - run `preflight`
  - run `doctor` when dependencies are missing
  - run one real build attempt when ready
  - allow one repair pass using validation feedback
  - persist the final authored recipe as active or inactive

## Interfaces
- New intent: `RequestIntent::AuthorBuildRecipe`
- New CLI:
  - `omnix recipe author <source-path> [--assist] [common build options]`
- Natural language alias:
  - `omnix ask "Create a build recipe for /path/to/project"`
- New memory file:
  - `authored_recipes.json`
- New persisted types:
  - `AuthoredRecipeRecord`
  - `RecipeAuthoringArtifact`

## Behavioral Rules
- Phase 1 is local-path only
- Active authored recipes outrank static alias recipes only for the exact resolved local source path that produced them
- Static alias behavior remains unchanged for everything else
- Validation remains the execution authority even though recipe content is model-authored
- Failed authored recipes stay persisted but inactive

## Acceptance Targets
- a local CMake hello-world project can be authored and built
- a local Makefile project can be authored and built
- a failed first recipe can trigger exactly one repair pass
- replay, chain, and report show `RecipeAuthoring.SourceIntake` through `RecipeAuthoring.ValidateRepairStore`
- later `preflight` and `build` reuse the validated authored recipe for the exact same local path

## Legacy Name Translation
- `x.C_P.1` -> `RecipeAuthoring.SourceIntake`
- `x.C_P.2` -> `RecipeAuthoring.EvidenceRanking`
- `x.C_P.3` -> `RecipeAuthoring.RecipeDraft`
- `x.C_P.4` -> `RecipeAuthoring.ValidateRepairStore`
