# Niagara compile and generated-source inspection

`content.niagara.system.compile.request` is the explicit compile boundary for a
loaded Niagara System. It accepts `waitForCompletion` (default `true`) and
`force` (default `false`), then returns aggregate status, bounded per-script
rows, compiler events, and whether the engine still has outstanding VM or GPU
work. A successful cache hit may report `compilationLaunched: false`; that
boolean only says whether a new job launched. When synchronous waiting cannot
finish, the command returns a typed `niagara_compile_pending` result with the
bounded status payload. Compile requests update transient editor compile state,
do not save the package, and do not modify authored graph values.

`content.niagara.system.diagnostics.get` is a pure query. It reads the latest
per-script status, source-synchronization state, compiler error text, bounded
compile events, VM operation/register statistics, and renderer feedback. It
never compiles; callers that need fresh evidence must call
`content.niagara.system.compile.request` first. `diagnosticsMayBeStale` is true
when work is pending, a script is dirty or unsynchronized, or status is unknown.
The response has a shared row and character budget and reports truncation.

`content.niagara.emitter.gpu_hlsl.get` is also a pure query. It requires an
exact case-sensitive emitter handle name, unique instance name, or handle GUID.
If more than one handle matches a textual selector it returns
`emitter_ambiguous`; use the GUID to disambiguate. The selected emitter must use
`GPUComputeSim`. The result contains generated `ParticleGPUComputeScript`
source, preferring GPU HLSL, then regular HLSL, then assembly translation.
`maxCharacters` is bounded to 1 MiB by default and 4 MiB maximum. The response
includes source synchronization, compile status, pending state, and truncation
metadata. A caller requiring fresh source must explicitly call
`content.niagara.system.compile.request` first.

Compile and source responses are authored-editor evidence only. They do not
prove a runtime Niagara component executed successfully or that a rendered GPU
frame accepted the generated shader. Renderer feedback similarly reports editor
compatibility diagnostics, not runtime acceptance.
