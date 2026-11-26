#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <signal.h>
#include "orchestrator.h"

static void refresh_task_nonblocking(Task *task);
static int task_id_in_use(Task *tasks, int count, const char *id,
                          const char *allowed_task_file);
static int refresh_recorded_state(const char *id,
                                  char *state, size_t state_size);


static char *trim_whitespace(char *text) {
    if (text == NULL) {
        return NULL;
    }

    while (isspace((unsigned char)*text)) {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    char *end = text + strlen(text) - 1;

    while (end > text && isspace((unsigned char)*end)) {
        end--;
    }

    end[1] = '\0';

    return text;
}

static int copy_task_field(char *destination, size_t destination_size,
                           const char *value, const char *field_name) {
    int length = snprintf(destination, destination_size, "%s", value);

    if (length < 0 || (size_t)length >= destination_size) {
        fprintf(stderr, "Task field '%s' is too long\n", field_name);
        return -1;
    }

    return 0;
}

static int valid_task_id(const char *id) {
    if (strlen(id) != 8 || strncmp(id, "task_", 5) != 0) {
        return 0;
    }

    for (int i = 5; i < 8; i++) {
        if (!isdigit((unsigned char)id[i])) {
            return 0;
        }
    }

    return 1;
}

static int valid_mailbox_name(const char *name) {
    if (name == NULL || *name == '\0') {
        return 0;
    }

    for (const unsigned char *p = (const unsigned char *)name;
         *p != '\0'; p++) {
        if (!isalnum(*p) && *p != '_' && *p != '-') {
            return 0;
        }
    }
    return 1;
}

static int next_message_sequence(void) {
    DIR *dir = opendir("messages");
    if (dir == NULL) {
        perror("open messages directory failed");
        return -1;
    }

    int maximum = 0;
    int failed = 0;
    while (1) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                perror("read messages directory failed");
                failed = 1;
            }
            break;
        }

        const char *name = entry->d_name;
        size_t length = strlen(name);
        if (strncmp(name, "msg_", 4) != 0 || length < 9 ||
            strcmp(name + length - 4, ".txt") != 0) {
            continue;
        }

        char *end = NULL;
        errno = 0;
        long sequence = strtol(name + 4, &end, 10);
        if (errno != 0 || end == name + 4 || *end != '.' ||
            sequence <= 0 || sequence > INT_MAX) {
            continue;
        }
        if (sequence > maximum) {
            maximum = (int)sequence;
        }
    }

    if (closedir(dir) == -1) {
        perror("close messages directory failed");
        failed = 1;
    }
    if (failed) {
        return -1;
    }
    if (maximum == INT_MAX) {
        fprintf(stderr, "Message sequence limit reached\n");
        return -1;
    }
    return maximum + 1;
}

int send_message(const char *from, const char *to, const char *task_id,
                 int message_count, char **message_parts) {
    if (!valid_mailbox_name(from) || !valid_mailbox_name(to)) {
        fprintf(stderr,
                "Invalid agent name: use letters, digits, '_' or '-'\n");
        return -1;
    }
    if (task_id == NULL || !valid_task_id(task_id)) {
        fprintf(stderr, "Invalid task id: expected task_007 format\n");
        return -1;
    }
    if (message_count <= 0 || message_parts == NULL) {
        fprintf(stderr, "Message cannot be empty\n");
        return -1;
    }

    char message[MAX_CMD_LEN] = "";
    size_t used = 0;
    for (int i = 0; i < message_count; i++) {
        int length = snprintf(message + used, sizeof(message) - used,
                              "%s%s", i == 0 ? "" : " ",
                              message_parts[i]);
        if (length < 0 || (size_t)length >= sizeof(message) - used) {
            fprintf(stderr, "Message is too long\n");
            return -1;
        }
        used += (size_t)length;
    }

    int sequence = next_message_sequence();
    if (sequence == -1) {
        return -1;
    }

    char final_path[MAX_PATH_LEN];
    char temp_path[MAX_PATH_LEN];
    int final_length = snprintf(final_path, sizeof(final_path),
                                "messages/msg_%03d.%s.%s.txt",
                                sequence, from, to);
    int temp_length = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                               final_path);
    if (final_length < 0 ||
        (size_t)final_length >= sizeof(final_path) ||
        temp_length < 0 || (size_t)temp_length >= sizeof(temp_path)) {
        fprintf(stderr, "Message path is too long\n");
        return -1;
    }

    FILE *f = fopen(temp_path, "w");
    if (f == NULL) {
        perror("open temporary message failed");
        return -1;
    }

    int written = fprintf(f,
                          "from: %s\n"
                          "to: %s\n"
                          "task: %s\n"
                          "message: %s\n",
                          from, to, task_id, message);
    int closed = fclose(f);
    if (written < 0 || closed == EOF) {
        fprintf(stderr, "write message failed\n");
        unlink(temp_path);
        return -1;
    }

    if (rename(temp_path, final_path) == -1) {
        perror("publish message failed");
        unlink(temp_path);
        return -1;
    }

    printf("Message written: %s\n", final_path);
    return 0;
}

