#include "gnugo_esp_wrapper.h"
#include "gnugo.h"
#include "gg_utils.h"
#include "interface.h"
#include "gtp.h"
#include "board.h"
#include "liberty.h"
#include "assert.h"
#include "string.h"
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifdef ESP_PLATFORM
#include "esp_random.h"
#endif

static int grid_points = 9;

static Gameinfo gameinfo_;
static Gameinfo *gameinfo = &gameinfo_;
static char sgfname[128] = {0};
static SGFTree sgftree;
static esp_gnugo_game_state_t game_state;
static int sgf_initialized = 0;
static int passes = 0;
static int game_is_over = 0;
static bool undo_allowed = false;

/* Move tracking — avoids querying move_history via GTP. */
static int  total_moves = 0;
static int  last_move_pos  = -1;   /* board POS, or -1 (none), or 0 (pass) */
static int  last_move_color = 0;   /* BLACK or WHITE */
static int  prev_move_pos  = -1;
static int  prev_move_color = 0;

/* Local mirrors of GTP-controlled settings (avoid reading gnugo globals). */
static float wrapper_komi  = 6.5f;
static int   wrapper_level = 0;

/* memstream for in-memory SGF output */
static char  *sgf_outptr    = NULL;
static size_t sgf_outbuf_len = 0;

/* ------------------------------------------------------------------ *
 *  GTP dispatch                                                       *
 * ------------------------------------------------------------------ */

/*
 * Core GTP dispatch: send one command (must end with '\n'), optionally
 * echoing to stdout.  Returns response in a malloc'd buffer; caller frees.
 */
static char *
gtp_send_internal(const char *cmd, bool echo)
{
    if (echo) {
        printf(">> %s", cmd);
        fflush(stdout);
    }
    FILE *in = fmemopen((void *)cmd, strlen(cmd), "r");
    if (!in)
        return NULL;
    char   *out_buf = NULL;
    size_t  out_len = 0;
    FILE   *out = open_memstream(&out_buf, &out_len);
    assert(out != NULL);
    gtp_run_command(in, out);
    fclose(in);
    fclose(out);
    if (echo) {
        printf("<< %s", out_buf ? out_buf : "(null)\n");
        fflush(stdout);
    }
    return out_buf;
}

/* Internal use: echo command + response to stdout. */
static char *gtp_send(const char *cmd) { return gtp_send_internal(cmd, true); }

/* ------------------------------------------------------------------ *
 *  GTP response parsers                                               *
 * ------------------------------------------------------------------ */

/* Parse a single integer from a GTP success response "= <int>\n\n". */
static int
gtp_parse_int(const char *resp)
{
    int val = 0;
    if (resp && resp[0] == '=')
        sscanf(resp + 1, " %d", &val);
    return val;
}

/*
 * Parse a score from "= W+X.X (...)\n\n" or "= B+X.X (...)\n\n".
 * Returns positive float if white leads, negative if black leads.
 */
static float
gtp_parse_score(const char *resp)
{
    if (!resp || resp[0] != '=')
        return 0.0f;
    const char *p = resp + 1;
    while (*p == ' ') p++;
    float val = 0.0f;
    if (p[0] == 'W' && p[1] == '+')
        sscanf(p + 2, "%f", &val);
    else if (p[0] == 'B' && p[1] == '+') {
        sscanf(p + 2, "%f", &val);
        val = -val;
    }
    return val;
}

/*
 * Convert a GTP vertex token ("A1", "D5", "PASS") to gnugo (i, j).
 * Returns 1 on success, 0 for PASS or invalid vertex.
 */
static int
parse_gtp_vertex(const char *v, int *out_i, int *out_j)
{
    if (!v || !*v || strncasecmp(v, "PASS", 4) == 0)
        return 0;
    int col = tolower((unsigned char)v[0]) - 'a';
    if (col >= 8) col--;   /* skip 'i' */
    if (col < 0 || col >= grid_points)
        return 0;
    int row = atoi(v + 1);
    if (row < 1 || row > grid_points)
        return 0;
    *out_j = col;
    *out_i = grid_points - row;
    return 1;
}

/* Convert a gnugo board position to a GTP vertex string (e.g. "D5", "PASS"). */
static void
pos_to_gtp_vertex(int pos, char *buf)
{
    if (is_pass(pos)) {
        strcpy(buf, "PASS");
        return;
    }
    int i = I(pos), j = J(pos);
    buf[0] = 'A' + j + (j >= 8 ? 1 : 0);
    snprintf(buf + 1, 8, "%d", grid_points - i);
}

