#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PATH_SIZE 256
#define NAME_SIZE 64
#define MESSAGE_SIZE 512

typedef struct {
    int sequence;
    char from[NAME_SIZE];
    char to[NAME_SIZE];
    char task[32];
    char message[MESSAGE_SIZE];
} MailMessage;

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

static char *trim_whitespace(char *text) {
    while (isspace((unsigned char)*text)) {
        text++;
    }

    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return text;
}

static int copy_value(char *destination, size_t destination_size,
                      const char *value, const char *field) {
    int length = snprintf(destination, destination_size, "%s", value);
    if (length < 0 || (size_t)length >= destination_size) {
        fprintf(stderr, "Message field '%s' is too long\n", field);
        return -1;
    }
    return 0;
}

static int message_sequence(const char *name) {
    size_t length = strlen(name);
    if (strncmp(name, "msg_", 4) != 0 || length < 9 ||
        strcmp(name + length - 4, ".txt") != 0) {
        return 0;
    }

    char *end = NULL;
    errno = 0;
    long value = strtol(name + 4, &end, 10);
    if (errno != 0 || end == name + 4 || *end != '.' ||
        value <= 0 || value > INT_MAX) {
        return 0;
    }
    return (int)value;
}

static int read_message(const char *path, int sequence,
                        MailMessage *mail) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror("open reporter message failed");
        return -1;
    }

    memset(mail, 0, sizeof(*mail));
    mail->sequence = sequence;
    int seen_from = 0;
    int seen_to = 0;
    int seen_task = 0;
    int seen_message = 0;
    int failed = 0;
    char line[1024];

    while (fgets(line, sizeof(line), f) != NULL) {
        char *text = trim_whitespace(line);

        if (strncmp(text, "from:", 5) == 0) {
            if (seen_from ||
                copy_value(mail->from, sizeof(mail->from),
                           trim_whitespace(text + 5), "from") == -1) {
                failed = 1;
                break;
            }
            seen_from = 1;
        } else if (strncmp(text, "to:", 3) == 0) {
            if (seen_to ||
                copy_value(mail->to, sizeof(mail->to),
                           trim_whitespace(text + 3), "to") == -1) {
                failed = 1;
                break;
            }
            seen_to = 1;
        } else if (strncmp(text, "task:", 5) == 0) {
            if (seen_task ||
                copy_value(mail->task, sizeof(mail->task),
                           trim_whitespace(text + 5), "task") == -1) {
                failed = 1;
                break;
            }
            seen_task = 1;
        } else if (strncmp(text, "message:", 8) == 0) {
            if (seen_message ||
                copy_value(mail->message, sizeof(mail->message),
                           trim_whitespace(text + 8), "message") == -1) {
                failed = 1;
                break;
            }
            seen_message = 1;
        }
    }

    if (ferror(f) != 0) {
        perror("read reporter message failed");
        failed = 1;
    }
    if (fclose(f) == EOF) {
        perror("close reporter message failed");
        failed = 1;
    }

    if (failed) {
        return -1;
    }
    if (!seen_from || !seen_to || !seen_task || !seen_message ||
        !valid_task_id(mail->task)) {
        fprintf(stderr, "Invalid message file: %s\n", path);
        return -1;
    }
    return 0;
}

static int find_reporter_message(MailMessage *selected) {
    DIR *dir = opendir("messages");
    if (dir == NULL) {
        perror("open messages directory failed");
        return -1;
    }

    memset(selected, 0, sizeof(*selected));
    int failed = 0;
    const char *suffix = ".reporter.txt";
    size_t suffix_length = strlen(suffix);

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

        size_t name_length = strlen(entry->d_name);
        int sequence = message_sequence(entry->d_name);
        if (sequence == 0 || name_length < suffix_length ||
            strcmp(entry->d_name + name_length - suffix_length,
                   suffix) != 0) {
            continue;
        }

        char path[PATH_SIZE];
        int length = snprintf(path, sizeof(path),
                              "messages/%s", entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            fprintf(stderr, "Message path is too long\n");
            failed = 1;
            break;
        }

        MailMessage candidate;
        if (read_message(path, sequence, &candidate) == -1) {
            failed = 1;
            break;
        }
        if (strcmp(candidate.to, "reporter") == 0 &&
            candidate.sequence > selected->sequence) {
            *selected = candidate;
        }
    }

    if (closedir(dir) == -1) {
        perror("close messages directory failed");
        failed = 1;
    }
    if (failed) {
        return -1;
    }
    return selected->sequence == 0 ? 0 : 1;
}