static int reporter_message_exists(const char *task_id) {
    DIR *dir = opendir("messages");
    if (dir == NULL) {
        perror("open messages directory failed");
        return -1;
    }

    int found = 0;
    int failed = 0;
    while (!found) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                perror("read messages directory failed");
                failed = 1;
            }
            break;
        }

        size_t name_length = strlen(entry->d_name);
        if (name_length < 4 ||
            strcmp(entry->d_name + name_length - 4, ".txt") != 0) {
            continue;
        }

        char path[MAX_PATH_LEN];
        int path_length = snprintf(path, sizeof(path),
                                   "messages/%s", entry->d_name);
        if (path_length < 0 ||
            (size_t)path_length >= sizeof(path)) {
            fprintf(stderr, "Message path is too long\n");
            failed = 1;
            break;
        }

        FILE *f = fopen(path, "r");
        if (f == NULL) {
            perror("open message file failed");
            failed = 1;
            break;
        }

        int to_reporter = 0;
        int matching_task = 0;
        char line[1024];
        while (fgets(line, sizeof(line), f) != NULL) {
            char *text = trim_whitespace(line);
            if (strncmp(text, "to:", 3) == 0 &&
                strcmp(trim_whitespace(text + 3), "reporter") == 0) {
                to_reporter = 1;
            } else if (strncmp(text, "task:", 5) == 0 &&
                       strcmp(trim_whitespace(text + 5), task_id) == 0) {
                matching_task = 1;
            }
        }

        if (ferror(f) != 0) {
            perror("read message file failed");
            failed = 1;
        }
        if (fclose(f) == EOF) {
            perror("close message file failed");
            failed = 1;
        }
        if (failed) {
            break;
        }
        if (to_reporter && matching_task) {
            found = 1;
        }
    }

    if (closedir(dir) == -1) {
        perror("close messages directory failed");
        failed = 1;
    }
    return failed ? -1 : found;
}

static void choose_lower_id(TaskSpec *choice, int *has_choice,
                            const TaskSpec *candidate) {
    if (!*has_choice || strcmp(candidate->id, choice->id) < 0) {
        *choice = *candidate;
        *has_choice = 1;
    }
}

static void choose_higher_id(TaskSpec *choice, int *has_choice,
                             const TaskSpec *candidate) {
    if (!*has_choice || strcmp(candidate->id, choice->id) > 0) {
        *choice = *candidate;
        *has_choice = 1;
    }
}

int suggest_next_command(void) {
    DIR *dir = opendir("tasks");
    if (dir == NULL) {
        perror("open tasks directory failed");
        return -1;
    }

    TaskSpec not_started = {0};
    TaskSpec running = {0};
    TaskSpec error = {0};
    TaskSpec done = {0};
    TaskSpec analyst_done = {0};
    char not_started_path[MAX_PATH_LEN] = "";
    int has_not_started = 0;
    int has_running = 0;
    int has_error = 0;
    int has_done = 0;
    int has_analyst_done = 0;
    int task_file_count = 0;
    int failed = 0;

    while (1) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                perror("read tasks directory failed");
                failed = 1;
            }
            break;
        }

        const char *name = entry->d_name;
        size_t name_length = strlen(name);
        if (name_length < 5 ||
            strcmp(name + name_length - 5, ".task") != 0) {
            continue;
        }

        char path[MAX_PATH_LEN];
        int path_length = snprintf(path, sizeof(path), "tasks/%s", name);
        if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
            fprintf(stderr, "Task path is too long\n");
            failed = 1;
            break;
        }

        TaskSpec spec = {0};
        if (parse_task_file(path, &spec) == -1) {
            failed = 1;
            break;
        }
        task_file_count++;

        char state[32];
        if (refresh_recorded_state(spec.id, state, sizeof(state)) == -1) {
            failed = 1;
            break;
        }

        if (strcmp(state, "not_started") == 0) {
            if (!has_not_started || strcmp(spec.id, not_started.id) < 0) {
                not_started = spec;
                has_not_started = 1;
                if (copy_task_field(not_started_path,
                                    sizeof(not_started_path),
                                    path, "task path") == -1) {
                    failed = 1;
                    break;
                }
            }
        } else if (strcmp(state, "running") == 0) {
            choose_lower_id(&running, &has_running, &spec);
        } else if (strcmp(state, "error") == 0) {
            choose_lower_id(&error, &has_error, &spec);
        } else if (strcmp(state, "done") == 0) {
            choose_higher_id(&done, &has_done, &spec);
            if (strcmp(spec.agent, "analyst") == 0) {
                choose_higher_id(&analyst_done, &has_analyst_done,
                                 &spec);
            }
        }
    }

    if (closedir(dir) == -1) {
        perror("close tasks directory failed");
        failed = 1;
    }
    if (failed) {
        return -1;
    }

    if (task_file_count == 0) {
        printf("Suggestion: submit tasks/task_001.task\n");
        return 0;
    }
    if (has_error) {
        printf("Suggestion: log %s\n", error.id);
        return 0;
    }
    if (has_running) {
        printf("Suggestion: tasks, then status %s\n", running.id);
        return 0;
    }
    if (has_analyst_done) {
        char report_path[MAX_PATH_LEN];
        int length = snprintf(report_path, sizeof(report_path),
                              "reports/%s.report", analyst_done.id);
        if (length < 0 || (size_t)length >= sizeof(report_path)) {
            fprintf(stderr, "Report path is too long\n");
            return -1;
        }

        int report_exists = access(report_path, F_OK);
        if (report_exists == 0) {
            int message_exists = reporter_message_exists(analyst_done.id);
            if (message_exists == -1) {
                return -1;
            }
            if (!message_exists) {
                printf("Suggestion: send analyst reporter %s "
                       "please prepare the briefing\n",
                       analyst_done.id);
                return 0;
            }
        } else if (errno != ENOENT) {
            perror("check analyst report failed");
            return -1;
        }
    }
    if (has_not_started) {
        printf("Suggestion: submit %s\n", not_started_path);
        return 0;
    }
    if (has_done) {
        printf("Suggestion: collect %s\n", done.id);
        return 0;
    }

    printf("Suggestion: tasks\n");
    return 0;
}


