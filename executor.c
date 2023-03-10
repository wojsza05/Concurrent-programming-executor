#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>

#include "err.h"
#include "utils.h"

#define MAX_LINE 1024
#define MAX_N_TASKS 4096
#define MAX_ENDED_TASK_LINE 64

// A structure that stores information about one task
struct Task {
    int pid;
    char last_stdout[MAX_LINE];
    char last_stderr[MAX_LINE];
    int pipe_dsc_out[2];
    int pipe_dsc_err[2];
} tasks[MAX_N_TASKS];

// A structure that stores information about one command
struct Program_info {
    int id;
    char *program_name;
    char **program_args;
};

// A structure that stores information about one listener
struct Listener_info {
    int pipe_dsc;
    char *result;
};

pthread_mutex_t mutex;
bool command_is_running = false;
char ended_tasks[MAX_N_TASKS * MAX_ENDED_TASK_LINE];
int ended_tasks_length = 0;

void *listener(void *arg) {
    int pipe_dsc = ((struct Listener_info *) arg)->pipe_dsc;
    char *result = ((struct Listener_info *) arg)->result;
    free(arg);

    char buffer[MAX_LINE];
    int n_read;
    char previous_buffer[2 * MAX_LINE];
    int last_newline = -1;
    int previous_buffer_size = 0;

    while ((n_read = read(pipe_dsc, buffer, MAX_LINE)) > 0) {
        for (int i = 0; i < n_read; ++i) {
            previous_buffer[previous_buffer_size++] = buffer[i];
            if (buffer[i] == '\n') {
                ASSERT_ZERO(pthread_mutex_lock(&mutex));
                for (int position = last_newline + 1;
                     position < previous_buffer_size; ++position)
                    result[position - last_newline - 1] =
                            previous_buffer[position];
                result[previous_buffer_size - last_newline - 2] = '\0';
                last_newline = previous_buffer_size - 1;
                ASSERT_ZERO(pthread_mutex_unlock(&mutex));
            }
        }

        if (last_newline != -1) {
            for (int i = last_newline + 1; i < previous_buffer_size; ++i)
                previous_buffer[i - last_newline - 1] = previous_buffer[i];
            previous_buffer_size -= last_newline + 1;
            previous_buffer[previous_buffer_size] = '\0';
            last_newline = -1;
        }
    }

    if (n_read == -1)
        syserr("read");

    if (n_read == 0) {
        if (previous_buffer_size != 0) {
            ASSERT_ZERO(pthread_mutex_lock(&mutex));
            for (int i = 0; i <= previous_buffer_size; ++i)
                result[i] = previous_buffer[i];
            ASSERT_ZERO(pthread_mutex_unlock(&mutex));
        }
    }
    return NULL;
}

// Function that runs listener threads
void *run_listeners(void *arg) {
    int id = *(int *) arg;
    free(arg);

    pthread_attr_t attr;
    ASSERT_ZERO(pthread_attr_init(&attr));
    ASSERT_ZERO(
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE));
    pthread_t out_thread, err_thread;

    struct Listener_info *out_info = malloc(sizeof(struct Listener_info));
    out_info->pipe_dsc = tasks[id].pipe_dsc_out[0];
    out_info->result = tasks[id].last_stdout;

    struct Listener_info *err_info = malloc(sizeof(struct Listener_info));
    err_info->pipe_dsc = tasks[id].pipe_dsc_err[0];
    err_info->result = tasks[id].last_stderr;

    ASSERT_ZERO(pthread_mutex_lock(&mutex));
    ASSERT_ZERO(pthread_create(&out_thread, &attr, listener, out_info));
    ASSERT_ZERO(pthread_create(&err_thread, &attr, listener, err_info));
    ASSERT_ZERO(pthread_mutex_unlock(&mutex));

    ASSERT_ZERO(pthread_join(out_thread, NULL));
    ASSERT_ZERO(pthread_join(err_thread, NULL));
    ASSERT_ZERO(pthread_attr_destroy(&attr));

    int status;
    ASSERT_SYS_OK(waitpid(tasks[id].pid, &status, 0));

    char ended_task_message[MAX_ENDED_TASK_LINE];

    ASSERT_ZERO(pthread_mutex_lock(&mutex));

    if (WIFEXITED(status))
        sprintf(ended_task_message, "Task %d ended: status %d.\n", id,
                WEXITSTATUS(status));
    else
        sprintf(ended_task_message, "Task %d ended: signalled.\n", id);

    if (command_is_running) {
        for (int i = 0; i < strlen(ended_task_message); ++i)
            ended_tasks[ended_tasks_length++] = ended_task_message[i];
        ended_tasks[ended_tasks_length] = '\0';
    }
    else
        printf("%s", ended_task_message);

    ASSERT_SYS_OK(close(tasks[id].pipe_dsc_out[0]));
    ASSERT_SYS_OK(close(tasks[id].pipe_dsc_err[0]));
    ASSERT_ZERO(pthread_mutex_unlock(&mutex));

    return NULL;
}

