#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "orchestrator.h"

#define MAX_LINE 1024
#define MAX_ARGS 16

static int parse_line(char *line, char **argv, int max_args) {
    int argc = 0;
    char *saveptr = NULL;

    char *token = strtok_r(line, " \t\n", &saveptr);

    while (token != NULL && argc < max_args - 1) {
        argv[argc++] = token;
        token = strtok_r(NULL, " \t\n", &saveptr);
    }

    argv[argc] = NULL;
    return argc;
}

int main(void) {
    Task tasks[MAX_TASKS];
    int task_count = 0;

    memset(tasks, 0, sizeof(tasks));

    printf("AI Orchestrator Shell\n");
    printf("Type 'help' for commands.\n");

    while (1) {
        printf("aiorch> ");
        fflush(stdout);

        char line[MAX_LINE];

        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }

        char *argv[MAX_ARGS];
        int argc = parse_line(line, argv, MAX_ARGS);

        if (argc == 0) {
            continue;
        }

        if (strcmp(argv[0], "exit") == 0 ||
            strcmp(argv[0], "quit") == 0) {
            break;
        }

        if (strcmp(argv[0], "help") == 0) {
            printf("Commands:\n");
            printf("  run <program> [args...]\n");
            printf("  submit <task_file>\n");
            printf("  collect <task_id>\n");
            printf("  list\n");
            printf("  tasks\n");
            printf("  status <task_id>\n");
            printf("  wait <task_id>\n");
            printf("  report <task_id>\n");
            printf("  log <task_id>\n");
            printf("  send <from> <to> <task_id> <message>\n");
            printf("  suggest\n");
            printf("  exit\n");
            continue;
        }

        if (strcmp(argv[0], "run") == 0) {
            if (argc < 2) {
                fprintf(stderr, "Usage: run <program> [args...]\n");
                continue;
            }

            run_command(tasks, &task_count, &argv[1]);
            continue;
        }

        if (strcmp(argv[0], "submit") == 0) {
            if (argc != 2) {
                fprintf(stderr, "Usage: submit <task_file>\n");
                continue;
            }

            submit_task(tasks, &task_count, argv[1]);
            continue;
        }

        if (strcmp(argv[0], "collect") == 0) {
            if (argc != 2) {
                fprintf(stderr, "Usage: collect <task_id>\n");
                continue;
            }

            collect_task(tasks, task_count, argv[1]);
            continue;
        }

        if (strcmp(argv[0], "tasks") == 0) {
            if (argc != 1) {
                fprintf(stderr, "Usage: tasks\n");
                continue;
            }
            list_workspace_tasks(tasks, task_count);
            continue;
        }

        if (strcmp(argv[0], "list") == 0) {
            list_tasks(tasks, task_count);
            continue;
        }

        if (strcmp(argv[0], "status") == 0) {
            if (argc != 2) {
                fprintf(stderr, "Usage: status <task_id>\n");
                continue;
            }

            print_task_status(tasks, task_count, argv[1]);
            continue;
        }

        if (strcmp(argv[0], "wait") == 0) {
            if (argc < 2) {
                fprintf(stderr, "Usage: wait <task_id>\n");
                continue;
            }

            wait_for_task(tasks, task_count, argv[1]);
            continue;
        }

        if (strcmp(argv[0], "report") == 0) {
            if (argc < 2) {
                fprintf(stderr, "Usage: report <task_id>\n");
                continue;
            }

            print_task_report(argv[1]);
            continue;
        }

        if (strcmp(argv[0], "log") == 0) {
            if (argc < 2) {
                fprintf(stderr, "Usage: log <task_id>\n");
                continue;
            }

            print_task_log(argv[1]);
            continue;
        }

        if (strcmp(argv[0], "send") == 0) {
            if (argc < 5) {
                fprintf(stderr,
                        "Usage: send <from> <to> <task_id> <message>\n");
                continue;
            }

            send_message(argv[1], argv[2], argv[3],
                         argc - 4, &argv[4]);
            continue;
        }

        if (strcmp(argv[0], "suggest") == 0) {
            if (argc != 1) {
                fprintf(stderr, "Usage: suggest\n");
                continue;
            }

            suggest_next_command();
            continue;
        }

        fprintf(stderr, "Unknown command: %s\n", argv[0]);
    }

    if (wait_for_all_tasks(tasks, task_count) == -1) {
        fprintf(stderr, "One or more tasks could not be reaped cleanly.\n");
        return 1;
    }

    return 0;
}
