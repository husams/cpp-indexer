# .backlog

Backlog configuration for this repository: planned features, user stories,
subtasks, assignments, status, review threads and PR links.

- `workflow.yaml` — the project's **transition workflow** (the shipped default
  flow: Created → Ready → In Progress → In Review → Accepted → Done, with an
  Incomplete refinement path and a Needs Work review loop). It is a complete
  definition: it replaces the skill's bundled default rather than merging with
  it.
- `artifacts/<KEY>/` — design notes, specs, logs attached to a backlog item.

There is **no local SQLite store**: the backlog lives on the shared PostgreSQL
server selected by `BACKLOG_DB` / `BACK_LOG_URL`, project slug `cpp-indexer`.
Run `backlog where` to confirm before acting.

Drive it with the `backlog` skill (`backlog-plugin:backlog`), never by opening
the database:

```
~/.claude/skills/backlog/bin/backlog where
~/.claude/skills/backlog/bin/backlog board
~/.claude/skills/backlog/bin/backlog next --actor developer
```
