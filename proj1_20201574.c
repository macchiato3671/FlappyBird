#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum {
    BOARD_ROWS = 25,
    BOARD_COLS = 40,
    BIRD_COL = 9,
    PIPE_WIDTH = 10,
    GAP_HEIGHT = 10,
    PIPE_INTERVAL = 25,
    MAX_PIPES = 4,
    FRAME_MILLISECONDS = 200
};

typedef struct {
    int x;
    int gap_top;
    bool active;
    bool passed;
} Pipe;

typedef struct {
    Pipe pipes[MAX_PIPES];
    int bird_row;
    int score;
    int frames_until_pipe;
    bool game_over;
} Game;

static struct termios original_terminal;
static bool terminal_changed;
static volatile sig_atomic_t termination_signal;
static volatile sig_atomic_t suspend_requested;

static void restore_terminal(void)
{
    if (terminal_changed) {
        int result;
        do {
            result = tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal);
        } while (result == -1 && errno == EINTR);
        if (result == 0) {
            terminal_changed = false;
        }
    }
}

static int enter_game_mode(void)
{
    struct termios game_terminal = original_terminal;
    game_terminal.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    game_terminal.c_cc[VMIN] = 1;
    game_terminal.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &game_terminal) == -1) {
        return -1;
    }
    terminal_changed = true;
    return 0;
}

static void handle_signal(int signal_number)
{
    if (signal_number == SIGTSTP) {
        suspend_requested = 1;
    } else {
        termination_signal = signal_number;
    }
}

static int set_signal_handler(int signal_number, void (*handler)(int))
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handler;
    if (sigemptyset(&action.sa_mask) == -1) {
        return -1;
    }
    return sigaction(signal_number, &action, NULL);
}

static int install_signal_handlers(void)
{
    const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE, SIGTSTP};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        if (set_signal_handler(signals[i], handle_signal) == -1) {
            return -1;
        }
    }
    return 0;
}

static void init_game(Game *game)
{
    memset(game, 0, sizeof(*game));
    game->bird_row = BOARD_ROWS / 2;
}

static bool pipe_at(const Game *game, int row, int col)
{
    for (size_t i = 0; i < MAX_PIPES; i++) {
        const Pipe *pipe = &game->pipes[i];
        if (pipe->active && col >= pipe->x && col < pipe->x + PIPE_WIDTH &&
            (row < pipe->gap_top || row >= pipe->gap_top + GAP_HEIGHT)) {
            return true;
        }
    }
    return false;
}

static void move_bird(Game *game, int distance)
{
    const int direction = distance < 0 ? -1 : 1;
    const int steps = distance < 0 ? -distance : distance;
    for (int i = 0; i < steps; i++) {
        const int next_row = game->bird_row + direction;
        if (next_row <= 0 || next_row >= BOARD_ROWS - 1 ||
            pipe_at(game, next_row, BIRD_COL)) {
            game->game_over = true;
            return;
        }
        game->bird_row = next_row;
    }
}

static void spawn_pipe(Game *game)
{
    for (size_t i = 0; i < MAX_PIPES; i++) {
        Pipe *pipe = &game->pipes[i];
        if (!pipe->active) {
            pipe->x = BOARD_COLS - 1;
            pipe->gap_top = rand() % (BOARD_ROWS - GAP_HEIGHT + 1);
            pipe->active = true;
            pipe->passed = false;
            return;
        }
    }
}

static void update_game(Game *game, unsigned int flaps)
{
    for (size_t i = 0; i < MAX_PIPES; i++) {
        Pipe *pipe = &game->pipes[i];
        if (pipe->active) {
            pipe->x--;
            if (pipe->x + PIPE_WIDTH <= 0) {
                pipe->active = false;
            }
        }
    }

    if (game->frames_until_pipe == 0) {
        spawn_pipe(game);
        game->frames_until_pipe = PIPE_INTERVAL - 1;
    } else {
        game->frames_until_pipe--;
    }

    if (pipe_at(game, game->bird_row, BIRD_COL)) {
        game->game_over = true;
        return;
    }

    if (flaps == 0) {
        move_bird(game, 1);
    } else {
        while (flaps > 0 && !game->game_over) {
            move_bird(game, -2);
            flaps--;
        }
    }

    if (game->game_over) {
        return;
    }

    for (size_t i = 0; i < MAX_PIPES; i++) {
        Pipe *pipe = &game->pipes[i];
        if (pipe->active && !pipe->passed &&
            pipe->x + PIPE_WIDTH - 1 < BIRD_COL) {
            pipe->passed = true;
            game->score++;
        }
    }
}

static void make_board(const Game *game, char board[BOARD_ROWS][BOARD_COLS])
{
    memset(board, ' ', BOARD_ROWS * BOARD_COLS);
    for (int row = 0; row < BOARD_ROWS; row++) {
        for (int col = 0; col < BOARD_COLS; col++) {
            if (pipe_at(game, row, col)) {
                board[row][col] = '+';
            }
        }
    }
    board[game->bird_row][BIRD_COL] = '@';
}

