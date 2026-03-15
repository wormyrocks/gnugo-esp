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

static int grid_points = 9;

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

/* Public: send one GTP command, return raw response. No echo prefixes. */
char *esp_gnugo_send_gtp(const char *cmd) { return gtp_send_internal(cmd, false); }

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

/* ------------------------------------------------------------------ *
 *  Board state update                                                 *
 * ------------------------------------------------------------------ */

/* by convention, all updates to game_state happen in this function */
static void
esp_gnugo_update_board_state(void)
{
    /* ---- Board: read from list_stones ---- */
    {
        uint8_t new_board[19][19] = {{0}};

        static const char *cmds[2]      = {"list_stones black\n",
                                           "list_stones white\n"};
        static const uint8_t vals[2]    = {GRID_BLACK, GRID_WHITE};

        for (int c = 0; c < 2; c++) {
            char *resp = gtp_send(cmds[c]);
            if (!resp) continue;
            /* Response: "= A1 B2 ...\n\n" */
            char *p = resp + 1;
            char tok[16];
            int n;
            while (sscanf(p, " %15s%n", tok, &n) == 1) {
                int vi, vj;
                if (parse_gtp_vertex(tok, &vi, &vj))
                    new_board[vj][vi] = vals[c];
                p += n;
            }
            free(resp);
        }

        /* Capture animation: if a stone vanished, flash it for one frame */
        for (int x = 0; x < grid_points; x++)
            for (int y = 0; y < grid_points; y++) {
                uint8_t prev = game_state.board[x][y];
                uint8_t cur  = new_board[x][y];
                if (cur == GRID_EMPTY &&
                    (prev == GRID_BLACK || prev == GRID_WHITE))
                    game_state.board[x][y] = prev + 10;   /* GRID_DEAD_* */
                else
                    game_state.board[x][y] = cur;
            }
    }

    /* ---- Move history: count moves, get last two ---- */
    int movenum = 0;
    char last_color[16]  = {0}, last_vertex[16]  = {0};
    char prev_color[16]  = {0}, prev_vertex[16]  = {0};
    {
        char *mresp = gtp_send("move_history\n");
        if (mresp && mresp[0] == '=') {
            char *p = mresp + 1;
            while (*p == ' ') p++;
            int entry = 0;
            char col[16], vtx[16];
            int n;
            while (sscanf(p, "%15s %15s%n", col, vtx, &n) == 2) {
                movenum++;
                if (entry == 0) {
                    memcpy(last_color,  col, sizeof(last_color));
                    memcpy(last_vertex, vtx, sizeof(last_vertex));
                } else if (entry == 1) {
                    memcpy(prev_color,  col, sizeof(prev_color));
                    memcpy(prev_vertex, vtx, sizeof(prev_vertex));
                }
                entry++;
                p += n;
                while (*p == '\n' || *p == '\r') p++;
            }
        }
        free(mresp);
    }

    /* ---- Captures: gtp "captures <color>" returns stones taken FROM that color ----
     *   captures white  =>  gnugo black_captured  =>  game_state.black_captured
     *   captures black  =>  gnugo white_captured  =>  game_state.white_captured  */
    int new_bc, new_wc;
    {
        char *bc_r = gtp_send("captures white\n");
        char *wc_r = gtp_send("captures black\n");
        new_bc = gtp_parse_int(bc_r); free(bc_r);
        new_wc = gtp_parse_int(wc_r); free(wc_r);
    }

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
        if (movenum == 0) {
            game_state.last_event = ESP_GNUGO_EVENT_NONE;
        } else if (strncasecmp(last_vertex, "PASS", 4) == 0) {
            game_state.last_event = ESP_GNUGO_EVENT_PASS;
        } else if (new_bc > game_state.black_captured ||
                   new_wc > game_state.white_captured) {
            game_state.last_event = ESP_GNUGO_EVENT_CAPTURE;
        } else {
            game_state.last_event = ESP_GNUGO_EVENT_PLAY;
        }

        /* Last move strings */
        if (movenum > 0) {
            char *dst = strncasecmp(last_color, "white", 5) == 0 ?
                        game_state.white_last_move : game_state.black_last_move;
            strncpy(dst, last_vertex, sizeof(game_state.white_last_move) - 1);
        }
        if (movenum > 1) {
            char *dst = strncasecmp(prev_color, "white", 5) == 0 ?
                        game_state.white_last_move : game_state.black_last_move;
            strncpy(dst, prev_vertex, sizeof(game_state.white_last_move) - 1);
        }

        char *sresp = gtp_send("estimate_score\n");
        game_state.score = gtp_parse_score(sresp);
        free(sresp);
    }

    game_state.black_captured = new_bc;
    game_state.white_captured = new_wc;
    game_state.level          = wrapper_level;
    game_state.white_turn     = (gameinfo->to_move == WHITE);
    game_state.move_number    = movenum;
    game_state.board_size     = grid_points;

    /* SGF buffer */
    if (sgf_outptr)
        free(sgf_outptr);
    FILE *sgf_outfd = open_memstream(&sgf_outptr, &sgf_outbuf_len);
    assert(sgf_outfd != NULL);
    init_sgf(gameinfo);
    writesgf_fd(sgftree.root, sgf_outfd);
    fclose(sgf_outfd);

    if (update_cb != NULL)
        update_cb(&game_state);
}

