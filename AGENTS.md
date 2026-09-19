# RobotProjetGame

## Project

- Unreal Engine: 5.8
- Project file: `RobotProjetGame.uproject`
- Editor target: `RobotProjetGameEditor`
- Blueprint automation plugin: `Plugins/CodexBridge`
- Local MCP adapter: `Tools/CodexBridge`

## Blueprint automation rules

- Never edit `.uasset` or `.umap` files as text.
- Use the CodexBridge Unreal Editor API for Blueprint inspection and future graph edits.
- Keep the Unreal Editor open on this project when using the MCP tools.
- Do not run Blueprint writes while PIE or a simulation session is active.
- Inspect first, apply changes second, compile third, save last.
- Keep write/save tools approval-gated.

## Verification

- Build the MCP adapter with `dotnet build Tools/CodexBridge/codex-mcp.csproj -c Release`.
- Verify the Unreal Editor log contains `CodexBridge listening on http://127.0.0.1:31010`.
- Call `unreal_health` before inspecting or editing a Blueprint.
- Compile the affected Blueprint and review Git changes before committing.

## Repository hygiene

- Do not commit `.idea`, `.slnx`, `Binaries`, `Intermediate`, `Saved`, `DerivedDataCache`, or `.vs`.
- Do not commit generated MCP binaries unless the team explicitly chooses to distribute them.