/* ------------------------------------------------------------------ *
 *  SGF helpers                                                        *
 * ------------------------------------------------------------------ */

static void
init_sgf(Gameinfo *ginfo)
{
    if (sgf_initialized)
        return;
    sgf_initialized = 1;
    sgf_write_header(sgftree.root, 1, get_random_seed(), wrapper_komi,
                     ginfo->handicap, wrapper_level, chinese_rules);
    if (ginfo->handicap > 0)
        sgffile_recordboard(sgftree.root);
}

/* Forward declarations for internal functions */
static void esp_gnugo_restart(engine_context_t *ctx, int requested_level, bool player_is_white);

/* ------------------------------------------------------------------ *
 *  Board state update                                                 *
 * ------------------------------------------------------------------ */

/* by convention, all updates to game_state happen in this function.
 * Pass ctx to publish the fresh SGF snapshot to ctx->sgf_buf; pass NULL
 * when the caller doesn't have a ctx (e.g. during restart). */
static void
esp_gnugo_update_board_state(engine_context_t *ctx)
{
    /* ---- Board: read directly from GNU Go's board[] array ---- */
    {
        uint8_t new_board[19][19] = {{0}};
        static const uint8_t color_map[3] = {GRID_EMPTY, GRID_WHITE, GRID_BLACK};

        for (int i = 0; i < grid_points; i++)
            for (int j = 0; j < grid_points; j++)
                new_board[j][i] = color_map[BOARD(i, j)];

        /* Copy board state directly — the UI handles capture animations */
        for (int x = 0; x < grid_points; x++)
            for (int y = 0; y < grid_points; y++)
                game_state.board[x][y] = new_board[x][y];
    }

    /* ---- Captures: read directly from GNU Go globals ----
     *   GNU Go's black_captured = stones captured FROM black = white's captures
     *   GNU Go's white_captured = stones captured FROM white = black's captures */
    int new_bc = white_captured;
    int new_wc = black_captured;

    /* ---- Game state & events ---- */
    game_state.state = ESP_GNUGO_STATE_WAITING_FOR_PLAYER;
    if (gameinfo->computer_player == gameinfo->to_move)
        game_state.state = ESP_GNUGO_STATE_WAITING_FOR_CPU;

    if (game_is_over) {
        game_state.state = ESP_GNUGO_STATE_GAME_OVER;

        if (passes == 2) {
            game_state.last_event = ESP_GNUGO_EVENT_WIN;

            /* Mark dead stones */
            char *dresp = gtp_send("final_status_list dead\n");
            if (dresp && dresp[0] == '=') {
                char *p = dresp + 1;
                char tok[16];
                int n;
                while (sscanf(p, " %15s%n", tok, &n) == 1) {
                    int vi, vj;
                    if (parse_gtp_vertex(tok, &vi, &vj)) {
                        uint8_t v = game_state.board[vj][vi];
                        if (v == GRID_BLACK || v == GRID_WHITE)
                            game_state.board[vj][vi] = v + 10;
                    }
                    p += n;
                }
            }
            free(dresp);

            char *sresp = gtp_send("final_score\n");
            game_state.score = gtp_parse_score(sresp);
            free(sresp);
        } else {
            game_state.last_event = ESP_GNUGO_EVENT_RESIGN;
            game_state.score = 0.0f;
        }
    } else {
        if (total_moves == 0) {
            game_state.last_event = ESP_GNUGO_EVENT_NONE;
        } else if (last_move_pos <= 0) {
            game_state.last_event = ESP_GNUGO_EVENT_PASS;
        } else if (new_bc > game_state.black_captured ||
                   new_wc > game_state.white_captured) {
            game_state.last_event = ESP_GNUGO_EVENT_CAPTURE;
        } else {
            game_state.last_event = ESP_GNUGO_EVENT_PLAY;
        }

        /* Last move strings from tracked positions */
        memset(game_state.white_last_move, 0, sizeof(game_state.white_last_move));
        memset(game_state.black_last_move, 0, sizeof(game_state.black_last_move));
        if (total_moves > 0 && last_move_pos > 0) {
            char *dst = (last_move_color == WHITE) ?
                        game_state.white_last_move : game_state.black_last_move;
            pos_to_gtp_vertex(last_move_pos, dst);
        }
        if (total_moves > 1 && prev_move_pos > 0) {
            char *dst = (prev_move_color == WHITE) ?
                        game_state.white_last_move : game_state.black_last_move;
            pos_to_gtp_vertex(prev_move_pos, dst);
        }

    }

    game_state.black_captured = new_bc;
    game_state.white_captured = new_wc;
    game_state.level          = wrapper_level;
    game_state.white_turn     = (gameinfo->to_move == WHITE);
    game_state.move_number    = total_moves;
    game_state.board_size     = grid_points;

    /* SGF buffer — build new first, publish atomically, then free old, so
     * a UI thread reading ctx->sgf_buf never sees a dangling pointer if
     * the engine task is killed mid-regeneration. */
    char *new_ptr = NULL;
    size_t new_len = 0;
    FILE *sgf_outfd = open_memstream(&new_ptr, &new_len);
    assert(sgf_outfd != NULL);
    init_sgf(gameinfo);
    writesgf_fd(sgftree.root, sgf_outfd);
    fclose(sgf_outfd);

    char *old_ptr = sgf_outptr;
    sgf_outptr = new_ptr;
    sgf_outbuf_len = new_len;
    if (ctx) {
        ctx->sgf_buf = new_ptr;
        ctx->sgf_buf_len = new_len;
    }
    free(old_ptr);
}

