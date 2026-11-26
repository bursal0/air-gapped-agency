# Air-Gapped Agency

**Process orchestration and file-mailbox IPC for autonomous agents, written in C (POSIX).**

A small "agency" of offline agents that never touch the network. A shell-like
orchestrator (`aiorch`) launches each agent as a separate child process, tracks
its lifecycle, and lets agents talk to each other only through files on disk:
task definitions, status files, reports and a message mailbox.

## Highlights

- **fork / execvp process launching** with a fixed argv convention and per-task
  stdout/stderr log redirection
- **Startup pipe with `FD_CLOEXEC`** to tell a real `exec` failure apart from an
  agent that simply exits with 127
- **Atomic publishing** — every status, report and message is written to a temp
  file and `rename()`d, so readers never see a half-written file
- **Single-writer ownership** for every shared file
- **File mailbox** (`messages/msg_NNN.<from>.<to>.txt`) for analyst → reporter
  hand-off
- **Non-blocking reaping** (`waitpid(..., WNOHANG)`) for `list`/`status`,
  blocking for `wait`/`collect`
- **Crash recovery** — after a restart, recorded RUNNING tasks are checked with
  `kill(pid, 0)` and marked ERROR if the process is gone
- **Advisor** (`suggest`) that recommends the next sensible command
- Builds cleanly with `-Wall -Wextra -Werror`

## Architecture

```text
                +---------------- aiorch (orchestrator shell) ----------------+
 tasks/*.task ->| parse -> validate -> fork/exec -> waitpid -> status/*.status |
                +-------------------------------+-------------------------------+
                                                | argv: <task_id> <input> <output>
                    +---------------------------+---------------------------+
                    v                           v                           v
               agents/analyst              agents/reporter          sleeper / failer /
               (word/line/byte stats)      (reads mailbox, writes    wordcount (test agents)
                    |                       briefing)
                    v                           ^
             reports/task_101.report ----------+
                                     messages/msg_001.analyst.reporter.txt
```

## Build & run

```sh
make
./aiorch
```

```text
submit tasks/task_101.task
collect task_101
send analyst reporter task_101 please prepare the briefing
submit tasks/task_102.task
collect task_102
suggest
exit
```

The analyst produces `reports/task_101.report`; the reporter picks up the
message, reads that report and publishes `data/briefing.txt`.

## Project layout

```text
include/orchestrator.h   task struct and orchestrator API
src/main.c               shell loop and command parser
src/orchestrator.c       task lifecycle, status protocol, mailbox, recovery, advisor
agents/*.c               analyst, reporter and test agents
tasks/                   sample task definitions
report.md                full protocol, exit codes, recovery design and limitations
```

See [report.md](report.md) for the complete protocol specification and design
decisions.

## Context

Built as a project for BIL 322 System Programming at Istanbul Medeniyet
University. The shell skeleton came from the course; the orchestrator, agents,
protocol and recovery logic are my implementation.