static int read_task_state(const char *id, char *state, size_t state_size) {
    char status_path[MAX_PATH_LEN];
    int path_length = snprintf(status_path, sizeof(status_path),
                               "status/%s.status", id);

    if (path_length < 0 || (size_t)path_length >= sizeof(status_path)) {
        fprintf(stderr, "Could not build status path for task %s\n", id);
        return -1;
    }

    FILE *f = fopen(status_path, "r");

    if (f == NULL) {
        if (errno == ENOENT) {
            return copy_task_field(state, state_size, "not_started", "state");
        }

        perror("open status file failed");
        return -1;
    }

    char line[512];
    int found = 0;
    char *text;
    char *value = NULL;

    while (fgets(line, sizeof(line), f) != NULL) {
        text = trim_whitespace(line);
        if (strncmp(text, "state:", 6) == 0) {
            value = trim_whitespace(text + 6);
            found = 1;
            break;
        }
    }
    int read_failed = ferror(f);

    if (read_failed != 0) {
        perror("read status file failed");
    }
    if (fclose(f) == EOF) {
        perror("close status file failed");
        return -1;
    }

    if (read_failed != 0) {
        return -1;
    }

    if (!found) {
        fprintf(stderr, "Status file has no state field: %s\n", status_path);
        return -1;
    }

    if (strcmp(value, "running") != 0 &&
        strcmp(value, "done") != 0 &&
        strcmp(value, "error") != 0) {
        fprintf(stderr, "Invalid state '%s' in %s\n", value, status_path);
        return -1;
    }

    return copy_task_field(state, state_size, value, "state");
}










int list_workspace_tasks(Task *tasks, int count) {
    for (int i = 0; i < count; i++) {
        refresh_task_nonblocking(&tasks[i]);
    }

    DIR *dir = opendir("tasks");
    struct dirent *entry;

    if (dir == NULL) {
        perror("open tasks directory failed");
        return -1;
    }

    int result = 0;

    while (1) {
        errno = 0;
        entry = readdir(dir);

        if (entry == NULL) {
            if (errno != 0) {
                perror("read tasks directory failed");
                result = -1;
            }
            break;
        }

        const char *name = entry->d_name;
        size_t length = strlen(name);

        if (length >= 5 && strcmp(name + length - 5, ".task") == 0) {
            char task_path[MAX_PATH_LEN];
            int path_length = snprintf(task_path, sizeof(task_path),
                                       "tasks/%s", name);
            if (path_length < 0 || (size_t)path_length >= sizeof(task_path)) {
                fprintf(stderr, "Could not build task path for %s\n", name);
                result = -1;
                continue;
            }

            TaskSpec spec = {0};
            if (parse_task_file(task_path, &spec) == -1) {
                fprintf(stderr, "Skipping invalid task file: %s\n", task_path);
                result = -1;
                continue;
            }


            char state[32];
            if (refresh_recorded_state(spec.id, state,
                                       sizeof(state)) == -1) {
                result = -1;
                continue;
            }

            if (printf("%s %s %s\n", spec.id, spec.agent, state) < 0) {
                perror("print task information failed");
                result = -1;
                break;
            }

        }
    }

    if (closedir(dir) == -1) {
        perror("close tasks directory failed");
        result = -1;
    }

    return result;
}






int parse_task_file(const char *path, TaskSpec *spec) {
    if (path == NULL || spec == NULL) {
        fprintf(stderr, "parse_task_file received a NULL argument\n");
        return -1;
    }

    memset(spec, 0, sizeof(*spec));

    FILE *f = fopen(path, "r");

    if (f == NULL) {
        perror("open task file failed");
        return -1;
    }

    char line[512];
    int line_number = 0;
    int parse_failed = 0;
    int seen_id = 0;
    int seen_agent = 0;
    int seen_input = 0;
    int seen_output = 0;
    int seen_priority = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        line_number++;

        char *text = trim_whitespace(line);

        if (*text == '\0') {
            continue;
        }

        char *separator = strchr(text, ':');

        if (separator == NULL) {
            fprintf(stderr, "Invalid task line %d: missing ':'\n",
                    line_number);
            parse_failed = 1;
            break;
        }

        *separator = '\0';

        char *key = trim_whitespace(text);
        char *value = trim_whitespace(separator + 1);

        if (*key == '\0' || *value == '\0') {
            fprintf(stderr, "Invalid task line %d: empty key or value\n",
                    line_number);
            parse_failed = 1;
            break;
        }

        if (strcmp(key, "id") == 0) {
            if (seen_id || copy_task_field(spec->id, sizeof(spec->id),
                                           value, key) == -1) {
                fprintf(stderr, "Invalid or duplicate task field 'id'\n");
                parse_failed = 1;
                break;
            }
            seen_id = 1;
        } else if (strcmp(key, "agent") == 0) {
            if (seen_agent || copy_task_field(spec->agent,
                                              sizeof(spec->agent),
                                              value, key) == -1) {
                fprintf(stderr, "Invalid or duplicate task field 'agent'\n");
                parse_failed = 1;
                break;
            }
            seen_agent = 1;
        } else if (strcmp(key, "input") == 0) {
            if (seen_input || copy_task_field(spec->input,
                                              sizeof(spec->input),
                                              value, key) == -1) {
                fprintf(stderr, "Invalid or duplicate task field 'input'\n");
                parse_failed = 1;
                break;
            }
            seen_input = 1;
        } else if (strcmp(key, "output") == 0) {
            if (seen_output || copy_task_field(spec->output,
                                               sizeof(spec->output),
                                               value, key) == -1) {
                fprintf(stderr, "Invalid or duplicate task field 'output'\n");
                parse_failed = 1;
                break;
            }
            seen_output = 1;
        } else if (strcmp(key, "priority") == 0) {
            char *number_end;
            long priority;

            if (seen_priority) {
                fprintf(stderr, "Duplicate task field 'priority'\n");
                parse_failed = 1;
                break;
            }

            errno = 0;
            priority = strtol(value, &number_end, 10);

            if (errno == ERANGE || number_end == value ||
                *number_end != '\0' || priority < INT_MIN ||
                priority > INT_MAX) {
                fprintf(stderr, "Invalid priority on task line %d\n",
                        line_number);
                parse_failed = 1;
                break;
            }

            spec->priority = (int)priority;
            seen_priority = 1;
        } else {
            fprintf(stderr, "Unknown task field '%s' on line %d\n",
                    key, line_number);
            parse_failed = 1;
            break;
        }
    }

    if (!parse_failed && ferror(f)) {
        perror("read task file failed");
        parse_failed = 1;
    }

    if (fclose(f) == EOF) {
        perror("close task file failed");
        parse_failed = 1;
    }

    if (parse_failed) {
        return -1;
    }

    if (!seen_id || !seen_agent || !seen_input) {
        fprintf(stderr,
                "Invalid task file: required fields are id, agent, and input\n");
        return -1;
    }

    if (!valid_task_id(spec->id)) {
        fprintf(stderr,
                "Invalid task id '%s': expected format task_007\n",
                spec->id);
        return -1;
    }

    return 0;
}


