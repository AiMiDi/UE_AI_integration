---
name: setup-ue5
description: Install or upgrade UE_AI_integration in an Unreal Engine project, build and install its native CLIs, configure the optional MCP bridge, and install the CLI-first ue-ai entry Skill for Codex or Claude Code. Use for setup ue5, install ue5 plugin, connect to unreal, or missing ue-cli/ue-workflow-cli.
---

# Setup UE_AI_integration

Make the plugin, native CLIs, and client entry Skill usable in the current
project. Prefer CLI for discovery, execution, and verification; retain MCP as
a fallback. Report local tooling, Editor, and Trace Worker readiness separately.

## 1. Locate the project and package

Find the `.uproject` in the current directory or one level up. If none is
found, ask for its path. Read `Modules` and `EngineAssociation` to determine
whether the project can compile the plugin and which engine it uses.

Resolve the source plugin root from this packaged Skill's `../..`, or the
client-provided plugin root such as `CLAUDE_PLUGIN_ROOT`. The destination is
`<ProjectRoot>/Plugins/UE_AI_integration`. Do not copy a directory onto itself.

For a first source installation, retain the complete source package:

- `UE_AI_integration.uplugin`, `Source/`, and `Resources/`;
- root `CMakeLists.txt`, `CLI/`, and complete `Workflow/` including
  `ThirdParty/` and `Contracts/`;
- `MCP/`, `skills/`, `Recipes/`, `scripts/`, `docs/`, and the root READMEs;
- matching `Binaries/` and `Tools/Trace/` when provided by a release package.

A C++ project can build the UE module with its matching engine. A Blueprint-only
project needs a matching prebuilt plugin or a supported plugin build with that
engine. **Building the UE module does not build either native CLI.**

For an existing packaged Win64 installation, follow the root README's
`scripts/install_plugin.ps1` staging/preflight/activation flow. Do not replace
loaded DLLs or stop a user-owned Editor automatically. Release packages must
match the target engine and host platform; do not select binaries merely
because they belong to the latest release.

## 2. Prepare the Node runtime

Require Node.js 20 or newer. From `<PluginRoot>/MCP`, prepare a source checkout:

```powershell
npm ci
npm run build
```

A release package already containing current `MCP/dist` can use
`npm ci --omit=dev` if production dependencies need restoring. Keep the bridge
runtime even when CLI is preferred: some local CLI backends invoke its
JavaScript implementations. MCP registration is a separate, optional client
configuration step documented in the root README.

## 3. Build and install both native CLIs

First inspect `CLI/bin/ue-cli(.exe)` and `CLI/bin/ue-workflow-cli(.exe)`.
Reuse prebuilt binaries only when both run on this host, match the package
version/revision, and pass the catalog/contract checks below.
`scripts/build_plugin.bat` and `scripts/build_plugin.sh` already build and
install both CLIs during normal release packaging.

If either binary is missing, incompatible, or stale, build from the plugin's
root CMake project. Require CMake 3.24+ and a C++20 toolchain. On Windows,
use Visual Studio 2022 Build Tools or a newer toolchain supported by the
installed CMake generator. A missing `cmake` command or compiler is an
unmet prerequisite; do not claim installation is complete.

Substitute absolute paths below. `<BuildDir>` must be outside the plugin and
its install destination; use the workspace's designated temporary/build area.
Run each step only after the previous one succeeds:

```powershell
cmake -S "<PluginRoot>" -B "<BuildDir>" -DUE_WORKFLOW_BUILD_CLI=ON -DUE_WORKFLOW_BUILD_TESTS=OFF
cmake --build "<BuildDir>" --config Release --target ue ue-workflow
cmake --install "<BuildDir>" --config Release --prefix "<PluginRoot>/CLI"
```