/* ------------------------------------------------------------------ *
 *  Board initialisation                                               *
 * ------------------------------------------------------------------ */

/*
 * Remove a named property from an SGF node (no-op if absent).
 */
static void
remove_property(SGFNode *node, const char *name)
{
    short nam = name[0] | (name[1] << 8);
    SGFProperty **pp = &node->props;
    while (*pp) {
        if ((*pp)->name == nam) {
            SGFProperty *dead = *pp;
            *pp = dead->next;
            free(dead->value);
            free(dead);
        } else {
            pp = &(*pp)->next;
        }
    }
}

/*
 * Load a game from the SGF tree already parsed into sgftree.
 * Uses GTP loadsgf :memory: to replay moves into the engine.
 * Reads custom properties (XC/XZ/XV) into ctx if provided, then strips
 * them from the tree so they don't propagate to future SGF regenerations —
 * the UI owns UI state and re-injects it on save.
 * Returns 1 on success, 0 on failure.
 */
static int
load_game_from_sgftree(bool *player_is_white, engine_context_t *ctx)
{
    /* Detect who is the human from the SGF PW property */
    char *pw_name;
    if (sgfGetCharProperty(sgftree.root, "PW", &pw_name)) {
        if (!strncmp(pw_name, CPU_NAME, sizeof(CPU_NAME)))
            *player_is_white = false;
        else if (!strncmp(pw_name, YOUR_NAME, sizeof(YOUR_NAME))) {
            *player_is_white = true;
            printf("Player is white.\n");
        }
    }

    /* Replay moves into engine via GTP.  If we have the SGF in a
     * memory buffer (ctx->sgf_buf), use :memory: mode to avoid
     * filesystem access.  Otherwise fall back to the wrapper's
     * sgftree (which was already parsed from a file). */
    char *resp;
    if (ctx && ctx->sgf_buf && ctx->sgf_buf_len > 0) {
        gtp_set_loadsgf_buffer(ctx->sgf_buf, ctx->sgf_buf_len);
        resp = gtp_send("loadsgf :memory:\n");
    } else {
        /* Fallback: re-read from the file that sgftree was loaded from.
         * This path is used when loading from /flash/out.sgf on cold boot
         * where the file was already read into sgftree. */
        resp = gtp_send("loadsgf :memory:\n");
    }

    if (!resp || resp[0] != '=') {
        free(resp);
        return 0;
    }

    char color_str[16] = {0};
    sscanf(resp + 1, " %15s", color_str);
    gameinfo->to_move =
        strncasecmp(color_str, "white", 5) == 0 ? WHITE : BLACK;
    free(resp);

    char *hresp = gtp_send("get_handicap\n");
    gameinfo->handicap = gtp_parse_int(hresp);
    free(hresp);

    sgfOverwritePropertyInt(sgftree.root, "HA", gameinfo->handicap);
    gameinfo->computer_player = *player_is_white ? BLACK : WHITE;
    sgf_initialized = 1;

    /* Sync grid_points with the board size from the loaded SGF */
    char *bresp = gtp_send("query_boardsize\n");
    int loaded_size = gtp_parse_int(bresp);
    free(bresp);
    if (loaded_size > 0)
        grid_points = loaded_size;

    /* Read custom UI state into ctx, then strip from the tree so the
     * engine never re-emits them — UI re-injects fresh values on save. */
    if (ctx) {
        char *val;
        if (sgfGetCharProperty(sgftree.root, "XC", &val))
            sscanf(val, "%d,%d", &ctx->cursor_x, &ctx->cursor_y);
        if (sgfGetCharProperty(sgftree.root, "XZ", &val))
            ctx->zoomed = atoi(val);
        if (sgfGetCharProperty(sgftree.root, "XV", &val))
            sscanf(val, "%d,%d", &ctx->viewport_x, &ctx->viewport_y);
    }
    remove_property(sgftree.root, "XC");
    remove_property(sgftree.root, "XZ");
    remove_property(sgftree.root, "XV");

    return 1;
}