static int publish_briefing(const char *path, const char *task_id,
                            const char *input) {
    FILE *source = fopen(input, "r");
    if (source == NULL) {
        perror("open reporter input failed");
        return -1;
    }

    char temp_path[PATH_SIZE];
    int length = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (length < 0 || (size_t)length >= sizeof(temp_path)) {
        fprintf(stderr, "Briefing output path is too long: %s\n", path);
        fclose(source);
        return -1;
    }

    FILE *destination = fopen(temp_path, "w");
    if (destination == NULL) {
        perror("open temporary briefing file failed");
        fclose(source);
        return -1;
    }

    int failed = 0;
    if (fprintf(destination,
                "agent: reporter\n"
                "task: %s\n"
                "source: %s\n"
                "status: success\n"
                "briefing:\n",
                task_id, input) < 0) {
        failed = 1;
    }

    char buffer[512];
    while (!failed) {
        size_t received = fread(buffer, 1, sizeof(buffer), source);
        if (received > 0 &&
            fwrite(buffer, 1, received, destination) != received) {
            failed = 1;
        }
        if (received < sizeof(buffer)) {
            if (ferror(source) != 0) {
                failed = 1;
            }
            break;
        }
    }

    if (fclose(source) == EOF) {
        failed = 1;
    }
    if (fclose(destination) == EOF) {
        failed = 1;
    }

    if (failed) {
        fprintf(stderr, "write briefing failed: %s\n", temp_path);
        unlink(temp_path);
        return -1;
    }

    if (rename(temp_path, path) == -1) {
        perror("publish briefing failed");
        unlink(temp_path);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr,
                "Usage: reporter <task_id> <input> <output>\n");
        return 2;
    }

    const char *task_id = argv[1];
    const char *input = argv[2];
    const char *output = argv[3];

    if (!valid_task_id(task_id)) {
        fprintf(stderr, "Invalid task id: expected task_007 format\n");
        return 2;
    }
    MailMessage mail;
    int mail_result = find_reporter_message(&mail);
    if (mail_result == -1) {
        return 3;
    }

    char mailbox_input[PATH_SIZE];
    if (mail_result == 1) {
        int length = snprintf(mailbox_input, sizeof(mailbox_input),
                              "reports/%s.report", mail.task);
        if (length < 0 || (size_t)length >= sizeof(mailbox_input)) {
            fprintf(stderr, "Mailbox report path is too long\n");
            return 3;
        }
        input = mailbox_input;
        printf("Reporter received message %03d from %s: %s\n",
               mail.sequence, mail.from, mail.message);
    }

    if (output[0] != '\0' && strcmp(input, output) == 0) {
        fprintf(stderr, "Reporter input and output must be different\n");
        return 2;
    }

    if (output[0] != '\0' &&
        publish_briefing(output, task_id, input) == -1) {
        return 4;
    }

    char report_path[PATH_SIZE];
    int length = snprintf(report_path, sizeof(report_path),
                          "reports/%s.report", task_id);
    if (length < 0 || (size_t)length >= sizeof(report_path)) {
        fprintf(stderr, "Report path is too long\n");
        return 5;
    }

    if (publish_briefing(report_path, task_id, input) == -1) {
        return 5;
    }

    printf("Reporter completed briefing for %s\n", task_id);
    return 0;
}
