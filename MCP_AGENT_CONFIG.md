# Kongnitive EdgeMCP Agent Configuration Guide

How to connect AI clients to the device MCP endpoint.

## Endpoint

Use HTTP MCP endpoint:

```text
http://<DEVICE_IP>/mcp
```

Example used in this doc:

```text
http://192.168.1.31/mcp
```

## Claude Code

Add the MCP server with CLI:

```bash
claude mcp add --transport http edgemcp http://IP/mcp
```

Example:

```bash
claude mcp add --transport http edgemcp http://192.168.1.31/mcp
```

## Codex (config.toml)

Add this section to your Codex `config.toml`:

```toml
[mcp_servers.edgemcp]
type = "http"
url = "http://192.168.1.31/mcp"
```

## Quick Check (curl)

```bash
curl -X POST http://192.168.1.31/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
```

## Recommended First Calls

1. `get_system_prompt`
2. `get_status`
3. `lua_list_scripts`
4. `sys_get_logs`

## Available Tools (14)

- `control_led`
- `get_status`
- `get_system_prompt`
- `sys_get_logs`
- `sys_ota_push`
- `sys_ota_status`
- `sys_ota_rollback`
- `sys_reboot`
- `lua_push_script`
- `lua_get_script`
- `lua_list_scripts`
- `lua_exec`
- `lua_bind_dependency`
- `lua_restart`
