from dataclasses import dataclass
from os import (
    WEXITSTATUS,
    WIFEXITED,
    WIFSIGNALED,
    WNOHANG,
    WTERMSIG,
    _exit,
    execvp,
    fork,
    kill,
    waitpid,
)
from signal import SIGINT, SIGKILL, SIGQUIT, SIGTERM, signal
from sys import stderr
from time import monotonic, sleep
from types import FrameType
from typing import Any, Callable, Optional, TypeAlias

_HANDLER: TypeAlias = Optional[Callable[[int, FrameType | None], Any] | int]


@dataclass(frozen=True)
class Spinner:
    command: list[str]
    wait_text: str | None = None
    max_duration: int | None = None

    def __post_init__(self):
        if self.wait_text is None:
            object.__setattr__(
                self,
                "wait_text",
                f"Please wait, running command: {self.command}",
            )

    def _hide_cursor(self):
        print("\033[?25l", end="", flush=True)

    def _show_cursor(self):
        print("\033[?25h", end="", flush=True)

    def spin(self) -> int:
        # Normalize command
        argv: list[str] = (
            [self.command] if isinstance(self.command, str) else self.command
        )

        print(f"{self.wait_text} ", end="", flush=True)
        self._hide_cursor()

        pid: int = fork()
        if pid == 0:
            # --- Child ---
            try:
                execvp(argv[0], argv)
            except Exception as e:
                print(f"\nexec failed: {e}", file=stderr)
                _exit(127)

        # --- Parent ---
        spinner = "-\\|/"
        start: float = monotonic()
        i = 0
        interrupted = False
        signal_received = None

        def sig_handler(signum: int, _: FrameType | None):
            nonlocal interrupted, signal_received
            interrupted = True
            signal_received = signum
            # Forward the signal to the child process
            try:
                kill(pid, signum)
            except ProcessLookupError:
                # Child already exited
                pass

        # Store old handlers to restore later
        old_sigint: _HANDLER = signal(SIGINT, sig_handler)
        old_sigterm: _HANDLER = signal(SIGTERM, sig_handler)
        old_sigquit: _HANDLER = signal(SIGQUIT, sig_handler)

        try:
            while True:
                # Check for interruption FIRST, before checking if process finished
                if interrupted:
                    # Give child process a moment to handle the signal
                    sleep(0.1)
                    # Wait for child to exit
                    finished_pid, status = waitpid(pid, 0)

                    if signal_received is not None:
                        signal_names: dict[int, str] = {
                            SIGINT: "SIGINT",
                            SIGTERM: "SIGTERM",
                            SIGQUIT: "SIGQUIT",
                        }

                        signal_name = signal_names.get(
                            signal_received, f"signal {signal_received}"
                        )
                        print(f"\nInterrupted by {signal_name}.", file=stderr)

                        # Return appropriate exit code for the signal
                        return 128 + signal_received
                    else:
                        print("\nInterrupted.", file=stderr)
                        return 130

                finished_pid, status = waitpid(pid, WNOHANG)
                if finished_pid != 0:
                    break

                if self.max_duration is not None:
                    if monotonic() - start >= self.max_duration:
                        print("\nProcess timed out.", file=stderr)
                        kill(pid, SIGTERM)
                        # Wait a bit for graceful shutdown
                        sleep(1)
                        try:
                            finished_pid, status = waitpid(pid, WNOHANG)
                            if finished_pid == 0:
                                # Still running, force kill
                                kill(pid, SIGKILL)
                                waitpid(pid, 0)
                        except ProcessLookupError:
                            pass
                        return 124

                print(spinner[i % len(spinner)], end="", flush=True)
                sleep(0.2)
                print("\b", end="", flush=True)
                i += 1

            # Process finished normally
            if WIFEXITED(status):
                return WEXITSTATUS(status)
            elif WIFSIGNALED(status):
                # Child was terminated by a signal
                return 128 + WTERMSIG(status)
            else:
                return 128

        finally:
            # ALWAYS restore terminal state and signal handlers
            signal(SIGINT, old_sigint)
            signal(SIGTERM, old_sigterm)
            signal(SIGQUIT, old_sigquit)
            self._show_cursor()
            print()  # move to clean line


if __name__ == "__main__":
    spinner = Spinner(["sleep", "5"], "Hehe Waitin")
    exit_code: int = spinner.spin()
    print(f"Exit code: {exit_code}")
    _exit(exit_code)