static int launch_task(Task *tasks, int *count, const TaskSpec *task_spec,
                       char **argv);

int submit_task(Task *tasks, int *count, const char *task_file) {
    if (tasks == NULL || count == NULL || task_file == NULL) {
        fprintf(stderr, "submit_task received a NULL argument\n");
        return -1;
    }

    TaskSpec spec = {0};

    if (parse_task_file(task_file, &spec) == -1) {
        return -1;
    }

    int in_use = task_id_in_use(tasks, *count, spec.id, task_file);
    if (in_use == -1) {
        return -1;
    }
    if (in_use == 1) {
        fprintf(stderr, "Task %s already exists in the workspace\n",
                spec.id);
        return -1;
    }

    char agent_path[MAX_PATH_LEN];

    int length_agent = snprintf(agent_path, sizeof(agent_path),
                                "./agents/%s", spec.agent);

    if (length_agent < 0 || (size_t)length_agent >= sizeof(agent_path)) {
        fprintf(stderr, "agent path is too long for task %s\n", spec.id);
        return -1;
    }

    int agent_access_result = access(agent_path, X_OK);

    if (agent_access_result == -1) {
        perror("Agent executable is not accessible");
        return -1;
    }

    if (access(spec.input, R_OK) == -1) {
        perror("Input file is not readable");
        return -1;
    }

    char *agent_argv[5] = {
        agent_path, spec.id, spec.input, spec.output, NULL
    };

    return launch_task(tasks, count, &spec, agent_argv);
}

int task_add(Task *tasks, int *count, const TaskSpec *spec, pid_t pid,
             const char *command) {
    if (tasks == NULL || count == NULL || spec == NULL || command == NULL) {
        fprintf(stderr, "task_add received a NULL argument\n");
        return -1;
    }

    if (*count >= MAX_TASKS) {
        fprintf(stderr, "Too many tasks\n");
        return -1;
    }

    tasks[*count].spec = *spec;
    tasks[*count].pid = pid;
    tasks[*count].state = TASK_RUNNING;
    tasks[*count].exit_code = -1;

    if (copy_task_field(tasks[*count].command,
                        sizeof(tasks[*count].command),
                        command, "command") == -1) {
        return -1;
    }

    (*count)++;

    return 0;
}

Task *task_find(Task *tasks, int count, const char *id) {
    if (tasks == NULL || id == NULL) {
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        if (strcmp(tasks[i].spec.id, id) == 0) {
            return &tasks[i];
        }
    }

    return NULL;
}

static int write_status_file(const Task *task, const char *state,
                             int exit_code, const char *message) {
    if (task == NULL || state == NULL) {
        fprintf(stderr, "write_status_file received a NULL argument\n");
        return -1;
    }

    char final_path[256];
    char temp_path[256];

    int final_length = snprintf(final_path, sizeof(final_path),
                                "status/%s.status", task->spec.id);
    int temp_length = snprintf(temp_path, sizeof(temp_path),
                               "status/%s.status.tmp", task->spec.id);

    if (final_length < 0 || (size_t)final_length >= sizeof(final_path) ||
        temp_length < 0 || (size_t)temp_length >= sizeof(temp_path)) {
        fprintf(stderr, "Status path too long\n");
        return -1;
    }

    FILE *f = fopen(temp_path, "w");

    if (!f) {
        perror("fopen status file failed");
        return -1;
    }

    int written = fprintf(f,
                          "id: %s\nagent: %s\npid: %d\nstate: %s\n",
                          task->spec.id, task->spec.agent,
                          (int)task->pid, state);

    if (written >= 0 && exit_code >= 0) {
        written = fprintf(f,
                          "exit_code: %d\nreport: reports/%s.report\n",
                          exit_code, task->spec.id);
    }

    if (written >= 0 && message != NULL) {
        written = fprintf(f, "message: %s\n", message);
    }

    int closed = fclose(f);

    if (written < 0 || closed == EOF) {
        fprintf(stderr, "Status write failed\n");
        unlink(temp_path);
        return -1;
    }

    if (rename(temp_path, final_path) == -1) {
        perror("rename status file failed");
        unlink(temp_path);
        return -1;
    }

    return 0;
}

