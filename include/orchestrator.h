#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include <sys/types.h>

#define MAX_TASKS 64
#define MAX_CMD_LEN 512
#define MAX_TASK_ID_LEN 32
#define MAX_AGENT_LEN 64
#define MAX_PATH_LEN 256

typedef enum {
    TASK_EMPTY,
    TASK_RUNNING,
    TASK_DONE,
    TASK_ERROR
} TaskState;

typedef struct {
    char id[MAX_TASK_ID_LEN];
    char agent[MAX_AGENT_LEN];
    char input[MAX_PATH_LEN];
    char output[MAX_PATH_LEN];
    int priority;
} TaskSpec;

typedef struct {
    TaskSpec spec;
    pid_t pid;
    TaskState state;
    int exit_code;
    char command[MAX_CMD_LEN];
} Task;

int task_add(Task *tasks, int *count, const TaskSpec *spec, pid_t pid,
             const char *command);
Task *task_find(Task *tasks, int count, const char *id);
int submit_task(Task *tasks, int *count, const char *task_file);
int collect_task(Task *tasks, int count, const char *id);
int list_workspace_tasks(Task *tasks, int count);
int run_command(Task *tasks, int *count, char **argv);
int send_message(const char *from, const char *to, const char *task_id,
                 int message_count, char **message_parts);
int suggest_next_command(void);
int parse_task_file(const char *path, TaskSpec *spec);
int wait_for_task(Task *tasks, int count, const char *id);
int wait_for_all_tasks(Task *tasks, int count);
void list_tasks(Task *tasks, int count);
void print_task_status(Task *tasks, int count, const char *id);
void print_task_report(const char *id);
void print_task_log(const char *id);

#endif
