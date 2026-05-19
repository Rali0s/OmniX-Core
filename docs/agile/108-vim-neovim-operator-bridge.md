# VI, Vim, and Neovim Operator Bridge

## Summary

OmniX should eventually integrate with `vi`, `vim`, and `nvim` as optional operator workspaces. The bridge should detect editor capability, collect explicit file or buffer context, preview patches, and support future Neovim RPC without making any editor a required core dependency.

## Goals

- Detect `vi`, `vim`, and `nvim` availability through local PATH/tool discovery.
- Keep editor integration optional and local-first.
- Let operators pass explicit file paths or buffer snapshots into OmniX for explanation, review, patch preview, or TZE case capture.
- Provide patch-preview workflows that make OmniX suggestions visible before any file mutation.
- Support future Neovim RPC only when the operator explicitly opts in.

## Future CLI Shape

```sh
omnix editor doctor
omnix editor vim doctor
omnix editor vim open <file>
omnix editor vim explain-buffer <file>
omnix editor vim collect-context <file>
omnix editor vim patch-preview <file>
omnix editor nvim rpc doctor
```

## Guardrails

- No editor dependency in core OmniX.
- No hidden buffer reads; file or buffer context must be explicit.
- No automatic writes to open editor buffers.
- No plugin installation without operator consent.
- Neovim RPC is a later adapter, not v1 runtime authority.

## Use Cases

- Explain the current file or selected local path without leaving the terminal workflow.
- Preview an OmniX patch before applying it.
- Capture a failing code edit session into a TZE evidence bundle.
- Route editor context into Recursive Why/Diff when a patch or build failed.

## Test Plan

- `tool locate vi`, `tool locate vim`, and `tool locate nvim` should eventually resolve when available.
- Future `omnix editor vim doctor` should report which editor families are available.
- Patch preview should produce a deterministic artifact without modifying the target file.
- Missing editors should return a clean unavailable status with install guidance, not a hard failure.
