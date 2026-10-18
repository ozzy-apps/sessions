# Project instructions

- Keep `handoff.md` current at each logical step: project purpose, native C++17 / Qt 6 Widgets stack, completed work, verification and remaining steps for the next assistant.
- UI text must be functional. Match the existing Sessions surfaces, typography and theme colors; use Material 3 as the general template where no more specific device or view direction applies.
- Describe the design in the project's own terms in the handoff.

## Parallel development

- Codex normally works in the main checkout: `lattice-m1/lattice`.
- Claude works in the sibling `lattice-m1/lattice-sampler` worktree. Keep that worktree for future parallel work; do not remove it after a release.
- Before new parallel work, inspect each checkout's status and fetch `origin`. Merge `origin/main` into the working feature branch early, or start a fresh branch from it, so integration stays small.
- Refresh from `origin/main` again before the final combined build and regression run. Preserve both sides of documentation additions and the newer release version when combining work.
- Do not modify another assistant's worktree or discard its local changes. Coordinate if a clean update is not possible.
- Push only when the user explicitly requests it for the current update. Use normal fast-forward pushes; never force-push shared history.
- After a published update, bring a clean main checkout up to date with `git pull --ff-only`. Do not pull over uncommitted work or merge an active feature branch without authorization.

Integration record (2026-10-18): the 0.34.0 browser release was combined with `93af5a8` / 0.33.0 (Arrangement clip fades), which includes `dc229a8` / 0.32.0 (Sampler and Osmium). All five combined suites passed, including the 11 fade checks. Always fetch the current origin/main again for the next update.
