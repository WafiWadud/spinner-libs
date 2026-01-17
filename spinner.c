#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// ============================================================================
// Constants and Type Definitions
// ============================================================================

#define SPINNER_ANIMATION "-\\|/"
#define SPINNER_FRAME_MS 200
#define SIGTERM_GRACE_PERIOD_SEC 1
#define MAX_WAIT_TEXT_LEN 512

typedef enum {
  SPINNER_SUCCESS = 0,
  SPINNER_ERR_ALLOCATION = 1,
  SPINNER_ERR_FORK = 2,
  SPINNER_ERR_EXEC = 127,
  SPINNER_ERR_TIMEOUT = 124,
  SPINNER_ERR_INTERRUPTED = 130
} SpinnerError;

typedef struct {
  char **argv;          // Command arguments (NULL-terminated)
  size_t argc;          // Number of arguments
  char *message;        // Display message
  unsigned int timeout; // Timeout in seconds (0 = no timeout)
} SpinnerConfig;

// ============================================================================
// Global State (for signal handling)
// ============================================================================

static volatile sig_atomic_t g_interrupted = 0;
static volatile sig_atomic_t g_signal_number = 0;
static pid_t g_child_pid = 0;

// ============================================================================
// Terminal Control
// ============================================================================

static inline void terminal_hide_cursor(void) {
  fputs("\033[?25l", stdout);
  fflush(stdout);
}

static inline void terminal_show_cursor(void) {
  fputs("\033[?25h", stdout);
  fflush(stdout);
}

static inline void terminal_clear_line(void) { fputs("\r\033[K", stdout); }

// ============================================================================
// Time Utilities
// ============================================================================

static double time_monotonic_seconds(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0.0; // Fallback, though this should rarely fail
  }
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void time_sleep_ms(unsigned int milliseconds) {
  struct timespec ts = {.tv_sec = milliseconds / 1000,
                        .tv_nsec = (milliseconds % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

// ============================================================================
// Signal Handling
// ============================================================================

static void signal_handler(int signum) {
  g_interrupted = 1;
  g_signal_number = signum;

  if (g_child_pid > 0) {
    kill(g_child_pid, signum);
  }
}

typedef struct {
  struct sigaction sa_int;
  struct sigaction sa_term;
  struct sigaction sa_quit;
} SignalHandlerBackup;

static bool signal_setup_handlers(SignalHandlerBackup *backup) {
  struct sigaction sa = {.sa_handler = signal_handler, .sa_flags = 0};
  sigemptyset(&sa.sa_mask);

  if (sigaction(SIGINT, &sa, &backup->sa_int) != 0 ||
      sigaction(SIGTERM, &sa, &backup->sa_term) != 0 ||
      sigaction(SIGQUIT, &sa, &backup->sa_quit) != 0) {
    return false;
  }

  return true;
}

static void signal_restore_handlers(const SignalHandlerBackup *backup) {
  sigaction(SIGINT, &backup->sa_int, NULL);
  sigaction(SIGTERM, &backup->sa_term, NULL);
  sigaction(SIGQUIT, &backup->sa_quit, NULL);
}

static const char *signal_get_name(int signum) {
  switch (signum) {
  case SIGINT:
    return "SIGINT";
  case SIGTERM:
    return "SIGTERM";
  case SIGQUIT:
    return "SIGQUIT";
  default:
    return "unknown signal";
  }
}

// ============================================================================
// Process Management
// ============================================================================

static int process_wait_with_timeout(pid_t pid, unsigned int timeout_sec) {
  double start = time_monotonic_seconds();
  int status;

  while (true) {
    // Check if interrupted
    if (g_interrupted) {
      time_sleep_ms(100);
      waitpid(pid, &status, 0);
      fprintf(stderr, "\nInterrupted by %s\n",
              signal_get_name(g_signal_number));
      return 128 + g_signal_number;
    }

    // Check if process finished
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result > 0) {
      if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
      }
      return 128;
    } else if (result < 0) {
      perror("waitpid");
      return 1;
    }

    // Check for timeout
    if (timeout_sec > 0 && (time_monotonic_seconds() - start) >= timeout_sec) {
      fprintf(stderr, "\nProcess timed out after %u seconds\n", timeout_sec);

      // Graceful termination
      kill(pid, SIGTERM);
      sleep(SIGTERM_GRACE_PERIOD_SEC);

      // Force kill if still running
      if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
      }

      return SPINNER_ERR_TIMEOUT;
    }

    time_sleep_ms(50);
  }
}

static pid_t process_execute(char **argv) {
  pid_t pid = fork();

  if (pid == 0) {
    // Child process
    execvp(argv[0], argv);
    fprintf(stderr, "Failed to execute '%s': %s\n", argv[0], strerror(errno));
    _exit(SPINNER_ERR_EXEC);
  }

  return pid;
}

// ============================================================================
// Spinner Animation
// ============================================================================

typedef struct {
  const char *frames;
  size_t frame_count;
  size_t current_frame;
  const char *message;
} SpinnerAnimation;

