# Trading-system roadmap

Captured 2026-05-30 from a free-form brainstorm with the user, after yen v4 produced the first clean result. Holds the open questions / feature wish-list that don't fit cleanly into the existing `AGENT_HANDOFF_LOG.md` per-task entries.

## Layout

- [`questions.md`](questions.md) - the user's 13 numbered items, verbatim.
- [`answers.md`](answers.md) - per-item discussion: what the current code does, what changing it would cost, open design questions.
- [`plan.md`](plan.md) - prioritised tackle order with rough effort estimates.

When you ship work that closes (or partially closes) a roadmap item, update the corresponding section in `answers.md` with a "STATUS: ..." line at the top and append a handoff entry to `AGENT_HANDOFF_LOG.md` with the commit hash.

Two related living docs:
- `agent/AGENT_HANDOFF_LOG.md` - chronological per-task handoffs. The roadmap items here become entries there as they get worked.
- `agent/ibkr_client_audit.md` - the IBKRClient-specific audit (items #7, #8, etc. of the umbrella). Already has its own status table; roadmap items #7 (daemon) and #4 step-by-step trace overlap with that audit.
