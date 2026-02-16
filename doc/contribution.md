# Contribution Guide

Thanks for contributing to `Kongnitive EdgeMCP`.

## Scope

- Runtime behavior changes should prefer Lua script updates via MCP tools when possible.
- Firmware/C changes are for runtime platform capabilities (MCP server, tooling, bindings, transport, OTA, safety).

## Development Environment

1. Install ESP-IDF v5.0+ and export environment.
2. Configure Wi-Fi and defaults in `sdkconfig.defaults`.
3. Configure Wi-Fi, then build and flash:

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

4. Read the ESP32-S3 monitor logs and capture the assigned STA IP, then use that IP for MCP client configuration (for example `http://<ip>/mcp` or `wss://<ip>/mcp`).

## Branch Management

We use a lightweight `main + dev` model:

- `main`: release-ready code only. Protect this branch (no direct push).
- `dev`: integration branch for daily development.
- Feature/fix/docs work should branch from `dev` and merge back to `dev` via PR.
- Feature flow:
  - Create a branch from `dev`: `feat/*`, `fix/*`, `docs/*`, etc.
  - Keep the branch focused on one issue.
  - Open PR to `dev` with linked issue and test evidence.
  - After merge, delete the feature branch.
  - Periodically fast-forward/sync `dev` to `main` for stable releases.

## Branch Naming

Use lowercase names and hyphenated descriptions.

- `feat/<scope>-<short-desc>`
- `fix/<scope>-<short-desc>`
- `docs/<scope>-<short-desc>`
- `chore/<scope>-<short-desc>`
- `refactor/<scope>-<short-desc>`
- `test/<scope>-<short-desc>`

Examples:

- `feat/mcp-add-ota-status-fields`
- `fix/lua-runtime-memory-report`
- `docs/contribution-branch-policy`

## Issue Creation

Create an issue before coding for all non-trivial work.

Include the following fields:

- **Title**: clear and actionable
- **Background**: why this is needed
- **Current behavior**: what happens now
- **Expected behavior**: what should happen
- **Reproduction**: steps/logs (if bug)
- **Proposed solution**: optional initial approach
- **Impact**: modules/files/tools affected
- **Acceptance criteria**: testable checklist

Label guidance:

- Type: `bug`, `feature`, `docs`, `chore`
- Area: `runtime`, `mcp`, `lua`, `ota`, `wifi`, `docs`
- Priority: `p0` - `p3`

PRs should reference issues with `Closes #<id>` when applicable.

## Coding Rules

- Keep code and docs in English.
- Keep changes minimal and focused.
- Do not commit secrets, private certs, or local IDE metadata.
- Follow existing C style and naming patterns in `main/`.
- Update docs when behavior or tools change.

## MCP/Lua Workflow Expectations

When adding or changing tools:

- Update tool registry and validation in `main/mcp_tools.c`.
- Verify behavior on device using logs (`sys_get_logs`).
- Document the tool and example calls in `README.md`.
- If agent behavior changes, update `MCP_AGENT_CONFIG.md`.

## Testing

Before opening a PR:

- Build succeeds locally (`idf.py build`).
- Device smoke test passes for changed area.
- No regressions in core tools (`get_status`, `sys_get_logs`, `lua_*`).

## Pull Request Checklist

- [ ] Clear description of what changed and why
- [ ] Linked issue or context
- [ ] Build/test evidence included
- [ ] Docs updated (`README.md`, `MCP_AGENT_CONFIG.md`, or `doc/` as needed)
- [ ] No secrets or machine-local files added

## Commit Message Style

Use conventional prefixes when possible:

- `feat:` new capability
- `fix:` bug fix
- `docs:` documentation only
- `chore:` maintenance/refactor without behavior change

Example:

```text
docs: add contribution guide for ESP32 MCP workflow
```
