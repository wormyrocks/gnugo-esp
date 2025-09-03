#include "gnugo_esp_wrapper.h"
#include "gnugo.h"
#include "gg_utils.h"
#include "board.h"
#include "liberty.h"
#include "assert.h"

#define GRID_POINTS FIXED_BOARD_SIZE

static Gameinfo gameinfo_;
static Gameinfo *gameinfo = &gameinfo_;
static SGFTree sgftree;

static esp_gnugo_game_state_t game_state;
static board_update_callback update_cb;

void esp_gnugo_update_board_state(int game_is_over)
{
    // Update board
    for (int x = 0; x < GRID_POINTS; x++)
        for (int y = 0; y < GRID_POINTS; y++)
            (game_state.board)[x][y] = BOARD(y, x);
    int last_move = get_last_move();
    game_state.state = ESP_GNUGO_STATE_WAITING_FOR_PLAYER;
    if (gameinfo->computer_player == gameinfo->to_move)
    {
        game_state.state = ESP_GNUGO_STATE_WAITING_FOR_CPU;
    }

    if (game_is_over)
    {
        game_state.state = ESP_GNUGO_STATE_GAME_OVER;
        game_state.last_event = ESP_GNUGO_EVENT_WIN;
        if (!is_pass(last_move))
        {
            game_state.last_event = ESP_GNUGO_EVENT_RESIGN;
        }
        for (int pos = BOARDMIN; pos < BOARDMAX; pos++)
        {
            if (!IS_STONE(board[pos]))
                continue;
            if (dragon[pos].status == DEAD)
            {
                (game_state.board)[J(pos)][I(pos)] = ((dragon[pos].color == GRID_WHITE) ? GRID_DEAD_WHITE : GRID_DEAD_BLACK);
            }
        }
    }
    else
    {
        if (is_pass(last_move))
        {
            game_state.last_event = ESP_GNUGO_EVENT_PASS;
        }
        else
        {
            game_state.last_event = ESP_GNUGO_EVENT_PLAY;
            if (game_state.black_captured < black_captured || game_state.white_captured < white_captured)
            {
                game_state.last_event = ESP_GNUGO_EVENT_CAPTURE;
            }
        }
        if (movenum)
            location_to_buffer(last_move, (gameinfo->to_move == BLACK) ? game_state.white_last_move : game_state.black_last_move);
    }
    game_state.score = (white_score + black_score) / 2.0;
    game_state.white_turn = (gameinfo->to_move == WHITE);
    game_state.move_number = movenum;
    game_state.black_captured = black_captured;
    game_state.white_captured = white_captured;
    showboard(0);
    if (update_cb != NULL)
        update_cb(&game_state);
}

void esp_gnugo_set_level(int level)
{
    set_level(level);
}

static int sgf_initialized = 0;
static int passes = 0;
static bool undo_allowed = false;

static void
init_sgf(Gameinfo *ginfo)
{
    if (sgf_initialized)
        return;
    sgf_initialized = 1;
    sgf_write_header(sgftree.root, 1, get_random_seed(), komi,
                     ginfo->handicap, get_level(), chinese_rules);
}

void esp_gnugo_start(esp_gnugo_game_init_t init_params)
{
    assert(game_state.state == ESP_GNUGO_STATE_NOT_STARTED);
    passes = 0;
    update_cb = init_params.update_callback;
    undo_allowed = init_params.undo_allowed;
    set_level(init_params.start_level);
    komi = init_params.komi;
    board_size = FIXED_BOARD_SIZE;
    gameinfo_clear(gameinfo);
    gameinfo->handicap = init_params.requested_handicap;
    choose_mc_patterns("montegnu_classic");
    gnugo_clear_board(board_size);
    init_gnugo(init_params.memory_mb, init_params.random_seed);
    if (init_params.sgftree)
    {
        assert(sgftree_readfile(&sgftree, init_params.sgftree));
        assert(gameinfo_play_sgftree(gameinfo, &sgftree, NULL) != EMPTY);
    }
    else
    {
        gameinfo->computer_player = (init_params.player_is_white ? BLACK : WHITE);
        gameinfo->handicap = init_params.requested_handicap;
        gameinfo->to_move = BLACK;
        if (gameinfo->handicap != 0)
        {
            gameinfo->handicap = place_fixed_handicap(gameinfo->handicap);
            gameinfo->to_move = WHITE;
        }
        sgftreeCreateHeaderNode(&sgftree, board_size, komi, handicap);
    }
    gameinfo->game_record = sgftree;
    if (gameinfo->computer_player == gameinfo->to_move)
    {
        game_state.state = ESP_GNUGO_STATE_WAITING_FOR_CPU;
    }
    else
    {
        game_state.state = ESP_GNUGO_STATE_WAITING_FOR_PLAYER;
    }
    esp_gnugo_update_board_state(0);
}

static void process_move(int move, int did_resign)
{
    init_sgf(gameinfo);
    gnugo_play_move(move, gameinfo->to_move);
    sgftreeAddPlay(&sgftree, gameinfo->to_move, I(move), J(move));
    gameinfo->to_move = OTHER_COLOR(gameinfo->to_move);
    if (is_pass(move))
        passes++;
    else
        passes = 0;
    if (passes >= 2 || did_resign)
    {
        if (passes >= 2)
        {
            who_wins(EMPTY, stdout);
        }
        printf("***GAME OVER***\n\n\n");
        esp_gnugo_update_board_state(1);
    }
    else
    {
        esp_gnugo_update_board_state(0);
    }
}

// return 1 if ok, 0 if illegal, -1 if ???
int esp_gnugo_set_player_move_str(char *in_move)
{
    int out_move = 0, resign = 0;
    if (in_move[0] == 'p')
    {
        in_move = PASS_MOVE;
    }
    else if (in_move[0] == 'r')
    {
        resign = 1;
    }
    else
    {
        out_move = string_to_location(FIXED_BOARD_SIZE, in_move);
        if (out_move == NO_MOVE || !is_allowed_move(out_move, gameinfo->to_move))
        {
            printf("illegal\n");
            return 0;
        }
    }
    process_move(out_move, resign);
    return 1;
}

// return 1 if ok, 0 if illegal, -1 if ???
int esp_gnugo_set_player_move_int(int in_move)
{
    int out_move = 0, resign = 0;
    if (!is_allowed_move(in_move, gameinfo->to_move))
    {
        printf("illegal\n");
        return 0;
    }
    process_move(in_move, resign);
    return 1;
}

void esp_gnugo_restart()
{
    passes = 0;
    sgfFreeNode(sgftree.root);
    sgftree_clear(&sgftree);
    sgftreeCreateHeaderNode(&sgftree, board_size, komi, gameinfo->handicap);
    sgf_initialized = 0;
    gameinfo_clear(gameinfo);
}

void esp_gnugo_save_sgf(char *fname)
{
    init_sgf(gameinfo);
    writesgf(sgftree.root, fname);
}

void esp_gnugo_get_computer_move()
{
    init_sgf(gameinfo);
    adjust_level_offset(gameinfo->to_move);
    int resign = 0;
    float move_value;
    int move = genmove(gameinfo->to_move, &move_value, &resign);
    process_move(move, resign);
}

int esp_gnugo_pos_from_xy(int x, int y)
{
    return POS(x, y);
}