/* 1: reserved, 0: free, -1: could not safely check the workspace. */
static int task_id_in_use(Task *tasks, int count, const char *id,
                          const char *allowed_task_file) {
    if (task_find(tasks, count, id) != NULL) {
        return 1;
    }

    struct stat allowed_info;
    int has_allowed_file = allowed_task_file != NULL;
    if (has_allowed_file && stat(allowed_task_file, &allowed_info) == -1) {
        perror("inspect submitted task file failed");
        return -1;
    }

    const char *formats[] = {
        "tasks/%s.task", "status/%s.status",
        "reports/%s.report", "logs/%s.log",
        "status/%s.status.tmp"
    };

    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        char path[MAX_PATH_LEN];
        int length = snprintf(path, sizeof(path), formats[i], id);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            fprintf(stderr, "Task path is too long for %s\n", id);
            return -1;
        }

        struct stat info;
        if (lstat(path, &info) == 0) {
            if (has_allowed_file && i == 0) {
                struct stat followed_info;
                if (stat(path, &followed_info) == -1) {
                    perror("inspect workspace task file failed");
                    return -1;
                }
                if (followed_info.st_dev == allowed_info.st_dev &&
                    followed_info.st_ino == allowed_info.st_ino) {
                    continue;
                }
            }
            return 1;
        }
        if (errno != ENOENT) {
            perror("check task path failed");
            return -1;
        }
    }

    DIR *dir = opendir("tasks");
    if (dir == NULL) {
        perror("open tasks directory failed");
        return -1;
    }

    int result = 0;
    while (1) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                perror("read tasks directory failed");
                result = -1;
            }
            break;
        }

        size_t length = strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name + length - 5, ".task") != 0) {
            continue;
        }

        char path[MAX_PATH_LEN];
        int written = snprintf(path, sizeof(path), "tasks/%s", entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            fprintf(stderr, "Task filename is too long\n");
            result = -1;
            break;
        }

        if (has_allowed_file) {
            struct stat entry_info;
            if (stat(path, &entry_info) == -1) {
                perror("inspect workspace task file failed");
                result = -1;
                break;
            }
            if (entry_info.st_dev == allowed_info.st_dev &&
                entry_info.st_ino == allowed_info.st_ino) {
                continue;
            }
        }

        TaskSpec spec = {0};
        if (parse_task_file(path, &spec) == -1) {
            fprintf(stderr, "Cannot safely check task IDs in %s\n", path);
            result = -1;
            break;
        }
        if (strcmp(spec.id, id) == 0) {
            result = 1;
            break;
        }
    }

    if (closedir(dir) == -1) {
        perror("close tasks directory failed");
        return -1;
    }
    return result;
}

int run_command(Task *tasks, int *count, char **argv) {
    if (tasks == NULL || count == NULL || argv == NULL || argv[0] == NULL) {
        fprintf(stderr, "run_command received a NULL argument\n");
        return -1;
    }

    if (*count >= MAX_TASKS) {
        fprintf(stderr, "Task table full (MAX_TASKS=%d)\n", MAX_TASKS);
        return -1;
    }

    TaskSpec spec = {0};
    int available = 0;
    for (int sequence = 1; sequence <= 999; sequence++) {
        int length = snprintf(spec.id, sizeof(spec.id), "task_%03d", sequence);
        if (length < 0 || (size_t)length >= sizeof(spec.id)) {
            fprintf(stderr, "Task id is too long\n");
            return -1;
        }

        int in_use = task_id_in_use(tasks, *count, spec.id, NULL);
        if (in_use == -1) {
            return -1;
        }
        if (in_use == 0) {
            available = 1;
            break;
        }
    }
    if (!available) {
        fprintf(stderr, "No free task ID between task_001 and task_999\n");
        return -1;
    }

    const char *last_slash = strrchr(argv[0], '/');

    const char *agent_name;

    if (last_slash != NULL) {
        agent_name = last_slash + 1;
    } else {
        agent_name = argv[0];
    }

    if (copy_task_field(spec.agent, sizeof(spec.agent),
                        agent_name, "agent") == -1) {
        return -1;
    }

    return launch_task(tasks, count, &spec, argv);
}

static void report_child_start_failure(int fd, char code) {
    ssize_t written;
    do {
        written = write(fd, &code, 1);
    } while (written == -1 && errno == EINTR);
}