static int draw_board(const Game *game)
{
    char board[BOARD_ROWS][BOARD_COLS];
    make_board(game, board);

    fputs("\033[H\033[2J", stdout);
    printf("Score: \033[31m%d\033[0m\n", game->score);
    fputs("W/A/S/D or Space: flap  |  Ctrl+C: quit\n", stdout);
    for (int row = 0; row < BOARD_ROWS; row++) {
        for (int col = 0; col < BOARD_COLS; col++) {
            if (board[row][col] == '+') {
                fputs("\033[32m+\033[0m", stdout);
            } else {
                putchar(board[row][col]);
            }
        }
        putchar('\n');
    }
    return fflush(stdout) == EOF || ferror(stdout) ? -1 : 0;
}

static bool is_flap_key(unsigned char ch)
{
    return ch == 'w' || ch == 'W' || ch == 'a' || ch == 'A' ||
           ch == 's' || ch == 'S' || ch == 'd' || ch == 'D' || ch == ' ';
}

static void advance_deadline(struct timespec *deadline)
{
    deadline->tv_nsec += FRAME_MILLISECONDS * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static long long nanoseconds_until(const struct timespec *deadline,
                                   const struct timespec *now)
{
    return (long long)(deadline->tv_sec - now->tv_sec) * 1000000000LL +
           deadline->tv_nsec - now->tv_nsec;
}

static int suspend_game(struct timespec *deadline, const Game *game)
{
    suspend_requested = 0;
    restore_terminal();
    if (termination_signal) {
        return 0;
    }
    if (set_signal_handler(SIGTSTP, SIG_DFL) == -1 || raise(SIGTSTP) != 0 ||
        set_signal_handler(SIGTSTP, handle_signal) == -1) {
        return -1;
    }
    if (termination_signal) {
        return 0;
    }
    if (enter_game_mode() == -1 || clock_gettime(CLOCK_MONOTONIC, deadline) == -1) {
        return -1;
    }
    advance_deadline(deadline);
    return draw_board(game);
}

static int run_game(Game *game)
{
    struct timespec deadline;
    unsigned int pending_flaps = 0;

    if (clock_gettime(CLOCK_MONOTONIC, &deadline) == -1 || draw_board(game) == -1) {
        return -1;
    }
    advance_deadline(&deadline);

    while (!game->game_over && !termination_signal) {
        struct timespec now;
        if (suspend_requested) {
            if (suspend_game(&deadline, game) == -1) {
                return -1;
            }
            continue;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
            return -1;
        }

        const long long remaining = nanoseconds_until(&deadline, &now);
        if (remaining <= 0) {
            update_game(game, pending_flaps);
            pending_flaps = 0;
            advance_deadline(&deadline);
            if (!game->game_over && draw_board(game) == -1) {
                return -1;
            }
            continue;
        }

        const long long wait_ms = (remaining + 999999LL) / 1000000LL;
        struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
        const int ready = poll(&input, 1, wait_ms > INT_MAX ? INT_MAX : (int)wait_ms);
        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (ready > 0 && (input.revents & (POLLERR | POLLNVAL))) {
            errno = EIO;
            return -1;
        }
        if (ready > 0 && (input.revents & (POLLIN | POLLHUP))) {
            unsigned char ch;
            const ssize_t length = read(STDIN_FILENO, &ch, 1);
            if (length == 0) {
                errno = EIO;
                return -1;
            }
            if (length == -1) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (is_flap_key(ch) && pending_flaps < UINT_MAX) {
                pending_flaps++;
            }
        }
    }
    return 0;
}

int main(void)
{
    Game game;
    if (!isatty(STDIN_FILENO)) {
        fputs("Error: standard input must be a terminal.\n", stderr);
        return EXIT_FAILURE;
    }
    if (tcgetattr(STDIN_FILENO, &original_terminal) == -1) {
        perror("tcgetattr");
        return EXIT_FAILURE;
    }
    if (atexit(restore_terminal) != 0) {
        fputs("Error: could not register terminal cleanup.\n", stderr);
        return EXIT_FAILURE;
    }
    if (install_signal_handlers() == -1 || enter_game_mode() == -1) {
        perror("terminal setup");
        return EXIT_FAILURE;
    }

    srand((unsigned int)time(NULL));
    init_game(&game);
    const int result = run_game(&game);
    const int saved_errno = errno;
    restore_terminal();

    if (termination_signal) {
        return 128 + termination_signal;
    }
    if (result == -1) {
        errno = saved_errno;
        perror("game");
        return EXIT_FAILURE;
    }

    fputs("\033[H\033[2Jgame over!\n", stdout);
    printf("Score: %d\n", game.score);
    if (fflush(stdout) == EOF) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
