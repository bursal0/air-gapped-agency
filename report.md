# Air-Gapped Agency — Project Report

## Author

Ahmet Yusuf Bursal

## Design and protocol

The core uses C/POSIX and local files only. Run the shell from the repository
root. One orchestrator owns a workspace at a time. Agents are separate child
processes; each shared file has one writer. Published status, report, message
and agent output files use a temporary file followed by `rename()` in the same
directory. Logs are live stdout/stderr streams and may be read while growing.

Task IDs are exactly `task_` followed by three digits, for example `task_101`.
Task files use one `key: value` per line:

```text
id: task_101
agent: analyst
input: data/input.txt
output: data/analysis.txt
priority: 1
```

`id`, `agent`, and `input` are required. Output defaults to an empty argument;
priority defaults to zero and is stored but does not schedule work. Invalid,
duplicate or unknown fields are rejected. Task definitions are retained after
execution. A task ID already used by another definition or execution artifact
cannot be resubmitted. Create a new ID for another attempt.

The orchestrator writes `status/<id>.status`, containing `id`, `agent`, `pid`
and `state`. A normal exit produces `state: done`, `exit_code: N` and a
`report` path. Every normal exit, including 126 and 127, is DONE. Infrastructure
failures produce `state: error` and `message`. A parent-child startup pipe with
`FD_CLOEXEC` distinguishes actual exec failure from an agent returning 127.

Agents write `reports/<id>.report`; the orchestrator never writes these reports.
Logs are `logs/<id>.log`. Analyst reports contain agent, input, lines, words,
bytes and success information. Reporter reports contain task context and the
source report copied into a briefing. The optional output path receives the
same type of result through an atomic publish.

Messages are written only by `send`:

```text
from: analyst
to: reporter
task: task_101
message: please prepare the briefing
```

The filename is `messages/msg_001.analyst.reporter.txt`. The next sequence is
the maximum sequence on disk plus one, with at least three digits. Reporter
scans its mailbox, chooses the newest addressed valid message and reads
`reports/<message task>.report`. Without a message it uses its input argument.
Messages are retained; no consumed/read marker is maintained.

## Launch convention and process handling

`submit` constructs a NULL-terminated argv array:

```text
argv[0] = ./agents/<agent>
argv[1] = <task_id>
argv[2] = <input>
argv[3] = <output, or empty string>
argv[4] = NULL
```

The parser, duplicate check, executable access and input readability checks
run before fork. The child redirects stdout/stderr to its log and calls
`execvp`. Setup/exec failures are reported to the parent through the startup
pipe. The parent owns status publication and reaps children using `waitpid`.
`list` and `status` use `WNOHANG`; `wait` and `collect` block as necessary.
Exit, quit and EOF wait for remaining tracked children before shutting down.
Missing reports produce a clear diagnostic instead of a crash.

Analyst and reporter exit conventions: 0 success; 2 invalid arguments/task ID;
3 input or mailbox failure; 4 output failure; 5 report publication failure.
These nonzero codes still produce DONE because the program exited normally.

## Recovery and limitations

After restart, task definitions and status/report/message/log files remain
available. `tasks` scans task definitions; `status` and `collect` can read
recorded status. A recorded RUNNING PID is checked with `kill(pid, 0)`; if it
no longer exists its status is changed to ERROR because its exit result cannot
be reconstructed. A restarted orchestrator cannot `waitpid` an old child: it
is no longer that process's parent. PID reuse can make liveness ambiguous.
Successful atomic rename prevents partial visibility, but is not a durability
guarantee against power failure (no fsync protocol).

Only one orchestrator per workspace is supported. The shell tokenizes on
whitespace and does not implement shell quoting or paths containing spaces.
There are at most 64 in-memory tasks per session and three-digit task IDs.
Output paths must be chosen uniquely by the user to preserve single ownership.
The advisor prints suggestions only. Since reports have no read marker,
`collect` suggestions can repeat. Reporter deliberately retains messages and
can reuse the newest message. Graceful exit can wait indefinitely for a hung
agent; timeout/cancellation is outside the required scope. Automatic reaping
occurs on inspection commands; this is not a background SIGCHLD event loop.

## Reproducible demo

```text
make clean && make
./aiorch
suggest
submit tasks/task_101.task
tasks
collect task_101
suggest
send analyst reporter task_101 please prepare the briefing
submit tasks/task_102.task
collect task_102
suggest
exit
```

The analyst creates `reports/task_101.report` and `data/analysis.txt`.
The reporter logs receipt of the message, reads that analyst report and
creates `reports/task_102.report` and `data/briefing.txt`.
The briefing path is the task's explicit output; the reporter always also
publishes its own ID-derived report.

## AI disclosure

OpenAI ChatGPT/Codex was used throughout development for explanation,
pseudocode, debugging, implementation and test assistance. The student wrote
parts of the implementation with guided feedback.