static int launch_task(Task *tasks, int *count, const TaskSpec *task_spec,
                       char **argv) {
    if (*count >= MAX_TASKS) {
        fprintf(stderr, "Task table full (MAX_TASKS=%d)\n", MAX_TASKS);
        return -1;
    }

    TaskSpec spec = *task_spec;

    char logpath[256];
    int log_length = snprintf(logpath, sizeof(logpath),
                              "logs/%s.log", spec.id);

    if (log_length < 0 || (size_t)log_length >= sizeof(logpath)) {
        fprintf(stderr, "Log path is too long\n");
        return -1;
    }

    char command[MAX_CMD_LEN] = "";
    size_t used = 0;

    for (int i = 0; argv[i] != NULL; i++) {
        int length = snprintf(command + used, sizeof(command) - used,
                              "%s%s", i == 0 ? "" : " ", argv[i]);

        if (length < 0 || (size_t)length >= sizeof(command) - used) {
            fprintf(stderr, "Command is too long\n");
            return -1;
        }

        used += (size_t)length;
    }

    int startup_pipe[2];
    if (pipe(startup_pipe) == -1) {
        perror("create startup pipe failed");
        return -1;
    }

    int descriptor_flags = fcntl(startup_pipe[1], F_GETFD);
    if (descriptor_flags == -1 ||
        fcntl(startup_pipe[1], F_SETFD,
              descriptor_flags | FD_CLOEXEC) == -1) {
        perror("configure startup pipe failed");
        close(startup_pipe[0]);
        close(startup_pipe[1]);
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork failed");
        close(startup_pipe[0]);
        close(startup_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child process: the agent */

        close(startup_pipe[0]);

        int log_fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (log_fd < 0) {
            perror("open log file failed");
            report_child_start_failure(startup_pipe[1], 'S');
            _exit(126);
        }

        if (dup2(log_fd, STDOUT_FILENO) == -1) {
            perror("dup2 stdout failed");
            close(log_fd);
            report_child_start_failure(startup_pipe[1], 'S');
            _exit(126);
        }

        if (dup2(log_fd, STDERR_FILENO) == -1) {
            perror("dup2 stderr failed");
            close(log_fd);
            report_child_start_failure(startup_pipe[1], 'S');
            _exit(126);
        }

        if (log_fd != STDOUT_FILENO && log_fd != STDERR_FILENO &&
            close(log_fd) == -1) {
            perror("close log file failed");
            report_child_start_failure(startup_pipe[1], 'S');
            _exit(126);
        }

        execvp(argv[0], argv);
        perror("execvp failed");
        report_child_start_failure(startup_pipe[1], 'E');
        _exit(127);
    }

    /* Parent process: the orchestrator */

    if (close(startup_pipe[1]) == -1) {
        perror("close startup pipe write end failed");
    }

    char startup_failure = '\0';
    ssize_t received;
    do {
        received = read(startup_pipe[0], &startup_failure, 1);
    } while (received == -1 && errno == EINTR);

    if (close(startup_pipe[0]) == -1) {
        perror("close startup pipe read end failed");
    }

    if (received == -1 || received == 1) {
        const char *message;
        if (received == -1) {
            perror("read startup pipe failed");
            message = "could not verify agent startup";
        } else if (startup_failure == 'E') {
            message = "exec failed";
        } else {
            message = "agent setup failed";
        }

        int child_status;
        pid_t waited;
        do {
            waited = waitpid(pid, &child_status, 0);
        } while (waited == -1 && errno == EINTR);
        if (waited == -1) {
            perror("wait for failed child failed");
        }

        if (task_add(tasks, count, &spec, pid, command) == -1) {
            return -1;
        }

        Task *failed_task = task_find(tasks, *count, spec.id);
        if (failed_task == NULL) {
            fprintf(stderr, "Internal inconsistency: task was not found\n");
            return -1;
        }

        failed_task->state = TASK_ERROR;
        failed_task->exit_code = -1;
        if (write_status_file(failed_task, "error", -1, message) == -1) {
            fprintf(stderr, "Could not write failed startup status for %s\n",
                    spec.id);
        }

        fprintf(stderr, "Task %s infrastructure error: %s\n",
                spec.id, message);
        return -1;
    }

    int success = task_add(tasks, count, &spec, pid, command);

    if (success == -1) {
        return -1;
    }

    Task *new_task = task_find(tasks, *count, spec.id);

    if (new_task == NULL) {
        fprintf(stderr, "Internal inconsistency: task was not found\n");
        return -1;
    }

    if (write_status_file(new_task, "running", -1, NULL) == -1) {
        fprintf(stderr, "Could not create status file for task %s\n", spec.id);
    }

    printf("Started task %s\n", spec.id);
    printf("PID: %d\n", pid);

    return 0;
}

static void record_task_result(Task *task, int status, int announce) {
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);

        task->exit_code = exit_code;
        task->state = TASK_DONE;
        if (write_status_file(task, "done", task->exit_code, NULL) == -1) {
            fprintf(stderr, "Could not update status file for task %s\n",
                    task->spec.id);
        }

        if (announce) {
            printf("Task %s finished with exit code %d\n",
                   task->spec.id,
                   task->exit_code);
        }
    } else if (WIFSIGNALED(status)) {
        int signal_number = WTERMSIG(status);
        char message[128];

        snprintf(message, sizeof(message),
                 "killed by signal %d", signal_number);

        task->state = TASK_ERROR;
        if (write_status_file(task, "error", -1, message) == -1) {
            fprintf(stderr, "Could not update status file for task %s\n",
                    task->spec.id);
        }

        if (announce) {
            printf("Task %s killed by signal %d\n",
                   task->spec.id,
                   signal_number);
        }
    } else {
        task->state = TASK_ERROR;
        if (write_status_file(task, "error", -1,
                              "terminated abnormally") == -1) {
            fprintf(stderr, "Could not update status file for task %s\n",
                    task->spec.id);
        }
        if (announce) {
            printf("Task %s terminated abnormally\n", task->spec.id);
        }
    }
}

