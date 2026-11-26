#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PATH_SIZE 256

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

static int publish_analysis(const char *path, const char *task_id,
                            const char *input, size_t lines,
                            size_t words, size_t bytes) {
    char temp_path[PATH_SIZE];
    int length = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (length < 0 || (size_t)length >= sizeof(temp_path)) {
        fprintf(stderr, "Analysis output path is too long: %s\n", path);
        return -1;
    }

    FILE *f = fopen(temp_path, "w");
    if (f == NULL) {
        perror("open temporary analysis file failed");
        return -1;
    }

    int written = fprintf(f,
                          "agent: analyst\n"
                          "task: %s\n"
                          "input: %s\n"
                          "lines: %zu\n"
                          "words: %zu\n"
                          "bytes: %zu\n"
                          "status: success\n",
                          task_id, input, lines, words, bytes);

    int closed = fclose(f);
    if (written < 0 || closed == EOF) {
        fprintf(stderr, "write analysis file failed: %s\n", temp_path);
        unlink(temp_path);
        return -1;
    }

    if (rename(temp_path, path) == -1) {
        perror("publish analysis file failed");
        unlink(temp_path);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr,
                "Usage: analyst <task_id> <input> <output>\n");
        return 2;
    }

    const char *task_id = argv[1];
    const char *input = argv[2];
    const char *output = argv[3];

    if (!valid_task_id(task_id)) {
        fprintf(stderr, "Invalid task id: expected task_007 format\n");
        return 2;
    }

    FILE *f = fopen(input, "r");
    if (f == NULL) {
        perror("open analyst input failed");
        return 3;
    }

    size_t lines = 0;
    size_t words = 0;
    size_t bytes = 0;
    int in_word = 0;
    int character;
    int last_character = '\n';

    while ((character = fgetc(f)) != EOF) {
        bytes++;
        last_character = character;

        if (character == '\n') {
            lines++;
        }
        if (isspace((unsigned char)character)) {
            in_word = 0;
        } else if (!in_word) {
            words++;
            in_word = 1;
        }
    }

    if (ferror(f) != 0) {
        perror("read analyst input failed");
        fclose(f);
        return 3;
    }
    if (fclose(f) == EOF) {
        perror("close analyst input failed");
        return 3;
    }
    if (bytes > 0 && last_character != '\n') {
        lines++;
    }

    if (output[0] != '\0' &&
        publish_analysis(output, task_id, input,
                         lines, words, bytes) == -1) {
        return 4;
    }

    char report_path[PATH_SIZE];
    int length = snprintf(report_path, sizeof(report_path),
                          "reports/%s.report", task_id);
    if (length < 0 || (size_t)length >= sizeof(report_path)) {
        fprintf(stderr, "Report path is too long\n");
        return 5;
    }

    if (publish_analysis(report_path, task_id, input,
                         lines, words, bytes) == -1) {
        return 5;
    }

    printf("Analyst completed %s: %zu lines, %zu words, %zu bytes\n",
           task_id, lines, words, bytes);
    return 0;
}
