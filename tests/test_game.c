#define main flappy_bird_main
#include "../proj1_20201574.c"
#undef main

#include <assert.h>

static Game game_without_spawning(int row)
{
    Game game;
    init_game(&game);
    game.bird_row = row;
    game.frames_until_pipe = 100;
    return game;
}

static void test_initial_board(void)
{
    Game game;
    char board[BOARD_ROWS][BOARD_COLS];
    init_game(&game);
    make_board(&game, board);
    for (int row = 0; row < BOARD_ROWS; row++) {
        for (int col = 0; col < BOARD_COLS; col++) {
            const char expected = row == BOARD_ROWS / 2 && col == BIRD_COL ? '@' : ' ';
            assert(board[row][col] == expected);
        }
    }
}

static void test_boundaries_and_flaps(void)
{
    Game game = game_without_spawning(1);
    update_game(&game, 1);
    assert(game.game_over && game.bird_row == 1);

    game = game_without_spawning(BOARD_ROWS - 2);
    update_game(&game, 0);
    assert(game.game_over && game.bird_row == BOARD_ROWS - 2);

    game = game_without_spawning(12);
    update_game(&game, 2);
    assert(!game.game_over && game.bird_row == 8);

    game = game_without_spawning(12);
    update_game(&game, UINT_MAX);
    assert(game.game_over);
    assert(game.bird_row > 0 && game.bird_row < BOARD_ROWS - 1);
}

static void test_collisions(void)
{
    Game game = game_without_spawning(10);
    game.pipes[0] = (Pipe){.x = BIRD_COL, .gap_top = 10, .active = true};
    assert(!pipe_at(&game, 10, BIRD_COL));
    assert(pipe_at(&game, 9, BIRD_COL));
    update_game(&game, 0);
    assert(!game.game_over && game.bird_row == 11);

    game = game_without_spawning(10);
    game.pipes[0] = (Pipe){.x = BIRD_COL + 1, .gap_top = 0, .active = true};
    update_game(&game, 0);
    assert(game.game_over && game.score == 0);

    game = game_without_spawning(11);
    game.pipes[0] = (Pipe){.x = BIRD_COL, .gap_top = 11, .active = true};
    update_game(&game, 1);
    assert(game.game_over && game.bird_row == 11);
}

static void test_scoring_and_clipping(void)
{
    Game game = game_without_spawning(5);
    game.pipes[0] = (Pipe){.x = BIRD_COL + 1, .gap_top = 0, .active = true};
    for (int frame = 0; frame < 11; frame++) {
        game.bird_row = 5;
        update_game(&game, 0);
        assert(!game.game_over);
        assert(game.score == (frame == 10 ? 1 : 0));
    }
    for (int frame = 0; frame < 12; frame++) {
        game.bird_row = 5;
        update_game(&game, 0);
        assert(!game.game_over && game.score == 1);
    }

    game = game_without_spawning(BOARD_ROWS - 2);
    game.pipes[0] = (Pipe){.x = 0, .gap_top = 14, .active = true};
    update_game(&game, 0);
    assert(game.game_over && game.score == 0);

    char board[BOARD_ROWS][BOARD_COLS];
    for (int x = -PIPE_WIDTH - 1; x <= BOARD_COLS + 1; x++) {
        game = game_without_spawning(12);
        game.pipes[0] = (Pipe){.x = x, .gap_top = 7, .active = true};
        make_board(&game, board);
        for (int row = 0; row < BOARD_ROWS; row++) {
            for (int col = 0; col < BOARD_COLS; col++) {
                const char expected = row == game.bird_row && col == BIRD_COL
                    ? '@' : pipe_at(&game, row, col) ? '+' : ' ';
                assert(board[row][col] == expected);
            }
        }
    }
}

static void test_random_gaps(void)
{
    bool saw_top = false;
    bool saw_bottom = false;
    srand(1);
    for (int i = 0; i < 1000; i++) {
        Game game = game_without_spawning(12);
        spawn_pipe(&game);
        const int gap = game.pipes[0].gap_top;
        assert(game.pipes[0].active);
        assert(gap >= 0 && gap <= BOARD_ROWS - GAP_HEIGHT);
        saw_top |= gap == 0;
        saw_bottom |= gap == BOARD_ROWS - GAP_HEIGHT;
    }
    assert(saw_top && saw_bottom);
}

static void test_long_game(void)
{
    Game game;
    char board[BOARD_ROWS][BOARD_COLS];
    srand(2);
    init_game(&game);
    for (int frame = 0; frame < 3000; frame++) {
        game.bird_row = BOARD_ROWS / 2;
        for (size_t i = 0; i < MAX_PIPES; i++) {
            const Pipe *pipe = &game.pipes[i];
            if (pipe->active && BIRD_COL >= pipe->x - 1 &&
                BIRD_COL < pipe->x - 1 + PIPE_WIDTH) {
                game.bird_row = pipe->gap_top + GAP_HEIGHT / 2;
                break;
            }
        }
        update_game(&game, 0);
        assert(!game.game_over);
        make_board(&game, board);
        assert(board[game.bird_row][BIRD_COL] == '@');
    }
    assert(game.score >= 100);
}

int main(void)
{
    test_initial_board();
    test_boundaries_and_flaps();
    test_collisions();
    test_scoring_and_clipping();
    test_random_gaps();
    test_long_game();
    return 0;
}
