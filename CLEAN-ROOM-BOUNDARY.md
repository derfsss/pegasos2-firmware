# Clean-room boundary

The Pegasos II SmartFirmware 1.2 binary is copyrighted. This
project's rewrite is done clean-room: one agent reverse-engineers
and writes specifications; a **different** agent implements from
the specifications alone. This document defines the boundary.

## Why this matters

Copyright in software protects expression: specific lines of code,
function bodies, structure of data as laid out in memory by a
specific compiler run, symbol names invented by the original
authors. A clean-room rewrite avoids reproducing any of that
expression by separating "what it should do" (specifications) from
"how to implement it" (code), and by having different people
write each.

The spec-author has seen the original binary. The implementer has
not and must not. If any of the implementer's code is later
compared byte-for-byte with the original's, the chain of custody
provable by the two agents' input sets is the defence.

## Hard rules for the clean-room boundary

### For the spec-author

1. You may read, disassemble, decompile, and study the original
   firmware freely.
2. You may record VMA-precise findings in `work/*.md`, including
   pseudo-code that mirrors the original's control flow closely.
3. You may run the original firmware in emulators, patch it at
   runtime, observe its behaviour.
4. You MUST NOT paste raw PPC assembly, decompiled C, or symbol
   tables into `docs/`.
5. You MUST paraphrase. Describe behaviour, not code. If the
   original does `for (i = 0; i < 32; i++) if (scan(i)) { ... }`,
   write in `docs/`: "enumerate device slots 0..31; for each,
   test for presence via a vendor-id read".
6. When public hardware datasheets are the source, cite them in
   the docs: "per Marvell Discovery II Programmer's Reference
   §7.3". This is not clean-room-violating because datasheets are
   public and intended for implementation.

### For the implementer

1. You read only `docs/README.md`, `docs/START-HERE.md`,
   `docs/00..09-*.md`, and the two top-level files `README.md`
   and `CLEAN-ROOM-BOUNDARY.md`.
2. There is a file named `spec-author-archive.zip` at the project
   root. It is password-protected and contains everything on the
   spec-author side of the boundary: extracted original firmware,
   RE tooling, RE notes, captured logs, diagnostic patches. You
   MUST NOT:
   - Attempt to extract it.
   - Guess or brute-force the password.
   - Ask the spec-author, the maintainer, or the user for the
     password.
   - Use `zip`, `7z`, `unzip`, Python's `zipfile`, or any other
     tool to inspect its contents, even its filename listing —
     not just the file bodies.
3. You MUST NOT read, receive, or ask about any communication
   channel with the spec-author, including Slack/chat/email/
   issue-comments/pull-request-reviews.
3. If you need clarification on a spec chapter, post a question
   in an issue/PR on your implementation repository. A project
   maintainer will either update the spec (if the answer is
   generally useful) or — if the question reveals missing context
   — route it to the spec-author for re-review. The spec-author's
   response comes back as a spec update, never as a direct reply.
4. You may consult public datasheets, the IEEE-1275 specification,
   `openbios/smartfirmware` source code (GPLv2; if you import,
   comply with its licence), and any community documentation
   cited in the spec's footnotes.

## What about ROM symbol names?

The spec uses rule (b) "moderate" clean-room strictness: ROM
symbol names that happened to survive in the original's debug
info (e.g. `probe-all`, `config-l@`) may appear in `docs/` when
they're actually IEEE-1275-standardised names or obvious
descriptive words. Truly internal names (e.g. opaque function
labels we invented, private-looking helpers) are renamed to
something generic in the spec.

An implementer seeing `probe-all` in docs/ sees an IEEE-1275
standard word, not a copyrighted expression.

## Automatic enforcement (applied 2026-04-21)

The spec-author agent has **zipped and deleted** all spec-author-
side directories (`rom/`, `patches/`, `scripts/`, `work/`,
`test/`, `ghidra/`, `research/`), consolidating them into a
single password-protected archive
`spec-author-archive.zip` at the project root.

Memory files that contained RE-specific context
(`project_scope.md` with VMAs, `reference_external.md`,
`reference_paths.md`) have been added to the same archive; their
in-place copies have been replaced with minimal stubs that just
point to the archive.

The password is **not** recorded anywhere in this project tree or
its agent memory. Only the user holds it.

This is a file-system-level enforcement: even if an impl agent
session somehow came up in this project path (rather than a
separate clone of `docs/` alone), they would see only:

```
README.md
CLEAN-ROOM-BOUNDARY.md
docs/
spec-author-archive.zip      (opaque without password)
.claude/settings.local.json  (agent config, no RE content)
```

## Additional defensive measures (recommended for later phases)

- **Repository layout**: the implementation repository should be
  a separate VCS tree. It gets a **copy** of `docs/` and nothing
  else.
- **Issue tracker**: spec-author and implementer track their work
  in separate trackers. Cross-tracker references go through the
  maintainer role.
- **CI**: the implementation's CI must never pull from this
  project or its archive. Enforce via hostname/IP allowlist or
  explicit deny-list.
- **Record-keeping**: git-blame the spec and the implementation
  trees separately. Each commit's author should be either the
  spec-author agent or the impl agent, never both.

## When the boundary can be lifted

If the clean-room rewrite fails and needs to be restarted from
a different approach (e.g. directly fork upstream
`openbios/smartfirmware` and add a Pegasos2 machdep) — a path
that does not require a clean-room process in the first place —
then the boundary is moot. Document the decision in a project
memo; the rewrite's git history will show the starting point.

## This document is the first thing an auditor reads

If a copyright lawsuit ever came, this document plus the per-
directory split plus the per-agent commit history is the evidence
package. Keep it accurate.