static void
esp_gnugo_init_board_state(engine_context_t *ctx, bool player_is_white,
                           int requested_handicap, int requested_level)
{
    gameinfo_clear(gameinfo);
    int did_load = 0;

    /* Try to load from memory buffer */
    if (ctx && ctx->sgf_buf && ctx->sgf_buf_len > 0) {
        if (sgftree_readbuf(&sgftree, ctx->sgf_buf, ctx->sgf_buf_len)) {
            printf("Resumed game from memory buffer (%zu bytes).\n",
                   ctx->sgf_buf_len);
            did_load = load_game_from_sgftree(&player_is_white, ctx);
        }
    }

    /* Try to load from file */
    if (!did_load && ctx && ctx->init_params.infile) {
        char *infile = ctx->init_params.infile;
        struct stat info;
        if (stat(infile, &info) >= 0 && info.st_size > 0) {
            if (sgftree_readfile(&sgftree, infile)) {
                printf("Resumed game from %s.\n", infile);
                /* Set up buffer for loadsgf :memory: — read the file into
                 * memory so the GTP command doesn't need filesystem access */
                FILE *f = fopen(infile, "r");
                if (f) {
                    char *filebuf = malloc(info.st_size);
                    size_t n = fread(filebuf, 1, info.st_size, f);
                    fclose(f);
                    gtp_set_loadsgf_buffer(filebuf, n);
                    did_load = load_game_from_sgftree(&player_is_white, ctx);
                    free(filebuf);
                }
            }
        } else {
            printf("No saved game. Starting new game.\n");
        }
    }

    /* Fresh start */
    if (!did_load) {
        printf("Starting new game with level %d\n", requested_level);

        /* Clear the board for a clean slate */
        char *cr = gtp_send("clear_board\n");
        free(cr);

        if (requested_level != -1) {
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "level %d\n", requested_level);
            char *r = gtp_send(cmd);
            free(r);
            wrapper_level = requested_level;
        }

        gameinfo->computer_player = player_is_white ? BLACK : WHITE;
        gameinfo->handicap        = requested_handicap;
        gameinfo->to_move         = BLACK;

        if (gameinfo->handicap != 0) {
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "fixed_handicap %d\n",
                     gameinfo->handicap);
            char *r = gtp_send(cmd);
            free(r);
            gameinfo->to_move = WHITE;
        }

        sgf_initialized = 0;
        sgftreeCreateHeaderNode(&sgftree, grid_points, wrapper_komi,
                                gameinfo->handicap);
        sgfAddProperty(sgftree.root, "PW",
                       player_is_white ? YOUR_NAME : CPU_NAME);
    }

    gameinfo->game_record = sgftree;
    memset(&game_state, 0, sizeof(esp_gnugo_game_state_t));
    total_moves = 0;
    last_move_pos = -1;  last_move_color = 0;
    prev_move_pos = -1;  prev_move_color = 0;
    esp_gnugo_update_board_state(ctx);
}

/* ------------------------------------------------------------------ *
 *  Public API                                                         *
 * ------------------------------------------------------------------ */

static int restart_handicap = 0;

