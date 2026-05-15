# Legacy X++ Translation Map

## Summary
- Status: active nomenclature cleanup map
- Goal: keep the original X++ source vocabulary searchable while making OmniX runtime output readable to operators
- Rule: legacy symbols remain stable provenance keys; HumanReadable names lead user-facing output
- Source history: [res/tze.txt](../../res/tze.txt)

## Runtime Stage Translation
| Legacy source label | HumanReadable runtime label | Runtime responsibility |
| --- | --- | --- |
| `xProcessingCache` | `Cache.PrepareWorkspace` | Prepare the run workspace, cache budget, and cache operation list. |
| `x.Define.Low` | `Intent.DecodeInstruction` | Decode the resolved intent and instruction slot into an executable family. |
| `x.DisplayPriorityProcessingGate` | `Knowledge.EvidenceRanking` | Rank source references, preference data, and relevant knowledge context. |
| `x.DisplayFeedBackLoop` | `Memory.FeedbackReview` | Load prior outcomes and learned context for the current request. |
| `x.Store` | `Memory.StoreArtifact` | Persist retained run artifacts, history, reports, preferences, and learned records. |

## Storage Namespace Translation
| Legacy storage label | HumanReadable label | Runtime meaning |
| --- | --- | --- |
| `xMap_Temp` | `Storage.Temporary` | Ephemeral work area for transient run state. |
| `xMap_Perm` | `Storage.Permanent` | Persistent memory root for retained OmniX records. |
| `xMap_Core` | `Storage.Core` | Protected core storage namespace reserved for elevated flows. |

## Compatibility Rules
- `TzeStageRecord.stage_id` remains the legacy source key for replay compatibility and source graph matching.
- Reports, replay output, chain output, and memory views should display the HumanReadable label first.
- When a HumanReadable label replaces a legacy symbol in output, include the legacy symbol as provenance, for example `Knowledge.EvidenceRanking (legacy=x.DisplayPriorityProcessingGate)`.
- Definitions and operand lookup must continue to accept legacy symbols because they are part of the original source manuscript.
- No migration of existing `tze_runs.json` files is required.

## Follow-On Cleanup Candidates
- Translate additional assist stages after the provider flows settle.
- Add HumanReadable labels for `uAC` once the preprocessor/postprocessor vocabulary stabilizes.
- Keep extending this map instead of renaming source-history tokens directly.
