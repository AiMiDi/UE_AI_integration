# Rider file-diagnostics fallback

Use this fallback only when a requested file is outside the project root that
Rider reports as open.

1. Do not pass the external path, or a `..` traversal to it, to Rider's
   `get_file_problems` request. Rider rejects files outside its open project
   boundary.
2. Preserve the caller's canonical absolute path in the response and state
   that Rider diagnostics are unavailable for that file in the current project
   session.
3. Use the narrowest available alternative that can inspect the file without
   pretending to provide Rider diagnostics, such as a read-only source search,
   a compiler/build diagnostic, or a UE CLI capability whose discovered schema
   accepts that path.
4. Identify the alternative backend and its scope in the result. If no
   alternative is available, stop after reporting the Rider boundary rather
   than guessing diagnostics.