/* ------------------------------------------------------------------ *
 *  Board initialisation                                               *
 * ------------------------------------------------------------------ */

/* Comment in 'infile' takes precedence over player_is_white. */
static void
esp_gnugo_init_board_state(char *infile, bool player_is_white,
                           int requested_handicap, int requested_level)
{
    gameinfo_clear(gameinfo);
    int did_load = 0;

    if (infile) {
        struct stat info;
        if (stat(infile, &info) >= 0 && info.st_size > 0) {
            if (sgftree_readfile(&sgftree, infile)) {
                printf("Resumed game from %s.\n", infile);

                /* Detect who is the human from the SGF PW property */
                char *pw_name;
                if (sgfGetCharProperty(sgftree.root, "PW", &pw_name)) {
                    if (!strncmp(pw_name, CPU_NAME, sizeof(CPU_NAME)))
                        player_is_white = false;
                    else if (!strncmp(pw_name, YOUR_NAME, sizeof(YOUR_NAME))) {
                        player_is_white = true;
                        printf("Player is white.\n");
                    }
                }

                /* Use GTP loadsgf to replay the game into the engine */
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "loadsgf %s\n", infile);
                char *resp = gtp_send(cmd);
                if (resp && resp[0] == '=') {
                    did_load       = 1;
                    sgf_initialized = 1;

                    char color_str[16] = {0};
                    sscanf(resp + 1, " %15s", color_str);
                    gameinfo->to_move =
                        strncasecmp(color_str, "white", 5) == 0 ? WHITE : BLACK;

                    char *hresp = gtp_send("get_handicap\n");
                    gameinfo->handicap = gtp_parse_int(hresp);
                    free(hresp);

                    sgfOverwritePropertyInt(sgftree.root, "HA",
                                            gameinfo->handicap);
                    gameinfo->computer_player =
                        player_is_white ? BLACK : WHITE;
                }
                free(resp);
            }
        } else {
            printf("Empty file. Starting new game.\n");
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
    esp_gnugo_update_board_state();
}

/* ------------------------------------------------------------------ *
 *  Public API                                                         *
 * ------------------------------------------------------------------ */

static esp_gnugo_game_init_t i_p;

esp_gnugo_state_t
esp_gnugo_start(esp_gnugo_game_init_t init_params, bool *player_is_white_)
{
    memcpy(&i_p, &init_params, sizeof(esp_gnugo_game_init_t));
    assert(game_state.state == ESP_GNUGO_STATE_NOT_STARTED);

    passes       = 0;
    autolevel_on = init_params.autolevel;
    update_cb    = init_params.update_callback;
    undo_allowed = init_params.undo_allowed;
    wrapper_komi = init_params.komi;

    grid_points = init_params.board_size > 0 ? init_params.board_size : 9;

    init_gnugo(init_params.memory_mb, init_params.random_seed);
#ifndef CONFIG_DISABLE_MONTE_CARLO
    choose_mc_patterns("montegnu_classic");
#endif

    /* boardsize clears the board and primes gtp_boardsize for coord parsing */
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "boardsize %d\n", grid_points);
    char *r = gtp_send(cmd); free(r);

    snprintf(cmd, sizeof(cmd), "komi %.1f\n", wrapper_komi);
    r = gtp_send(cmd); free(r);

    strcpy(sgfname, "-");
    if (init_params.outfile) {
        strcpy(sgfname, init_params.outfile);
        printf("Game may be manually saved to %s\n", init_params.outfile);
    }

    esp_gnugo_init_board_state(init_params.infile, init_params.player_is_white,
                               init_params.requested_handicap,
                               init_params.start_level);

    *player_is_white_ = (gameinfo->computer_player == BLACK);
    return game_state.state;
}

/* Play one move through GTP and update all bookkeeping. */
static void
process_move(int move, int did_resign)
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

    esp_gnugo_update_board_state();
}

