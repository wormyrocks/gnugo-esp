#include "gnugo_esp_wrapper.h"
#include "gnugo.h"
#include "gg_utils.h"
#include "board.h"
#include "liberty.h"
#include "assert.h"
#include "string.h"

#define GRID_POINTS FIXED_BOARD_SIZE

static Gameinfo gameinfo_;
static Gameinfo *gameinfo = &gameinfo_;
static char sgfname[128] = {0};
static SGFTree sgftree;
static esp_gnugo_game_state_t game_state;
static board_update_callback update_cb;
static int sgf_initialized = 0;
static int passes = 0;
static int game_is_over = 0;
static bool undo_allowed = false;

// by convention (for now), all updates to game_state happen in this function
static void esp_gnugo_update_board_state()
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
        // If last move was a resignation, no need to update board or show dead stones
        if (passes == 2)
        {
            game_state.last_event = ESP_GNUGO_EVENT_WIN;
            for (int pos = BOARDMIN; pos < BOARDMAX; pos++)
            {
                if (!IS_STONE(board[pos]))
                    continue;
                if (dragon[pos].status == DEAD)
                {
                    // Deadify stone
                    (game_state.board)[J(pos)][I(pos)] += 10;
                }
            }
        }
        else
        {
            game_state.last_event = ESP_GNUGO_EVENT_RESIGN;
        }
    }
    else
    {
        if (movenum == 0)
            game_state.last_event = ESP_GNUGO_EVENT_NONE;
        else if (is_pass(get_last_move()))
            game_state.last_event = ESP_GNUGO_EVENT_PASS;
        else
        {
            game_state.last_event = ESP_GNUGO_EVENT_PLAY;
            if (game_state.black_captured < black_captured || game_state.white_captured < white_captured)
            {
                game_state.last_event = ESP_GNUGO_EVENT_CAPTURE;
            }
        }
        if (movenum)
        {
            location_to_buffer(get_last_move(), (gameinfo->to_move == BLACK) ? game_state.white_last_move : game_state.black_last_move);
        }
    }
    game_state.level = get_level();
    game_state.score = (white_score + black_score) / 2.0;
    game_state.white_turn = (gameinfo->to_move == WHITE);
    game_state.move_number = movenum;
    game_state.black_captured = black_captured;
    game_state.white_captured = white_captured;
    if (update_cb != NULL)
        update_cb(&game_state);
}

static void esp_gnugo_init_board_state(char *infile, bool player_is_white, int requested_handicap)
{
    gameinfo_clear(gameinfo);
    int did_load = 0;
    if (infile)
    {
        printf("Trying to resume from %s\n", infile);
        if (sgftree_readfile(&sgftree, infile))
        {
            did_load = (gameinfo_play_sgftree(gameinfo, &sgftree, NULL) != EMPTY);
            if (did_load)
                sgf_initialized = 1;
            sgfOverwritePropertyInt(sgftree.root, "HA", gameinfo->handicap);
        }
    }
    // Start fresh
    if (!did_load)
    {
        gameinfo->computer_player = (player_is_white ? BLACK : WHITE);
        gameinfo->handicap = requested_handicap;
        handicap = gameinfo->handicap;
        gameinfo->to_move = BLACK;
        if (gameinfo->handicap != 0)
        {
            gameinfo->handicap = place_fixed_handicap(gameinfo->handicap);
            gameinfo->to_move = WHITE;
        }
        sgf_initialized = 0;
        sgftreeCreateHeaderNode(&sgftree, board_size, komi, gameinfo->handicap);
    }
    gameinfo->game_record = sgftree;
    white_score = 0;
    black_score = 0;
    memset(&game_state, 0, sizeof(esp_gnugo_game_state_t));
    esp_gnugo_update_board_state();
}

void esp_gnugo_set_level(int level)
{
    set_level(level);
}

