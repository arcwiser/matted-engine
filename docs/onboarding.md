# Agent onboarding

`AGENTS.md` is the project contract and always takes precedence over this quick
start. Complete every step before editing files.

## Ten-minute boot checklist

From the repository root (`C:\Users\Admin\Desktop\matted engine`):

1. Read `AGENTS.md` completely.
2. Join using the exact name assigned to you:

   ```bat
   python tools\mattedcomms\mattedcomms.py join --user AgentC
   ```

3. Start the mandatory watcher in a background shell and keep it alive. Pass
   `--no-cursor` so it never touches your read cursor (AGENTS.md rule 3) and
   you can still run ad-hoc `recv`:

   ```bat
   python tools\mattedcomms\mattedcomms.py watch --user AgentC --no-cursor
   ```

4. Check team health and available work:

   ```bat
   python tools\mattedcomms\mattedcomms.py status
   python tools\mattedcomms\mattedcomms.py task list
   ```

5. Claim an assigned task before working, always passing your user name:

   ```bat
   python tools\mattedcomms\mattedcomms.py task claim --id 8 --user AgentC
   ```

6. Announce the claim in `#tasks` and your immediate plan in `#general`.
7. Inspect the current files before editing; another agent may be mid-change.
8. Work only inside your ownership or explicit assignment.
9. Verify proportionally to the risk. Report a broken build in `#build`
   immediately; do not replace AgentA's build system.
10. Mark the task done and report the result, tests, and remaining caveats.

Replace `AgentC` and task `8` in the examples with your assigned identity and
task. Never reuse another agent's chat identity.

## Communication rules

- Reply within five minutes to a `#general` message beginning with your
  `@AgentName`, even if the response is only a short status update.
- Keep the watcher running throughout the session. It supplies presence,
  streams messages, and recovers the local service if needed.
- Do not run foreground `recv` for the same user while `watch` is active: both
  update that user's read cursor. Use cursor-safe `history`/`unread` queries or
  the watcher output when you need an additional view.
- Use `#rendering`, `#physics`, and `#build` for subject-specific coordination;
  use `#tasks` for claims and hand-offs.
- If no suitable task is unclaimed, follow the idle protocol below instead of
  idling. Never self-assign another agent's claimed task or files.
- Check messages every few minutes during long work, not only at the end.

## Idle protocol: never sit around

When `task list` shows nothing unclaimed that fits you, create your own task
(see the "Idle protocol" section of `AGENTS.md`, which takes precedence):

1. Invent the next most valuable task, consistent with the mission
   (performance-first rendering, large-scale destruction physics) and the
   code conventions.
2. Add it with the next free number, a priority and a measurable gate:

   ```bat
   python tools\mattedcomms\mattedcomms.py task add "TASK-<n> [P0|P1|P2][area] <what must be done>. Gate: <measurable done-check>"
   ```

   `P0` blocks shipping, `P1` is important, `P2` is polish. Prefer `P0` work.
3. Announce it in `#tasks`, claim it (`task claim --id N --user <Name>`), and
   start.
4. Never take a task another agent has claimed. If the new task would touch
   another agent's files, propose it in `#general` and wait for an ack first.

Useful commands:

```bat
python tools\mattedcomms\mattedcomms.py recv --user AgentC
python tools\mattedcomms\mattedcomms.py send --user AgentC --channel "#general" "status message"
python tools\mattedcomms\mattedcomms.py task done --id 8 --user AgentC
```

## Ownership and hand-offs

Read the live ownership table in `AGENTS.md`. For any file outside your area,
write a proposal in `#general` and wait for an explicit acknowledgement before
editing. New files are not automatically ownerless when they overlap another
agent's assigned feature.

Do not overwrite or roll back concurrent work. There may be no Git repository
metadata in a local snapshot, so coordination and careful file inspection are
essential.

## Build and validation

The intended build is MSVC 2022 + CMake/Ninja in Release mode using C++20,
`/O2`, `/fp:fast`, and `/EHsc`:

```bat
scripts\build.bat
build\demo.exe
build\bench.exe render
build\bench.exe physics
```

These targets are being implemented concurrently. If the script or target is
not yet present, report that fact rather than inventing a competing workflow.

## Copy-paste boot prompt

The human runs `scripts\onboard.cmd` (exists in `scripts\`) and pastes its
generated prompt — kept in `ONBOARD_PROMPT.txt` — into a new agent chat. The
new agent then self-serves per the checklist above; AgentC greets them per the
onboarding-officer duty below.

## AgentC onboarding-officer duty

While assigned this duty, AgentC watches for `[onboard] <name> joined` in
`#general`, greets the new agent, points them to `AGENTS.md` and the unclaimed
task list, and confirms their watcher is active. If `status` shows an agent dark
for more than five minutes, AgentC pings them in `#general`; continued silence
is escalated to AgentA for reassignment.
