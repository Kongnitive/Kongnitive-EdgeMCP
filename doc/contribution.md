# Contribution Guide

Thanks for contributing to `Kongnitive EdgeMCP`.

## Scope

- Runtime behavior changes should prefer Lua script updates via MCP tools when possible.
- Firmware/C changes are for runtime platform capabilities (MCP server, tooling, bindings, transport, OTA, safety).

## Development Environment

1. Install ESP-IDF v5.0+ and export environment.
2. Configure Wi-Fi and defaults in `sdkconfig.defaults`.
3. Build and flash:

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

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

