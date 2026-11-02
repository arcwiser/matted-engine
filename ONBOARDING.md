# Matted Engine - Agent Onboarding (pointer file)

**AGENTS.md is the team contract. `scripts\onboard.cmd` is the onboarding flow.**
This file exists only so a human (or agent) landing in the repo root finds the
door. Do not duplicate protocol here — it lives in AGENTS.md.

## The 60-second version

1. Read **AGENTS.md** (contract: team, ownership, comms rules).
2. New agents: have the human run **`scripts\onboard.cmd`**, paste its output
   into your chat, then run `python tools\mattedcomms\mattedcomms.py join --user <YourName>`.
3. Start your **mandatory standing background task** immediately and never let
   it die (restart if the shell dies):
   `python tools\mattedcomms\mattedcomms.py watch --user <YourName>`
4. `task list` -> claim what is assigned to you -> announce in `#tasks`.
5. Team health at a glance: `python tools\mattedcomms\mattedcomms.py status`.

Comms rules that bite hardest (details in AGENTS.md): ack anything addressed
to you within ~5 minutes; `task claim` before working; post build breakages in
`#build` immediately; never edit another agent's files without an ack in
`#general`. While `watch` runs, do NOT also run plain `recv` for the same
user (they share a read cursor) — use `history` or `unread` for ad-hoc reads.

## For the user: onboarding a new agent

Preferred: `scripts\onboard.cmd` — copy its output into the new agent chat.

Fallback template (if the script is unavailable):

```text
You are <NAME> (e.g. AgentC), a new AI engineer joining the "Matted Engine"
team. Repo: C:\Users\Admin\Desktop\matted engine. Team chat: MattedComms
(local tool in the repo; always pass --user <NAME>).

BOOT SEQUENCE - complete all of it before writing any code:
1. Read AGENTS.md (team contract) and this onboarding pointer file.
2. python tools\mattedcomms\mattedcomms.py join --user <NAME>
   (posts your arrival, prints the digest, lists unclaimed tasks)
3. Start your standing background task NOW and keep it alive:
   python tools\mattedcomms\mattedcomms.py watch --user <NAME>
4. python tools\mattedcomms\mattedcomms.py task list
5. Do NOT claim anything on your own. Post in #general, ask AgentA/AgentB
   what to pick up, and only work what they assign (task claim --id N first).

While working: ack anything addressed to you within ~5 minutes, post build
breakages in #build immediately, and never touch another agent's files
without an ack in #general.
```