For Linux/macOS single-configuration generators, also pass
`-DCMAKE_BUILD_TYPE=Release` during configure. See
[CLI build and acceptance](../../docs/UE_SHORT_CLI.md#构建与分发) for both shells
and the complete installed layout.

The CMake target names are `ue` and `ue-workflow`; their installed filenames
are `ue-cli` and `ue-workflow-cli`. Run `cmake --install` after building:
copying executables alone omits the installed
`CLI/share/ue-workflow-cli/{Capabilities,Contracts,Skills,Recipes}` catalog.

## 4. Locate and validate the installed CLIs

Use absolute executable paths; PATH changes are optional. On Windows:

```powershell
$env:UE_CLI = "<PluginRoot>/CLI/bin/ue-cli.exe"
$env:UE_WORKFLOW_CLI = "<PluginRoot>/CLI/bin/ue-workflow-cli.exe"
& $env:UE_CLI --version --json
& $env:UE_WORKFLOW_CLI --version --json
& $env:UE_CLI capabilities --limit 1 --json
& $env:UE_CLI skills --query blueprint --json
& $env:UE_CLI skills --name ue-blueprint-diagnose --recipe scan-and-verify --detail full --json
& $env:UE_CLI help blueprint.scan --json
& $env:UE_WORKFLOW_CLI doctor --json
```

These acceptance commands do not require Editor. Inspect versions, resolved
catalog roots, result envelopes, and exit codes. Use the manifest-derived
capability total, not a hardcoded release count. The environment variables above
apply to this shell and child processes; the entry Skill can also find
`<PluginRoot>/CLI/bin` directly. Do not overwrite global PATH or existing
client configuration just to make discovery work.

Release 1.0.0 currently ships 564 capabilities across six domains. This is
release metadata; the installed manifest and live availability remain the
acceptance authority when versions change.

## 5. Install the UE AI client entry Skill

For Codex on Windows:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  "<PluginRoot>/scripts/install_entry_skill.ps1" -Client codex
```

For Claude Code / Bash:

```bash
bash "<PluginRoot>/scripts/install_entry_skill.sh" --client claude
```

The installer copies only `ue-ai`. Domain Skills remain in the plugin/CLI
catalog and are discovered with `ue-cli skills`; MCP `ue_skills` is the
fallback. Confirm the installed entry Skill says CLI is preferred for discovery,
execution, and verification.

If the installed copy differs, inspect it before replacing it and merge any
local additions. For an explicit full replacement, `-Force` / `--force`
backs up the old copy; restore relevant local additions afterward. Reload the
client's Skills or open a new session so it reads the updated entry Skill.

## 6. Verify the Editor connection when available

Against the intended running Editor:

```powershell
& $env:UE_CLI status --json
& $env:UE_CLI doctor --full --no-clean-stale-instances --json
& $env:UE_CLI help blueprint.scan --live-schema --json
```

Use the same `UE_PORT` as the target Editor, or its explicit CLI `--endpoint`.
If the port belongs to another instance, identify the correct endpoint; do not
close that process automatically. Neither transport starts the Editor.

Full Doctor also checks the loaded module, bundle, and Trace Worker. A missing
Editor or Worker should be reported as that component's unavailable state,
while retaining successful local CLI checks. CMake builds the portable CLIs;
it does not build the engine-specific Trace Worker. Resolve relevant
version/digest mismatches before dependent operations.

## 7. Report the installed state

Report the plugin path, both CLI paths and versions, catalog acceptance, entry
Skill destination, and Editor/Worker readiness. If Editor is closed, report
local setup completion and the outstanding online checks. If a CLI build failed,
report the failed step and concrete error; do not label CLI setup complete.

The default route is `ue-cli skills` → local Skill instructions →
`ue-cli help` → `ue-cli <capability-id>` or `ue-workflow-cli` → recipe
verification. MCP is available when CLI is absent, unusable, incompatible,
lacks the required feature, or is explicitly requested. Both routes retain
the same approvals, request IDs, recovery, and verification contracts.