static esp_gnugo_state_t
esp_gnugo_start(engine_context_t *ctx, bool *player_is_white_)
{
    esp_gnugo_game_init_t init_params = ctx->init_params;
    assert(game_state.state == ESP_GNUGO_STATE_NOT_STARTED);

    restart_handicap = init_params.requested_handicap;
    passes       = 0;
    undo_allowed = init_params.undo_allowed;
    wrapper_komi = init_params.komi;

    grid_points = init_params.board_size > 0 ? init_params.board_size : 9;

    /* init_gnugo must only be called once — it initializes DFA tables,
     * hash caches, and transformation data that cannot be reinitialized. */
    static int gnugo_initialized = 0;
    if (!gnugo_initialized) {
        unsigned int seed = init_params.random_seed;
#ifdef ESP_PLATFORM
        if (seed == 0)
            seed = esp_random();
#endif
        init_gnugo(init_params.memory_mb, seed);
#ifndef CONFIG_DISABLE_MONTE_CARLO
        choose_mc_patterns("montegnu_classic");
#endif
        showtime = 1;
        showstatistics = 1;
        gnugo_initialized = 1;
    } else {
        unsigned int seed = init_params.random_seed;
#ifdef ESP_PLATFORM
        if (seed == 0)
            seed = esp_random();
#endif
        set_random_seed(seed);
    }

    /* boardsize clears the board and primes gtp_boardsize for coord parsing */
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "boardsize %d\n", grid_points);
    char *r = gtp_send(cmd); free(r);

    snprintf(cmd, sizeof(cmd), "komi %.1f\n", wrapper_komi);
    r = gtp_send(cmd); free(r);

    strcpy(sgfname, "-");

    esp_gnugo_init_board_state(ctx, init_params.player_is_white,
                               init_params.requested_handicap,
                               init_params.start_level);

    *player_is_white_ = (gameinfo->computer_player == BLACK);
    return game_state.state;
}

/* Play one move through GTP and update all bookkeeping. */
static void
process_move(engine_context_t *ctx, int move, int did_resign)
{
    init_sgf(gameinfo);

    if (!did_resign) {
        char vertex[16];
        pos_to_gtp_vertex(move, vertex);
        char cmd[48];
        snprintf(cmd, sizeof(cmd), "play %s %s\n",
                 (gameinfo->to_move == BLACK) ? "black" : "white",
                 vertex);
        char *r = gtp_send(cmd);
        free(r);
    }

    /* Track last moves */
    prev_move_pos   = last_move_pos;
    prev_move_color = last_move_color;
    last_move_pos   = move;
    last_move_color = gameinfo->to_move;
    total_moves++;

    /* Record to SGF (pass recorded for resign too, matching prior behaviour) */
    sgftreeAddPlay(&sgftree, gameinfo->to_move, I(move), J(move));
    gameinfo->to_move = OTHER_COLOR(gameinfo->to_move);

    if (did_resign) {
        game_is_over = 1;
    } else {
        if (is_pass(move)) {
            if (++passes == 2)
                game_is_over = 1;
        } else {
            passes = 0;
        }
    }
    if (passes)
        printf("passes: %d %d\n", passes, game_is_over);

    esp_gnugo_update_board_state(ctx);
}

static esp_gnugo_state_t esp_gnugo_get_computer_move(engine_context_t *ctx);