static void spinner_init_animation(SpinnerAnimation *anim,
                                   const char *message) {
  anim->frames = SPINNER_ANIMATION;
  anim->frame_count = strlen(SPINNER_ANIMATION);
  anim->current_frame = 0;
  anim->message = message;
}

static void spinner_render_frame(SpinnerAnimation *anim) {
  printf("\r%s %c", anim->message, anim->frames[anim->current_frame]);
  fflush(stdout);
  anim->current_frame = (anim->current_frame + 1) % anim->frame_count;
}

static int spinner_run_with_animation(pid_t pid, const char *message,
                                      unsigned int timeout) {
  SpinnerAnimation anim;
  spinner_init_animation(&anim, message);

  terminal_hide_cursor();

  double start = time_monotonic_seconds();
  int status;

  while (true) {
    // Render current frame
    spinner_render_frame(&anim);
    time_sleep_ms(SPINNER_FRAME_MS);

    // Check if interrupted
    if (g_interrupted) {
      terminal_clear_line();
      terminal_show_cursor();

      time_sleep_ms(100);
      waitpid(pid, &status, 0);

      fprintf(stderr, "Interrupted by %s\n", signal_get_name(g_signal_number));
      return 128 + g_signal_number;
    }

    // Check if process finished
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result > 0) {
      terminal_clear_line();
      terminal_show_cursor();

      if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
      }
      return 128;
    } else if (result < 0) {
      terminal_clear_line();
      terminal_show_cursor();
      perror("waitpid");
      return 1;
    }

    // Check for timeout
    if (timeout > 0 && (time_monotonic_seconds() - start) >= timeout) {
      terminal_clear_line();
      terminal_show_cursor();

      fprintf(stderr, "Process timed out after %u seconds\n", timeout);
      kill(pid, SIGTERM);
      sleep(SIGTERM_GRACE_PERIOD_SEC);

      if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
      }

      return SPINNER_ERR_TIMEOUT;
    }
  }
}

// ============================================================================
// Configuration Management
// ============================================================================

static char *config_build_default_message(char **argv, size_t argc) {
  char buffer[MAX_WAIT_TEXT_LEN];
  int offset = snprintf(buffer, sizeof(buffer), "Running:");

  for (size_t i = 0; i < argc && offset < (int)sizeof(buffer) - 1; i++) {
    offset +=
        snprintf(buffer + offset, sizeof(buffer) - offset, " %s", argv[i]);
  }

  return strdup(buffer);
}

SpinnerConfig *spinner_config_create(char **argv, size_t argc,
                                     const char *message,
                                     unsigned int timeout) {
  if (!argv || argc == 0) {
    return NULL;
  }

  SpinnerConfig *config = calloc(1, sizeof(SpinnerConfig));
  if (!config) {
    return NULL;
  }

  // Copy argv
  config->argv = calloc(argc + 1, sizeof(char *));
  if (!config->argv) {
    free(config);
    return NULL;
  }

  for (size_t i = 0; i < argc; i++) {
    config->argv[i] = strdup(argv[i]);
    if (!config->argv[i]) {
      // Cleanup on failure
      for (size_t j = 0; j < i; j++) {
        free(config->argv[j]);
      }
      free(config->argv);
      free(config);
      return NULL;
    }
  }
  config->argv[argc] = NULL;
  config->argc = argc;

  // Set message
  config->message =
      message ? strdup(message) : config_build_default_message(argv, argc);
  config->timeout = timeout;

  return config;
}

void spinner_config_destroy(SpinnerConfig *config) {
  if (!config) {
    return;
  }

  if (config->argv) {
    for (size_t i = 0; i < config->argc; i++) {
      free(config->argv[i]);
    }
    free(config->argv);
  }

  free(config->message);
  free(config);
}

// ============================================================================
// Main Spinner Interface
// ============================================================================

int spinner_execute(SpinnerConfig *config) {
  if (!config) {
    return SPINNER_ERR_ALLOCATION;
  }

  SignalHandlerBackup signal_backup;
  if (!signal_setup_handlers(&signal_backup)) {
    fprintf(stderr, "Failed to setup signal handlers\n");
    return 1;
  }

  g_child_pid = process_execute(config->argv);
  if (g_child_pid < 0) {
    perror("fork");
    signal_restore_handlers(&signal_backup);
    return SPINNER_ERR_FORK;
  }

  int exit_code =
      spinner_run_with_animation(g_child_pid, config->message, config->timeout);

  signal_restore_handlers(&signal_backup);
  g_child_pid = 0;

  return exit_code;
}

// ============================================================================
// Example Usage
// ============================================================================

int main(void) {
  char *command[] = {"sleep", "5"};

  SpinnerConfig *config =
      spinner_config_create(command, 2, "Waiting for operation to complete...",
                            0 // No timeout
      );

  if (!config) {
    fprintf(stderr, "Failed to create spinner configuration\n");
    return 1;
  }

  int exit_code = spinner_execute(config);
  printf("Process completed with exit code: %d\n", exit_code);

  spinner_config_destroy(config);
  return exit_code;
}