static void refresh_task_nonblocking(Task *task) {
    if (task->state != TASK_RUNNING) {
        return;
    }

    int status;
    pid_t waited;

    do {
        waited = waitpid(task->pid, &status, WNOHANG);
    } while (waited < 0 && errno == EINTR);

    if (waited == 0) {
        return;
    }

    if (waited < 0) {
        int wait_error = errno;
        char message[256];

        perror("waitpid WNOHANG failed");

        snprintf(message, sizeof(message),
                 "waitpid WNOHANG failed: %s", strerror(wait_error));

        task->state = TASK_ERROR;
        if (write_status_file(task, "error", -1, message) == -1) {
            fprintf(stderr, "Could not update status file for task %s\n",
                    task->spec.id);
        }
        return;
    }

    record_task_result(task, status, 0);
}

int wait_for_task(Task *tasks, int count, const char *id) {
    Task *task = task_find(tasks, count, id);

    if (!task) {
        fprintf(stderr, "Task %s not found\n", id);
        return -1;
    }

    if (task->state != TASK_RUNNING) {
        printf("Task %s already finished.\n", id);
        return 0;
    }

    int status;
    pid_t waited;

    do {
        waited = waitpid(task->pid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited < 0) {
        int wait_error = errno;
        char message[256];
        perror("waitpid failed");

        snprintf(message, sizeof(message),
                 "waitpid failed: %s", strerror(wait_error));

        task->state = TASK_ERROR;
        if (write_status_file(task, "error", -1, message) == -1) {
            fprintf(stderr, "Could not update status file for task %s\n",
                    task->spec.id);
        }
        return -1;
    }

    record_task_result(task, status, 1);

    return 0;
}

int wait_for_all_tasks(Task *tasks, int count) {
    int result = 0;

    for (int i = 0; i < count; i++) {
        if (tasks[i].state != TASK_RUNNING) {
            continue;
        }

        printf("Waiting for task %s before exit...\n", tasks[i].spec.id);

        if (wait_for_task(tasks, count, tasks[i].spec.id) == -1) {
            result = -1;
        }
    }

    return result;
}

int collect_task(Task *tasks, int count, const char *id) {
    if (id == NULL || !valid_task_id(id)) {
        fprintf(stderr, "Invalid task id: expected task_007 format\n");
        return -1;
    }

    Task *task = task_find(tasks, count, id);

    if (task != NULL) {
        if (wait_for_task(tasks, count, id) == -1) {
            return -1;
        }
        print_task_report(id);

        return 0;
    }

    char state[32];
    if (refresh_recorded_state(id, state, sizeof(state)) == -1) {
        return -1;
    }

    if (strcmp(state, "done") == 0 || strcmp(state, "error") == 0) {
        if (strcmp(state, "error") == 0) {
            fprintf(stderr, "Task %s has a recorded infrastructure error\n", id);
        }
        print_task_report(id);
        return 0;
    }

    if (strcmp(state, "running") == 0) {
        fprintf(stderr,
                "Task %s is not tracked in this session; "
                "its recorded running state cannot be verified\n", id);
        return -1;
    }

    fprintf(stderr, "No status record found for task %s\n", id);
    return -1;
}






void list_tasks(Task *tasks, int count) {
    printf("%-12s %-12s %-10s %-10s %s\n",
           "ID", "AGENT", "PID", "STATE", "COMMAND");

    for (int i = 0; i < count; i++) {
        refresh_task_nonblocking(&tasks[i]);

        const char *state = "UNKNOWN";

        if (tasks[i].state == TASK_RUNNING) {
            state = "RUNNING";
        } else if (tasks[i].state == TASK_DONE) {
            state = "DONE";
        } else if (tasks[i].state == TASK_ERROR) {
            state = "ERROR";
        }

        printf("%-12s %-12s %-10d %-10s %s\n",
               tasks[i].spec.id,
               tasks[i].spec.agent,
               (int)tasks[i].pid,
               state,
               tasks[i].command);
    }
}

static int read_recorded_process(const char *expected_id,
                                 char *agent, size_t agent_size,
                                 pid_t *pid) {
    char path[MAX_PATH_LEN];
    int length = snprintf(path, sizeof(path),
                          "status/%s.status", expected_id);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        fprintf(stderr, "Status path too long for task %s\n", expected_id);
        return -1;
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror("open status file failed");
        return -1;
    }

    char line[512];
    char recorded_id[MAX_TASK_ID_LEN] = "";
    int seen_id = 0;
    int seen_agent = 0;
    int seen_pid = 0;
    int failed = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        char *text = trim_whitespace(line);

        if (strncmp(text, "id:", 3) == 0) {
            if (seen_id ||
                copy_task_field(recorded_id, sizeof(recorded_id),
                                trim_whitespace(text + 3), "id") == -1) {
                failed = 1;
                break;
            }
            seen_id = 1;
        } else if (strncmp(text, "agent:", 6) == 0) {
            if (seen_agent ||
                copy_task_field(agent, agent_size,
                                trim_whitespace(text + 6), "agent") == -1) {
                failed = 1;
                break;
            }
            seen_agent = 1;
        } else if (strncmp(text, "pid:", 4) == 0) {
            if (seen_pid) {
                failed = 1;
                break;
            }

            char *value = trim_whitespace(text + 4);
            char *end = NULL;
            errno = 0;
            long parsed_pid = strtol(value, &end, 10);
            if (errno != 0 || end == value || *end != '\0' ||
                parsed_pid <= 0 || parsed_pid > INT_MAX) {
                fprintf(stderr, "Invalid pid in %s\n", path);
                failed = 1;
                break;
            }

            *pid = (pid_t)parsed_pid;
            seen_pid = 1;
        }
    }

    if (ferror(f) != 0) {
        perror("read status file failed");
        failed = 1;
    }
    if (fclose(f) == EOF) {
        perror("close status file failed");
        failed = 1;
    }

    if (failed) {
        return -1;
    }
    if (!seen_id || !seen_agent || !seen_pid) {
        fprintf(stderr, "Status file is missing id, agent, or pid: %s\n",
                path);
        return -1;
    }
    if (strcmp(recorded_id, expected_id) != 0) {
        fprintf(stderr, "Status id '%s' does not match requested id '%s'\n",
                recorded_id, expected_id);
        return -1;
    }

    return 0;
}

