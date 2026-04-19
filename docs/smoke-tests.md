# OmniX Smoke Tests

Use these steps on a fresh macOS or Linux host after unpacking a packaged OmniX release:

1. Verify the binary boots and prints its version.
   - `./bin/omnix --version`
2. Run the dependency and recipe checks before building.
   - `./bin/omnix preflight nmap --memory-root ~/.omnix-smoke`
   - `./bin/omnix doctor nmap --memory-root ~/.omnix-smoke`
   - `./bin/omnix tool inspect-host -- --linux`
3. Execute the portable alias builds.
   - `./bin/omnix build nmap --memory-root ~/.omnix-smoke`
   - `./bin/omnix build fmt --memory-root ~/.omnix-smoke`
   - `./bin/omnix build tinyxml2 --memory-root ~/.omnix-smoke`
   - `./bin/omnix build lua --memory-root ~/.omnix-smoke`
4. Confirm the managed state was created.
   - `~/.omnix-smoke/history.jsonl`
   - `~/.omnix-smoke/projects.json`
   - `~/.omnix-smoke/native_tools.json`
   - `~/.omnix-smoke/host-inspection/`
   - `~/.omnix-smoke/installs/`
   - `~/.omnix-smoke/logs/`

The Linux-first host inspection step is intended to capture:

- local users from `/etc/passwd`
- sudoers and `/etc/sudoers.d`
- package-manager and mirror-list hints
- common log locations and syslog/auth patterns
- `lastlog` metadata and samples when available
- cron and crontab locations
- systemd unit paths and enabled services
- initrd/initramfs paths
- cached native-tool providers such as `nmap`, `ssh`, `grep`, `sed`, and `awk`

You can also use the helper script to automate the packaged-binary checks and capture evidence:

- `sh scripts/smoke_test_package.sh --package build/release/omnix-<version>-<os>-<arch>.tar.gz --evidence-dir build/smoke-evidence`
- Add `--offline --skip-builds` when you only want bootstrap, version, preflight, and doctor coverage.

## Daily Omni Runner

Use the daily runner when you want OmniX to execute the core analyst workflow for this repo in one pass:

- `sh scripts/daily_omnix.sh`

What the script does:

1. Lists the current Omni case inventory.
2. Selects a working case, preferring a case tied to `res/tze.txt`.
3. Falls back to analyzing `res/tze.txt` if no suitable case already exists.
4. Runs `omnix case <id>` to inspect the selected case.
5. Runs `omnix decide <id>` to rank the current safe next actions.
6. Runs `omnix tool inspect-log -- <source>` for structured evidence inspection.
7. Runs `omnix tool inspect-build -- <repo-root>` to check local build readiness.
8. Runs `omnix tool text-pipeline -- <file> ...` against a repo document to surface high-signal lines.
9. Runs `omnix tool report-case -- <id>` to generate a saved analyst report.
10. Writes each step into a timestamped run directory under `build/daily/` by default, plus a final `summary.txt`.

Useful options:

- `sh scripts/daily_omnix.sh --case-id <case-id>`
- `sh scripts/daily_omnix.sh --memory-root build/daily-memory`
- `sh scripts/daily_omnix.sh --output-dir build/daily-runs`
- `sh scripts/daily_omnix.sh --skip-report`

The script is designed to be safe to run manually or from Omni. On this machine, invoking it with `sh scripts/daily_omnix.sh` is the most reliable form.
