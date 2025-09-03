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
static int last_move;

void esp_gnugo_update_board_state(int game_is_over)
{
    // Update board
    for (int x = 0; x < GRID_POINTS; x++)
        for (int y = 0; y < GRID_POINTS; y++)
        {
            int val = BOARD(y, x);
            if ((val == GRID_EMPTY) && IS_STONE((game_state.board)[x][y]))
            {
                // Show dead stones as captured for one frame
                game_state.board[x][y] += 10;
            }
            else
            {
                game_state.board[x][y] = val;
            }
        }
    game_state.state = ESP_GNUGO_STATE_WAITING_FOR_PLAYER;
    if (gameinfo->computer_player == gameinfo->to_move)
    {
        game_state.state = ESP_GNUGO_STATE_WAITING_FOR_CPU;
    }

    if (game_is_over)
    {
        game_state.state = ESP_GNUGO_STATE_GAME_OVER;
        game_state.last_event = ESP_GNUGO_EVENT_WIN;
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
    komi = (float)init_params.komi;
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

// Return 1 if game is over
static int enter_pass()
{
    passes++;
    init_sgf(gameinfo);
    gnugo_play_move(PASS_MOVE, gameinfo->to_move);
    sgftreeAddPlay(&sgftree, gameinfo->to_move, -1, -1);
    gameinfo->to_move = OTHER_COLOR(gameinfo->to_move);
    last_move = PASS_MOVE;
    if (passes >= 2)
    {
        // Score game
        who_wins(EMPTY, stdout);
        printf("***GAME OVER***\n\n\n");
        esp_gnugo_update_board_state(1);
        return 1;
    }
    esp_gnugo_update_board_state(0);
    return 0;
}

static int player_move(char *location)
{
    int move;
    if (location[0] == 'p')
    {
        move = PASS_MOVE;
        return enter_pass();
    }
    else
    {
        move = string_to_location(FIXED_BOARD_SIZE, location);
        if (move == NO_MOVE || !is_allowed_move(move, gameinfo->to_move))
        {
            printf("illegal\n");
            return 0;
        }
    }
    passes = 0;
    init_sgf(gameinfo);
    gnugo_play_move(move, gameinfo->to_move);
    sgftreeAddPlay(&sgftree, gameinfo->to_move, I(move), J(move));
    last_move = move;
    gameinfo->to_move = OTHER_COLOR(gameinfo->to_move);
    esp_gnugo_update_board_state(0);
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

// return 1 if ok, 0 if illegal, -1 if ???
int esp_gnugo_set_player_move(char *in_move)
{
    return player_move(in_move);
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
    if (resign)
    {
        printf("COMPUTER RESIGNED.\n");
        game_state.state = ESP_GNUGO_STATE_GAME_OVER;
    }
    if (is_pass(move))
    {
        printf("COMPUTER PASSES");
        return enter_pass();
    }
    passes = 0;
    gnugo_play_move(move, gameinfo->to_move);
    sgftreeAddPlay(&sgftree, gameinfo->to_move, I(move), J(move));
    game_state.state = ESP_GNUGO_STATE_WAITING_FOR_PLAYER;
    last_move = move;
    gameinfo->to_move = OTHER_COLOR(gameinfo->to_move);
    esp_gnugo_update_board_state(0);
}
