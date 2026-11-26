# Build and demo

Run from the repository root with `make` then `./aiorch`.
The full protocol, launch convention, exit codes and recovery limitations
are documented in [report.md](report.md).

```text
suggest
submit tasks/task_101.task
tasks
collect task_101
suggest
send analyst reporter task_101 please prepare the briefing
submit tasks/task_102.task
collect task_102
log task_102
suggest
exit
```

Inspect `data/briefing.txt`, `reports/task_102.report` and the message file.
Restart and use `status task_101` to inspect persisted state.
Use a new task ID when rerunning without cleaning.

`make clean` deletes binaries and generated log/status/report/message files
and the two sample outputs. It preserves task definitions, `data/input.txt`
and all `.gitkeep` placeholders. Run it only when previous results are no
longer needed and no orchestrator is active.