static int
esp_gnugo_set_player_command(engine_context_t *ctx, engine_signal_t e)
{
    go_command_t go_command = e.cmd;
    int move_if_any = e.pos;

    if (game_is_over && go_command <= COMMAND_RESIGN)
        return 0;

    switch (go_command) {
    case COMMAND_PASS:
    case COMMAND_RESIGN:
        process_move(ctx, 0, (go_command == COMMAND_RESIGN));
        return 1;

    case COMMAND_PLAY: {
        char vertex[16];
        pos_to_gtp_vertex(move_if_any, vertex);
        char cmd[48];
        snprintf(cmd, sizeof(cmd), "is_legal %s %s\n",
                 (gameinfo->to_move == BLACK) ? "black" : "white",
                 vertex);
        char *r = gtp_send(cmd);
        int legal = gtp_parse_int(r);
        free(r);
        if (legal)
            process_move(ctx, move_if_any, 0);
        else
            game_state.last_event = ESP_GNUGO_EVENT_ILLEGAL;
        return legal;
    }

    case COMMAND_RESTART:
        esp_gnugo_restart(ctx, game_state.level, (gameinfo->computer_player == BLACK));
        return 1;

    case COMMAND_UNDO: {
        /* Undo 2 moves (CPU + player) to get back to player's turn.
         * If it's currently the player's turn and no move was played yet,
         * undo 2 as well (undo previous pair). */
        int undo_count = 2;
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "gg-undo %d\n", undo_count);
        char *r = gtp_send(cmd);
        int ok = (r && r[0] == '=');
        free(r);
        if (ok) {
            game_state.last_event = ESP_GNUGO_EVENT_UNDO;
            /* Reset pass counter since we're rewinding */
            passes = 0;
            game_is_over = 0;
            /* Flip to_move back by undo_count moves */
            for (int i = 0; i < undo_count; i++)
                gameinfo->to_move = OTHER_COLOR(gameinfo->to_move);
            /* Roll back move tracking */
            total_moves = (total_moves > undo_count) ? total_moves - undo_count : 0;
            last_move_pos = -1;  last_move_color = 0;
            prev_move_pos = -1;  prev_move_color = 0;
            esp_gnugo_update_board_state(ctx);
        }
        return ok;
    }

    case COMMAND_BENCHMARK: {
        /* pos encodes: low byte = num_moves, high byte = level+1 (0=keep) */
        int num_moves = e.pos & 0xFF;
        int req_level = (e.pos >> 8) & 0xFF;
        if (num_moves == 0 || num_moves > 20) num_moves = 6;

        /* Clear saved game so restart begins from empty board */
        ctx->sgf_buf = NULL;
        ctx->sgf_buf_len = 0;
        ctx->init_params.infile = NULL;
        bool player_is_white = (gameinfo->computer_player == BLACK);
        int level = req_level > 0 ? req_level - 1 : wrapper_level;
        esp_gnugo_restart(ctx, level, player_is_white);
        /* Force level via GTP (restart may resume from SGF and skip level set) */
        {
            char lcmd[32];
            snprintf(lcmd, sizeof(lcmd), "level %d\n", level);
            char *lr = gtp_send(lcmd);
            free(lr);
            wrapper_level = level;
        }
        ctx->two_player = 0;
        set_random_seed(1);

        fprintf(stderr, "\n=== ENGINE BENCHMARK: %d moves, level %d, %dx%d ===\n",
                num_moves, wrapper_level, grid_points, grid_points);
        struct timeval tv_total_start, tv_total_end;
        gettimeofday(&tv_total_start, NULL);
        long total_nodes = 0;
        int moves_played = 0;
        for (int i = 0; i < num_moves && !game_is_over; i++) {
            esp_gnugo_get_computer_move(ctx);
            total_nodes += stats.nodes;
            moves_played++;
        }
        gettimeofday(&tv_total_end, NULL);
        int total_ms = (tv_total_end.tv_sec - tv_total_start.tv_sec) * 1000
                     + (tv_total_end.tv_usec - tv_total_start.tv_usec) / 1000;
        long avg_nps = total_ms > 0 ? (total_nodes * 1000) / total_ms : 0;
        fprintf(stderr, "=== BENCHMARK DONE: %d moves, %dms total, %ld nodes, %ld nodes/sec avg ===\n\n",
                moves_played, total_ms, total_nodes, avg_nps);
        esp_gnugo_update_board_state(ctx);
        return 1;
    }

    case COMMAND_FORCEQUIT:
        return 1;

    default:
        return game_state.state;
    }
}

static void
esp_gnugo_restart(engine_context_t *ctx, int requested_level, bool player_is_white)
{
    passes       = 0;
    game_is_over = 0;
    sgfFreeNode(sgftree.root);
    sgftree_clear(&sgftree);
    sgftreeCreateHeaderNode(&sgftree, grid_points, wrapper_komi,
                            restart_handicap);
    sgfAddProperty(sgftree.root, "PW",
                   player_is_white ? YOUR_NAME : CPU_NAME);
    gameinfo_clear(gameinfo);
    esp_gnugo_init_board_state(ctx, player_is_white,
                               restart_handicap, requested_level);
}