// Function that runs new process
void *run_process(void *arg) {
    int id = ((struct Program_info *) arg)->id;
    char *program_name = ((struct Program_info *) arg)->program_name;
    char **program_args = ((struct Program_info *) arg)->program_args;

    if (pipe(tasks[id].pipe_dsc_out) == -1 ||
        pipe(tasks[id].pipe_dsc_err) == -1)
        syserr("pipe");

    pid_t pid = fork();
    ASSERT_SYS_OK(pid);

    if (pid == 0) {
        // Child process
        ASSERT_SYS_OK(close(tasks[id].pipe_dsc_out[0]));
        ASSERT_SYS_OK(close(tasks[id].pipe_dsc_err[0]));
        ASSERT_SYS_OK(dup2(tasks[id].pipe_dsc_out[1], STDOUT_FILENO));
        ASSERT_SYS_OK(dup2(tasks[id].pipe_dsc_err[1], STDERR_FILENO));
        ASSERT_SYS_OK(close(tasks[id].pipe_dsc_out[1]));
        ASSERT_SYS_OK(close(tasks[id].pipe_dsc_err[1]));

        ASSERT_SYS_OK(execvp(program_name, program_args));
    } else {
        // Parent process
        ASSERT_SYS_OK(close(tasks[id].pipe_dsc_out[1]));
        ASSERT_SYS_OK(close(tasks[id].pipe_dsc_err[1]));

        set_close_on_exec(tasks[id].pipe_dsc_out[0], true);
        set_close_on_exec(tasks[id].pipe_dsc_err[0], true);

        tasks[id].pid = pid;
    }

    free(program_name);
    for (int i = 0; program_args[i] != NULL; ++i)
        free(program_args[i]);
    free(program_args);
    free(arg);
    return NULL;
}