int
esp_gnugo_set_player_command(engine_signal_t e)
{
    go_command_t go_command = e.cmd;
    int move_if_any = e.pos;

    if (game_is_over && go_command <= COMMAND_RESIGN)
        return 0;

    switch (go_command) {
    case COMMAND_PASS:
    case COMMAND_RESIGN:
        process_move(0, (go_command == COMMAND_RESIGN));
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
            process_move(move_if_any, 0);
        return legal;
    }

    case COMMAND_RESTART:
        esp_gnugo_restart(game_state.level, (gameinfo->computer_player == BLACK));
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
            esp_gnugo_update_board_state();
        }
        return ok;
    }

    case COMMAND_FORCEQUIT:
        return 1;

    default:
        return game_state.state;
    }
}

void
esp_gnugo_restart(int requested_level, bool player_is_white)
{
    passes       = 0;
    game_is_over = 0;
    sgfFreeNode(sgftree.root);
    sgftree_clear(&sgftree);
    sgftreeCreateHeaderNode(&sgftree, grid_points, wrapper_komi,
                            i_p.requested_handicap);
    sgfAddProperty(sgftree.root, "PW",
                   player_is_white ? YOUR_NAME : CPU_NAME);
    gameinfo_clear(gameinfo);
    esp_gnugo_init_board_state(NULL, player_is_white,
                               i_p.requested_handicap, requested_level);
}

esp_gnugo_state_t
esp_gnugo_get_computer_move(void)
{
    init_sgf(gameinfo);

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "genmove %s\n",
             (gameinfo->to_move == BLACK) ? "black" : "white");

    char *response = gtp_send(cmd);
    if (!response)
        return game_state.state;

    /* Response: "= resign\n\n", "= PASS\n\n", or "= <vertex>\n\n" */
    int did_resign = (strncmp(response, "= resign", 8) == 0);
    int color      = gameinfo->to_move;

    if (!did_resign) {
        char vertex[16] = {0};
        sscanf(response + 1, " %15s", vertex);

        /* Record to SGF */
        int vi, vj;
        if (parse_gtp_vertex(vertex, &vi, &vj))
            sgftreeAddPlay(&sgftree, color, vi, vj);
        else
            sgftreeAddPlay(&sgftree, color, -1, -1);   /* PASS */

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

    esp_gnugo_update_board_state();
    return game_state.state;
}

int
esp_gnugo_pos_from_xy(int x, int y)
{
    return POS(x, y);
}

esp_gnugo_game_state_t *
esp_gnugo_get_game_state(void)
{
    return &game_state;
}

esp_gnugo_state_t
esp_gnugo_get_state(void)
{
    return game_state.state;
}

/*
 * Dump the in-memory SGF buffer to disk.  The buffer is only updated in
 * esp_gnugo_update_board_state, giving atomic, deterministic snapshots.
 */
void
esp_gnugo_dump_sgf(char *filename)
{
    if (sgf_outbuf_len == 0)
        return;
    printf("Game saved to %s (%d bytes).\n", filename, (int)sgf_outbuf_len);
    FILE *f = fopen(filename, "w");
    fwrite(sgf_outptr, sgf_outbuf_len, 1, f);
    fclose(f);
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

    bool player_is_white;
    esp_gnugo_start(ctx->init_params, &player_is_white);
    ctx->player_is_white_out = player_is_white ? 1 : 0;

    /* Copy initial state to snapshot */
    memcpy(&ctx->state_snapshot, &game_state, sizeof(esp_gnugo_game_state_t));
    ctx->state_ready = 1;

    ctx->engine_status = ENGINE_STATUS_READY;
    printf("[engine] Ready. board_size=%d\n", grid_points);

    while (!ctx->quit_requested) {
        esp_gnugo_state_t st = game_state.state;

        if (st == ESP_GNUGO_STATE_WAITING_FOR_CPU) {
            ctx->engine_status = ENGINE_STATUS_THINKING;
            printf("[engine] Computing move...\n");
            esp_gnugo_get_computer_move();
            memcpy(&ctx->state_snapshot, &game_state,
                   sizeof(esp_gnugo_game_state_t));
            ctx->state_ready = 1;
            ctx->engine_status = ENGINE_STATUS_READY;
        } else if (st == ESP_GNUGO_STATE_WAITING_FOR_PLAYER ||
                   st == ESP_GNUGO_STATE_GAME_OVER) {
            if (ctx->command_ready) {
                engine_signal_t cmd = ctx->command_buffer;
                ctx->command_ready = 0;
                printf("[engine] Processing command: cmd=%d pos=%d\n",
                       cmd.cmd, cmd.pos);
                esp_gnugo_set_player_command(cmd);
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

    /* Save SGF before exiting */
    if (sgf_outbuf_len > 0 && sgfname[0] != '-') {
        esp_gnugo_dump_sgf(sgfname);
    }

    ctx->engine_status = ENGINE_STATUS_STOPPED;
    printf("[engine] Thread exiting.\n");
}