static esp_gnugo_state_t
esp_gnugo_get_computer_move(engine_context_t *ctx)
{
    init_sgf(gameinfo);

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "genmove %s\n",
             (gameinfo->to_move == BLACK) ? "black" : "white");

    struct timeval tv_start, tv_end;
    gettimeofday(&tv_start, NULL);

    char *response = gtp_send(cmd);
    if (!response)
        return game_state.state;

    gettimeofday(&tv_end, NULL);
    int elapsed_ms = (tv_end.tv_sec - tv_start.tv_sec) * 1000
                   + (tv_end.tv_usec - tv_start.tv_usec) / 1000;
    int nps = elapsed_ms > 0 ? (stats.nodes * 1000) / elapsed_ms : 0;
    fprintf(stderr, "[gnugo] genmove: %dms, %d nodes, %d nodes/sec (level %d, %dx%d)\n",
            elapsed_ms, stats.nodes, nps, wrapper_level, grid_points, grid_points);

    /* Response: "= resign\n\n", "= PASS\n\n", or "= <vertex>\n\n" */
    int did_resign = (strncmp(response, "= resign", 8) == 0);
    int color      = gameinfo->to_move;

    if (!did_resign) {
        char vertex[16] = {0};
        sscanf(response + 1, " %15s", vertex);

        /* Record to SGF */
        int vi, vj;
        int is_vertex = parse_gtp_vertex(vertex, &vi, &vj);
        if (is_vertex)
            sgftreeAddPlay(&sgftree, color, vi, vj);
        else
            sgftreeAddPlay(&sgftree, color, -1, -1);   /* PASS */

        /* Track last moves */
        prev_move_pos   = last_move_pos;
        prev_move_color = last_move_color;
        last_move_color = color;
        last_move_pos   = is_vertex ? POS(vi, vj) : 0;
        total_moves++;

        if (strncasecmp(vertex, "PASS", 4) == 0) {
            if (++passes == 2)
                game_is_over = 1;
        } else {
            passes = 0;
        }
    } else {
        game_is_over = 1;
    }

    free(response);
    gameinfo->to_move = OTHER_COLOR(color);

    esp_gnugo_update_board_state(ctx);
    return game_state.state;
}

int
esp_gnugo_pos_from_xy(int x, int y)
{
    return POS(x, y);
}

/* ------------------------------------------------------------------ *
 *  Platform sleep                                                     *
 * ------------------------------------------------------------------ */

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static void platform_sleep_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
#else
static void platform_sleep_ms(int ms) { usleep(ms * 1000); }
#endif

/* ------------------------------------------------------------------ *
 *  Engine thread main (called from Focus GnuGoTask)                   *
 * ------------------------------------------------------------------ */

void
go_engine_thread_main(engine_context_t *ctx)
{
    printf("[engine] Thread starting...\n");
    ctx->engine_status = ENGINE_STATUS_STARTING;

    /* Reset global state left over from any previous game so the assertion
       in esp_gnugo_start() passes when the engine is restarted. */
    game_state.state = ESP_GNUGO_STATE_NOT_STARTED;
    game_is_over = 0;

    bool player_is_white;
    esp_gnugo_start(ctx, &player_is_white);
    ctx->player_is_white_out = player_is_white ? 1 : 0;

    /* Copy initial state to snapshot */
    memcpy(&ctx->state_snapshot, &game_state, sizeof(esp_gnugo_game_state_t));
    ctx->state_ready = 1;

    ctx->engine_status = ENGINE_STATUS_READY;
    printf("[engine] Ready. board_size=%d\n", grid_points);

    while (!ctx->quit_requested) {
        esp_gnugo_state_t st = game_state.state;

        if (st == ESP_GNUGO_STATE_WAITING_FOR_CPU && !ctx->two_player) {
            ctx->engine_status = ENGINE_STATUS_THINKING;
            printf("[engine] Computing move...\n");
            esp_gnugo_get_computer_move(ctx);
            memcpy(&ctx->state_snapshot, &game_state,
                   sizeof(esp_gnugo_game_state_t));
            ctx->state_ready = 1;
            ctx->engine_status = ENGINE_STATUS_READY;
        } else if (st == ESP_GNUGO_STATE_WAITING_FOR_PLAYER ||
                   st == ESP_GNUGO_STATE_WAITING_FOR_CPU ||
                   st == ESP_GNUGO_STATE_GAME_OVER) {
            if (ctx->command_ready) {
                engine_signal_t cmd = ctx->command_buffer;
                ctx->command_ready = 0;
                printf("[engine] Processing command: cmd=%d pos=%d\n",
                       cmd.cmd, cmd.pos);
                esp_gnugo_set_player_command(ctx, cmd);
                memcpy(&ctx->state_snapshot, &game_state,
                       sizeof(esp_gnugo_game_state_t));
                ctx->state_ready = 1;
            } else {
                platform_sleep_ms(10);
            }
        } else {
            platform_sleep_ms(10);
        }
    }

    ctx->engine_status = ENGINE_STATUS_STOPPED;
    printf("[engine] Thread exiting. SGF buffer: %zu bytes.\n", sgf_outbuf_len);
}
