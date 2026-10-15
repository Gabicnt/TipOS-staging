#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static int report(const char *name, int passed) {
    const char *result = passed ? "PASS " : "FAIL ";
    write(STDOUT_FILENO, result, 5);
    write(STDOUT_FILENO, name, strlen(name));
    write(STDOUT_FILENO, "\n", 1);
    return passed;
}

static int read_exact(int fd, char *buffer, size_t length) {
    size_t total = 0;
    while (total < length) {
        ssize_t result = read(fd, buffer + total, length - total);
        if (result <= 0) return -1;
        total += (size_t)result;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    int master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    failures += !report("open /dev/ptmx", master >= 0);
    if (master < 0) return 1;

    int pty_number = -1;
    int pty_number_result = ioctl(master, TIOCGPTN, &pty_number);
    failures += !report("TIOCGPTN", pty_number_result == 0 && pty_number >= 0);
    if (pty_number_result != 0 || pty_number < 0) {
        close(master);
        return 1;
    }
    int slave_locked = 0;
    int unlock_result = ioctl(master, TIOCSPTLCK, &slave_locked);
    failures += !report("TIOCSPTLCK unlock", unlock_result == 0);
    if (unlock_result != 0) {
        close(master);
        return 1;
    }

    char slave_path[32];
    int path_length = snprintf(slave_path, sizeof(slave_path),
                               "/dev/pts/%d", pty_number);
    if (path_length < 0 || (size_t)path_length >= sizeof(slave_path)) {
        close(master);
        return 1;
    }
    int slave = open(slave_path, O_RDWR | O_NOCTTY);
    failures += !report("open /dev/pts/N", slave >= 0);
    if (slave < 0) {
        close(master);
        return 1;
    }

    struct termios termios_state;
    int termios_get_result = ioctl(slave, TCGETS, &termios_state);
    int termios_default_ok = termios_get_result == 0 &&
        (termios_state.c_lflag & (ICANON | ECHO | ISIG)) ==
            (ICANON | ECHO | ISIG) &&
        termios_state.c_cc[VINTR] == 3 &&
        termios_state.c_cc[VEOF] == 4 &&
        termios_state.c_cc[VERASE] == 127 &&
        termios_state.c_cc[VKILL] == 21 &&
        termios_state.c_cc[VMIN] == 1 && termios_state.c_cc[VTIME] == 0;
    termios_state.c_oflag = 0;
    int termios_set_result = ioctl(slave, TCSETS, &termios_state);
    memset(&termios_state, 0, sizeof(termios_state));
    int termios_get_after_result = ioctl(slave, TCGETS, &termios_state);
    failures += !report("TCGETS/TCSETS and defaults",
                        termios_default_ok && termios_set_result == 0 &&
                        termios_get_after_result == 0 &&
                        termios_state.c_oflag == 0);

    const char input[] = "hello\n";
    char slave_input[sizeof(input)] = {0};
    ssize_t master_written = write(master, input, sizeof(input) - 1);
    int slave_read_result = read_exact(slave, slave_input, sizeof(input) - 1);
    failures += !report("master write to canonical slave",
                        master_written == (ssize_t)(sizeof(input) - 1) &&
                        slave_read_result == 0 &&
                        memcmp(slave_input, input, sizeof(input) - 1) == 0);

    char echo[sizeof(input)] = {0};
    int echo_result = read_exact(master, echo, sizeof(input) - 1);
    failures += !report("canonical echo to master",
                        echo_result == 0 &&
                        memcmp(echo, input, sizeof(input) - 1) == 0);

    termios_state.c_lflag &= ~ECHO;
    int termios_echo_set_result = ioctl(slave, TCSETS, &termios_state);
    failures += !report("disable canonical echo", termios_echo_set_result == 0);

    const char output[] = "world\n";
    char master_output[sizeof(output)] = {0};
    ssize_t slave_written = write(slave, output, sizeof(output) - 1);
    int master_read_result = read_exact(master, master_output,
                                        sizeof(output) - 1);
    failures += !report("slave write to master",
                        slave_written == (ssize_t)(sizeof(output) - 1) &&
                        master_read_result == 0 &&
                        memcmp(master_output, output, sizeof(output) - 1) == 0);

    struct winsize window;
    int winsize_get_result = ioctl(slave, TIOCGWINSZ, &window);
    window.ws_row = 30;
    window.ws_col = 100;
    int winsize_set_result = ioctl(slave, TIOCSWINSZ, &window);
    memset(&window, 0, sizeof(window));
    int winsize_get_after_result = ioctl(master, TIOCGWINSZ, &window);
    failures += !report("TIOCGWINSZ/TIOCSWINSZ",
                        winsize_get_result == 0 && winsize_set_result == 0 &&
                        winsize_get_after_result == 0 &&
                        window.ws_row == 30 && window.ws_col == 100);

    close(slave);
    close(master);
    const char *result = failures ? "FAIL\n" : "PASS\n";
    write(STDOUT_FILENO, result, strlen(result));
    return failures ? 1 : 0;
}