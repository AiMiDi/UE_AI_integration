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
- map-safe LevelScriptBlueprint persistence plus Workflow v2
  `levelBlueprint` transactions, failed-run receipts, read-back, and verified
  `.umap` rollback;
- typed Vector, Rotator, Transform, and LinearColor pin defaults with
  post-compile and package-reload verification;
- session Recipes restricted to `sessionSafe` capabilities, an owned PIE
  generation, a caller-bound PIE lease, and reverse runtime compensation;
- capability schema v3 effects/lifecycle/tombstones;
- protected PIE, compile, restart, and performance leases bound to live client
  sessions;
- local project/config and isolated read-only asset-header workers;
- SAL `stub|lint|plan`, which cannot execute, import, or access I/O;
- an explicit-opt-in Development/DebugGame local IPC bridge that is absent
  from Shipping and never terminates a user-owned process.

`ue-cli doctor --full` compares the CLI and loaded module source revision,
plugin descriptor/compiled version, capability catalog digest, and both
Workflow contract digests. Instance records never publish request parameters;
after a stale Editor they can expose only the bounded last request identity and
a redacted relative CrashContext artifact path.

Release counts are derived from the manifests by validation and are never
hard-coded in runtime logic or tests.
