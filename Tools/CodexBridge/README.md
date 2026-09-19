# CodexBridge

This directory contains the local MCP adapter for the Unreal Editor plugin.

## Build

```powershell
dotnet build .\Tools\CodexBridge\codex-mcp.csproj -c Release
```

## Run manually

Start `RobotProjetGame.uproject` in Unreal Editor, then run:

```powershell
dotnet run --project .\Tools\CodexBridge\codex-mcp.csproj -- --url http://127.0.0.1:31010
```

The MCP process speaks JSON-RPC over stdin/stdout. Logs go to stderr so it can be used directly by Codex.

## Codex configuration

Add this to the user Codex configuration, adapting the absolute paths if needed:

```toml
[mcp_servers.unreal_blueprint]
command = "C:\\Program Files\\dotnet\\dotnet.exe"
args = [
  "run",
  "--project",
  "D:\\Documents\\01_GamingCampus\\02_Year\\Projet_Libre\\GittRep\\RobotPG\\Tools\\CodexBridge\\codex-mcp.csproj",
  "--",
  "--url",
  "http://127.0.0.1:31010"
]
cwd = "D:\\Documents\\01_GamingCampus\\02_Year\\Projet_Libre\\GittRep\\RobotPG"
startup_timeout_sec = 20
tool_timeout_sec = 120
default_tools_approval_mode = "prompt"
```

The first MVP exposes `unreal_health`, `inspect_blueprint`, and `compile_blueprint`. It does not modify or save assets yet.