static void
init_sgf(Gameinfo *ginfo)
{
    if (sgf_initialized)
        return;
    sgf_initialized = 1;
    sgf_write_header(sgftree.root, 1, get_random_seed(), komi,
                     ginfo->handicap, get_level(), chinese_rules);
    if (ginfo->handicap > 0)
        sgffile_recordboard(sgftree.root);
}
static esp_gnugo_game_init_t i_p;
esp_gnugo_state_t esp_gnugo_start(esp_gnugo_game_init_t init_params)
{
    memcpy(&i_p, &init_params, sizeof(esp_gnugo_game_init_t));
    assert(game_state.state == ESP_GNUGO_STATE_NOT_STARTED);
    passes = 0;
    autolevel_on = init_params.autolevel;
    update_cb = init_params.update_callback;
    undo_allowed = init_params.undo_allowed;
    set_level(init_params.start_level);
    komi = init_params.komi;
    board_size = FIXED_BOARD_SIZE;
    choose_mc_patterns("montegnu_classic");
    gnugo_clear_board(board_size);
    init_gnugo(init_params.memory_mb, init_params.random_seed);
    // outfilename: autosave file. default to stdout
    strcpy(sgfname, "-");

    // init_params.outfile: user saves this explicitly
    if (init_params.outfile)
    {
        strcpy(sgfname, init_params.outfile);
        printf("Game may be manually saved to %s\n", init_params.outfile);
    }
    esp_gnugo_init_board_state(init_params.infile, init_params.player_is_white, init_params.requested_handicap);
    return game_state.state;
}

static void process_move(int move, int did_resign)
{
    init_sgf(gameinfo);
    gnugo_play_move(move, gameinfo->to_move);
    sgftreeAddPlay(&sgftree, gameinfo->to_move, I(move), J(move));
    gameinfo->to_move = OTHER_COLOR(gameinfo->to_move);
    if (did_resign)
    {
        game_is_over = 1;
    }
    else
    {
        if (is_pass(move))
        {
            if (++passes == 2)
            {
                game_is_over = 1;
                who_wins(EMPTY, stdout);
            }
        }
        else
        {
            passes = 0;
        }
    }
    printf("passes: %d %d\n",passes, game_is_over);
    esp_gnugo_update_board_state();
}

// return 1? check the game state
// return 0? nothing change, engine expects another command
int esp_gnugo_set_player_command(engine_signal_t e)
{
    go_command_t go_command = e.cmd;
    int move_if_any = e.pos;
    if (game_is_over && go_command <= COMMAND_RESIGN)
        return 0;
    switch (go_command)
    {
    case COMMAND_PASS:
    case COMMAND_RESIGN:
        process_move(0, (go_command == COMMAND_RESIGN));
        return 1;
    case COMMAND_PLAY:
    {
        int ret = is_allowed_move(move_if_any, gameinfo->to_move);
        if (ret)
            process_move(move_if_any, 0);
        return ret;
    }
    case COMMAND_SAVE:
    {
        printf("Game saved to %s\n", sgfname);
        init_sgf(gameinfo);
        writesgf(sgftree.root, sgfname);
        return 0;
    }
    case COMMAND_RESTART:
        esp_gnugo_restart();
        return 1;
    case COMMAND_FORCEQUIT:
        return 1;
    }
    return game_state.state;
}

void esp_gnugo_restart()
{
    passes = 0;
    game_is_over = 0;
    sgfFreeNode(sgftree.root);
    sgftree_clear(&sgftree);
    sgftreeCreateHeaderNode(&sgftree, board_size, komi, i_p.requested_handicap);
    gameinfo_clear(gameinfo);
    // Restarting should ignore the input filename arg
    esp_gnugo_init_board_state(NULL, i_p.player_is_white, i_p.requested_handicap);
}

esp_gnugo_state_t esp_gnugo_get_computer_move()
{
    init_sgf(gameinfo);
    adjust_level_offset(gameinfo->to_move);
    int resign = 0;
    float move_value;
    int move = genmove(gameinfo->to_move, &move_value, &resign);
    process_move(move, resign);
    return game_state.state;
}

int esp_gnugo_pos_from_xy(int x, int y)
{
    return POS(x, y);
}

esp_gnugo_game_state_t *esp_gnugo_get_game_state()
{
    return &game_state;
}

esp_gnugo_state_t esp_gnugo_get_state()
{
    return game_state.state;
}
