<!-- TRELLIS:START -->
# Trellis Instructions

These instructions are for AI assistants working in this project.

This project is managed by Trellis. The working knowledge you need lives under `.trellis/`:

- `.trellis/workflow.md` — development phases, when to create tasks, skill routing
- `.trellis/spec/` — package- and layer-scoped coding guidelines (read before writing code in a given layer)
- `.trellis/workspace/` — per-developer journals and session traces
- `.trellis/tasks/` — active and archived tasks (PRDs, research, jsonl context)

If a Trellis command is available on your platform (e.g. `/trellis:finish-work`, `/trellis:continue`), prefer it over manual steps. Not every platform exposes every command.

If you're using Codex or another agent-capable tool, additional project-scoped helpers may live in:
- `.agents/skills/` — reusable Trellis skills
- `.codex/agents/` — optional custom subagents

Managed by Trellis. Edits outside this block are preserved; edits inside may be overwritten by a future `trellis update`.

<!-- TRELLIS:END -->

# MonitorSystem Project Rules

- This is a C++17/Linux monitoring system. Keep `worker/`, `manager/`, and
  `proto/` boundaries explicit; a Protobuf change is a shared contract change.
- Treat production readiness as the default: do not add plaintext credentials,
  insecure network paths, shell-injection risks, or unchecked persistence
  failures.
- Before changing C++ or Linux runtime behavior, inspect the relevant source
  and `docs/ai/`; use narrow edits and verify with the real CMake/test/runtime
  path when available.
- `docs/ai/` contains repository-specific architecture and risk notes.
  Update it when an implemented change alters a documented operational
  contract or validation procedure.
- Trellis session/task bookkeeping must remain reviewable: automatic Trellis
  commits are disabled in `.trellis/config.yaml`.