int main() {
    int n_tasks = 0;
    char line[MAX_LINE];
    ended_tasks[0] = '\0';

    pthread_attr_t attr;
    ASSERT_ZERO(pthread_attr_init(&attr));
    ASSERT_ZERO(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE));
    pthread_t threads[MAX_N_TASKS];

    ASSERT_ZERO(pthread_mutex_init(&mutex, NULL));

    while (read_line(line, MAX_LINE, stdin)) {
        ASSERT_ZERO(pthread_mutex_lock(&mutex));
        command_is_running = true;
        ASSERT_ZERO(pthread_mutex_unlock(&mutex));

        // Remove trailing newline
        line[strlen(line) - 1] = '\0';

        // Split the line into words
        char **words = split_string(line);

        // Check if the line is a command
        if (strcmp(words[0], "run") == 0) {
            ASSERT_ZERO(pthread_mutex_lock(&mutex));

            struct Program_info *programArguments
                    = malloc(sizeof(struct Program_info));
            programArguments->id = n_tasks;
            programArguments->program_name =
                    malloc((strlen(words[1]) + 1) * sizeof(char));
            strcpy(programArguments->program_name, words[1]);

            int count_args = 0;
            while (words[count_args + 1] != NULL)
                ++count_args;
            programArguments->program_args =
                    malloc((count_args + 1) * sizeof(char *));
            for (int i = 0; i < count_args; ++i) {
                programArguments->program_args[i] =
                        malloc((strlen(words[i + 1]) + 1) * sizeof(char));
                strcpy(programArguments->program_args[i], words[i + 1]);
            }
            programArguments->program_args[count_args] = NULL;

            pthread_t run_process_thread;
            ASSERT_ZERO(pthread_create(&run_process_thread, &attr,
                                       run_process, programArguments));
            ASSERT_ZERO(pthread_join(run_process_thread, NULL));
            printf("Task %d started: pid %d.\n", n_tasks, tasks[n_tasks].pid);

            int *id = malloc(sizeof(int));
            *id = n_tasks;
            ASSERT_ZERO(pthread_create(&threads[n_tasks], &attr,
                                       run_listeners, id));
            ++n_tasks;
            ASSERT_ZERO(pthread_mutex_unlock(&mutex));
        } else if (strcmp(words[0], "out") == 0) {
            ASSERT_ZERO(pthread_mutex_lock(&mutex));
            int id = atoi(words[1]);
            if (id < 0 || id >= n_tasks)
                syserr("Invalid task id: %d", id);

            printf("Task %d stdout: '%s'.\n", id, tasks[id].last_stdout);
            ASSERT_ZERO(pthread_mutex_unlock(&mutex));
        } else if (strcmp(words[0], "err") == 0) {
            ASSERT_ZERO(pthread_mutex_lock(&mutex));
            int id = atoi(words[1]);
            if (id < 0 || id >= n_tasks)
                syserr("Invalid task id: %d", id);

            printf("Task %d stderr: '%s'.\n", id, tasks[id].last_stderr);
            ASSERT_ZERO(pthread_mutex_unlock(&mutex));
        } else if (strcmp(words[0], "kill") == 0) {
            ASSERT_ZERO(pthread_mutex_lock(&mutex));
            int id = atoi(words[1]);
            if (id < 0 || id >= n_tasks)
                syserr("Invalid task id: %d", id);

            kill(tasks[id].pid, SIGINT);
            ASSERT_ZERO(pthread_mutex_unlock(&mutex));
        } else if (strcmp(words[0], "sleep") == 0) {
            int time = atoi(words[1]);
            usleep(time * 1000);
        } else if (strcmp(words[0], "quit") == 0) {
            command_is_running = false;
            free_split_string(words);
            for (int i = 0; i < n_tasks; i++) {
                ASSERT_ZERO(pthread_mutex_lock(&mutex));
                kill(tasks[i].pid, SIGKILL);
                ASSERT_ZERO(pthread_mutex_unlock(&mutex));
                ASSERT_ZERO(pthread_join(threads[i], NULL));
            }
            ASSERT_ZERO(pthread_attr_destroy(&attr));
            ASSERT_ZERO(pthread_mutex_destroy(&mutex));
            exit(0);
        }

        // Free the memory allocated for the words
        free_split_string(words);

        ASSERT_ZERO(pthread_mutex_lock(&mutex));
        if (ended_tasks_length > 0) {
            printf("%s", ended_tasks);
            ended_tasks[0] = '\0';
            ended_tasks_length = 0;
        }

        command_is_running = false;
        ASSERT_ZERO(pthread_mutex_unlock(&mutex));
    }

    for (int i = 0; i < n_tasks; i++) {
        kill(tasks[i].pid, SIGKILL);
        ASSERT_ZERO(pthread_join(threads[i], NULL));
    }
    ASSERT_ZERO(pthread_attr_destroy(&attr));
    ASSERT_ZERO(pthread_mutex_destroy(&mutex));
    return 0;
}
