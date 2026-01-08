#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_COMMAND_ARGS 64

typedef struct {
  char **command;
  int command_len;
  char *wait_text;
  int max_duration; // in seconds, 0 means no limit
} Spinner;

// Global state for signal handling
static volatile sig_atomic_t interrupted = 0;
static volatile sig_atomic_t signal_received = 0;
static pid_t child_pid = 0;

static void hide_cursor(void) {
  printf("\033[?25l");
  fflush(stdout);
}

static void show_cursor(void) {
  printf("\033[?25h");
  fflush(stdout);
}

static void sig_handler(int signum) {
  interrupted = 1;
  signal_received = signum;

  // Forward the signal to the child process
  if (child_pid > 0) {
    kill(child_pid, signum);
  }
}

static double get_monotonic_time(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_ms(int milliseconds) {
  struct timespec ts;
  ts.tv_sec = milliseconds / 1000;
  ts.tv_nsec = (milliseconds % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

int spinner_spin(Spinner *spinner) {
  const char *spinner_chars = "-\\|/";
  int spinner_len = 4;

  // Print wait text
  printf("%s ", spinner->wait_text);
  fflush(stdout);
  hide_cursor();

  // Fork the process
  child_pid = fork();

  if (child_pid == -1) {
    perror("fork failed");
    show_cursor();
    printf("\n");
    return 1;
  }

  if (child_pid == 0) {
    // Child process - execute the command
    execvp(spinner->command[0], spinner->command);
    // If execvp returns, it failed
    fprintf(stderr, "\nexec failed: %s\n", strerror(errno));
    _exit(127);
  }

  // Parent process
  double start = get_monotonic_time();
  int i = 0;
  int status;
  pid_t result;

  // Set up signal handlers
  struct sigaction sa;
  struct sigaction old_sigint, old_sigterm, old_sigquit;

  sa.sa_handler = sig_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sigaction(SIGINT, &sa, &old_sigint);
  sigaction(SIGTERM, &sa, &old_sigterm);
  sigaction(SIGQUIT, &sa, &old_sigquit);

  int exit_code = 0;

  while (1) {
    // Check for interruption first
    if (interrupted) {
      sleep_ms(100); // Give child a moment to handle signal

      // Wait for child to exit
      waitpid(child_pid, &status, 0);

      if (signal_received != 0) {
        const char *signal_name;
        switch (signal_received) {
        case SIGINT:
          signal_name = "SIGINT";
          break;
        case SIGTERM:
          signal_name = "SIGTERM";
          break;
        case SIGQUIT:
          signal_name = "SIGQUIT";
          break;
        default:
          signal_name = "signal";
          break;
        }

        if (signal_received == SIGINT || signal_received == SIGTERM ||
            signal_received == SIGQUIT) {
          fprintf(stderr, "\nInterrupted by %s.\n", signal_name);
        } else {
          fprintf(stderr, "\nInterrupted by signal %d.\n", signal_received);
        }

        exit_code = 128 + signal_received;
      } else {
        fprintf(stderr, "\nInterrupted.\n");
        exit_code = 130;
      }
      break;
    }

    // Check if child process has finished
    result = waitpid(child_pid, &status, WNOHANG);
    if (result != 0) {
      // Process finished normally
      if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
      } else {
        exit_code = 128;
      }
      break;
    }

    // Check for timeout
    if (spinner->max_duration > 0) {
      if (get_monotonic_time() - start >= spinner->max_duration) {
        fprintf(stderr, "\nProcess timed out.\n");
        kill(child_pid, SIGTERM);

        // Wait a bit for graceful shutdown
        sleep(1);

        result = waitpid(child_pid, &status, WNOHANG);
        if (result == 0) {
          // Still running, force kill
          kill(child_pid, SIGKILL);
          waitpid(child_pid, &status, 0);
        }

        exit_code = 124;
        break;
      }
    }

    // Update spinner animation
    printf("%c", spinner_chars[i % spinner_len]);
    fflush(stdout);
    sleep_ms(200);
    printf("\b");
    fflush(stdout);
    i++;
  }

  // Restore signal handlers and terminal state
  sigaction(SIGINT, &old_sigint, NULL);
  sigaction(SIGTERM, &old_sigterm, NULL);
  sigaction(SIGQUIT, &old_sigquit, NULL);

  show_cursor();
  printf("\n");

  return exit_code;
}

void spinner_free(Spinner *spinner) {
  if (spinner->wait_text) {
    free(spinner->wait_text);
  }
  if (spinner->command) {
    for (int i = 0; i < spinner->command_len; i++) {
      free(spinner->command[i]);
    }
    free(spinner->command);
  }
}

Spinner *spinner_create(char **command, int command_len, const char *wait_text,
                        int max_duration) {
  Spinner *spinner = malloc(sizeof(Spinner));
  if (!spinner)
    return NULL;

  // Copy command
  spinner->command = malloc(sizeof(char *) * (command_len + 1));
  spinner->command_len = command_len;
  for (int i = 0; i < command_len; i++) {
    spinner->command[i] = strdup(command[i]);
  }
  spinner->command[command_len] = NULL; // execvp needs NULL-terminated array

  // Set wait text
  if (wait_text) {
    spinner->wait_text = strdup(wait_text);
  } else {
    // Build default wait text
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "Please wait, running command:");
    for (int i = 0; i < command_len; i++) {
      strncat(buffer, " ", sizeof(buffer) - strlen(buffer) - 1);
      strncat(buffer, command[i], sizeof(buffer) - strlen(buffer) - 1);
    }
    spinner->wait_text = strdup(buffer);
  }

  spinner->max_duration = max_duration;

  return spinner;
}

int main(void) {
  char *command[] = {"sleep", "5"};
  Spinner *spinner = spinner_create(command, 2, "Hehe Waitin", 0);

  if (!spinner) {
    fprintf(stderr, "Failed to create spinner\n");
    return 1;
  }

  int exit_code = spinner_spin(spinner);
  printf("Exit code: %d\n", exit_code);

  spinner_free(spinner);
  return exit_code;
}
