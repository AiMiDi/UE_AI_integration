# UE-AI-CLI 1.0 release contract

The release artifact is produced only by `scripts/package_release.ps1`. It
stages the BuildPlugin output, both CLIs, production MCP dependencies, Trace
Worker, Skills, Recipes, capability manifests, and protocol resources, then
creates a per-file SHA-256 manifest. A raw BuildPlugin directory is not a
release artifact.

The black-box gate extracts the final archive into an empty directory and
checks both doctors, install layout, capability and Skill discovery, exactly
12 MCP tools, Worker handshake, and—when the UE 5.3 fixture is supplied—Editor
health plus a schema-validated read-only Blueprint asset query.

The 1.0 safety layers are:

- an installable `$ue-ai` client entry Skill that declares the local MCP
  dependency and routes to packaged domain Skills without becoming an
  execution authority;
- `ue-cli doctor --full`, redacted diagnostic bundles, atomic instance discovery,
  and `ue-cli test-tools`;
- bounded Recipe v2 execution with checkpoint/resume/cancel, approvals,
  source-control preflight, and reverse compensation;
- capability schema v3 effects/lifecycle/tombstones;
- protected PIE, compile, restart, and performance leases bound to live client
  sessions;
- local project/config and isolated read-only asset-header workers;
- SAL `stub|lint|plan`, which cannot execute, import, or access I/O;
- an explicit-opt-in Development/DebugGame local IPC bridge that is absent
  from Shipping and never terminates a user-owned process.

Release counts are derived from the manifests by validation and are never
hard-coded in runtime logic or tests.
