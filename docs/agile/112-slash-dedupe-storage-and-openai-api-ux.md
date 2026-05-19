# Slash Dedupe Storage And OpenAI API UX

## Summary

This is a future-facing planning slice. OmniX should eventually gain a `slash` storage layer inspired by `../Blackwell` DeviceFabric, and the OpenAI API setup flow should be polished for public release. No runtime command, vendored storage code, or provider behavior changes are part of this slice.

## Slash Storage Direction

`slash` is the OmniX-facing name for a future user-space dedupe storage feature. Blackwell DeviceFabric is the preliminary design reference, especially its local-first and honest-storage posture:

- store data as normal user-space files rather than formatting drives or changing partitions
- address chunks by SHA-256 content hash
- represent files with manifests that can restore original content byte-for-byte
- compress chunks where useful and pack raw chunks when compression is not useful
- track duplicate references, logical bytes, physical bytes, dedupe savings, and restore risk
- verify manifests and stored chunks before trusting an archive
- compact only redundant storage objects after preserving a canonical referenced copy

Future command shape, design-only:

```sh
omnix slash init <fabric-path>
omnix slash ingest <fabric-path> <path>
omnix slash stats <fabric-path>
omnix slash verify <fabric-path>
omnix slash restore <fabric-path> <manifest-id> <output-dir>
omnix slash compact <fabric-path> [--dry-run]
```

## OpenAI API UX Public Release Direction

OmniX already has `omnix api configure openai`, which prompts for model/key, writes repo-local `.env`, masks secrets in output, and keeps OpenAI assistive rather than authoritative.

Future public-release work should improve the user experience instead of adding a new provider path:

- explain what will be written before asking for secrets
- preview the `.env` path and provider/model settings without printing the key
- confirm that OpenAI is assist-only and never execution authority
- show next commands such as `omnix api status --compact`, `omnix provider probe --compact`, and `./scripts/omnix_openai.sh provider probe --compact`
- polish `/api openai` in `omnix shell` into the same friendly wizard

## Boundaries

- Do not vendor or call Blackwell yet.
- Do not implement `omnix slash` yet.
- Do not change OpenAI provider behavior yet.
- Do not store secrets in TZE history, reports, memory, or slash manifests.

## References

- `../Blackwell` DeviceFabric: local user-space dedupe fabric, SHA-256 chunks, manifests, restore/verify, stats, and compaction.
- Current OmniX API UX: `omnix api status`, `omnix api configure openai`, `/api openai`, and `./scripts/omnix_openai.sh`.