static int refresh_recorded_state(const char *id,
                                  char *state, size_t state_size) {
    if (read_task_state(id, state, state_size) == -1) {
        return -1;
    }

    if (strcmp(state, "running") != 0) {
        return 0;
    }

    char agent[MAX_AGENT_LEN] = "";
    pid_t pid = -1;
    if (read_recorded_process(id, agent, sizeof(agent), &pid) == -1) {
        return -1;
    }

    if (kill(pid, 0) == 0 || errno == EPERM) {
        return 0;
    }
    if (errno != ESRCH) {
        perror("check recorded process failed");
        return -1;
    }

    Task recovered_task = {0};
    if (copy_task_field(recovered_task.spec.id,
                        sizeof(recovered_task.spec.id),
                        id, "id") == -1 ||
        copy_task_field(recovered_task.spec.agent,
                        sizeof(recovered_task.spec.agent),
                        agent, "agent") == -1) {
        return -1;
    }
    recovered_task.pid = pid;

    if (write_status_file(
            &recovered_task, "error", -1,
            "recorded running process is no longer alive after restart") == -1) {
        return -1;
    }

    return copy_task_field(state, state_size, "error", "state");
}

void print_task_status(Task *tasks, int count, const char *id) {
    if (id == NULL || !valid_task_id(id)) {
        fprintf(stderr, "Invalid task id: expected task_007 format\n");
        return;
    }

    Task *task = task_find(tasks, count, id);

    if (task == NULL) {
        char recorded_state[32];
        if (refresh_recorded_state(id, recorded_state,
                                   sizeof(recorded_state)) == -1) {
            return;
        }
        if (strcmp(recorded_state, "not_started") == 0) {
            fprintf(stderr,
                    "Task %s not found in memory or workspace\n", id);
            return;
        }

        char path[MAX_PATH_LEN];
        int length = snprintf(path, sizeof(path),
                              "status/%s.status", id);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            fprintf(stderr, "Status path too long for task %s\n", id);
            return;
        }

        FILE *f = fopen(path, "r");
        if (f == NULL) {
            if (errno == ENOENT) {
                fprintf(stderr,
                        "Task %s not found in memory or workspace\n", id);
            } else {
                perror("open status file failed");
            }
            return;
        }

        printf("Task %s is not tracked in this session; "
               "showing recorded status:\n", id);

        char line[512];
        while (fgets(line, sizeof(line), f) != NULL) {
            if (fputs(line, stdout) == EOF) {
                perror("print status file failed");
                break;
            }
        }

        int read_failed = ferror(f);
        if (read_failed != 0) {
            perror("read status file failed");
        }
        if (fclose(f) == EOF) {
            perror("close status file failed");
        }
        return;
    }

    refresh_task_nonblocking(task);

    printf("Task ID: %s\n", task->spec.id);
    printf("Agent: %s\n", task->spec.agent);
    printf("PID: %d\n", (int)task->pid);
    printf("Command: %s\n", task->command);

    if (task->state == TASK_RUNNING) {
        printf("State: RUNNING\n");
    } else if (task->state == TASK_DONE) {
        printf("State: DONE\n");
        printf("Exit code: %d\n", task->exit_code);
    } else {
        printf("State: ERROR\n");
    }
}

void print_task_report(const char *id) {
    if (!valid_task_id(id)) {
        fprintf(stderr, "Invalid task id '%s'\n", id);
        return;
    }

    char path[256];

    int length = snprintf(path, sizeof(path), "reports/%s.report", id);

    if (length < 0 || (size_t)length >= sizeof(path)) {
        fprintf(stderr, "Report path too long for task %s\n", id);
        return;
    }

    FILE *f = fopen(path, "r");

    if (!f) {
        if (errno == ENOENT) {
            fprintf(stderr, "No report produced for task %s\n", id);
        } else {
            perror("open report failed");
        }
        return;
    }

    char line[512];

    while (fgets(line, sizeof(line), f)) {
        printf("%s", line);
    }

    fclose(f);
}

void print_task_log(const char *id) {
    if (!valid_task_id(id)) {
        fprintf(stderr, "Invalid task id '%s'\n", id);
        return;
    }

    char path[256];

    int length = snprintf(path, sizeof(path), "logs/%s.log", id);

    if (length < 0 || (size_t)length >= sizeof(path)) {
        fprintf(stderr, "Log path too long for task %s\n", id);
        return;
    }

    FILE *f = fopen(path, "r");

    if (!f) {
        fprintf(stderr, "No log available for task %s\n", id);
        return;
    }

    char line[512];

    while (fgets(line, sizeof(line), f)) {
        printf("%s", line);
    }

    fclose(f);
}
