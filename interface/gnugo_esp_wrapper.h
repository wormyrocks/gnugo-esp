#ifndef _ESP_WRAPPER_H
#define _ESP_WRAPPER_H
#include "stdint.h"
#include "stdbool.h"
#include <stdio.h>

#define GRID_EMPTY 0
#define GRID_WHITE 1
#define GRID_BLACK 2
#define GRID_STRANGE 3

#define GRID_DEAD_WHITE 11
#define GRID_DEAD_BLACK 12

#define GRID_HOSHI 10

#define YOUR_NAME "You"
#define CPU_NAME "CPU"

// Engine => UI
typedef enum
{
    ESP_GNUGO_STATE_NOT_STARTED,
    ESP_GNUGO_STATE_WAITING_FOR_PLAYER,
    ESP_GNUGO_STATE_WAITING_FOR_CPU,
    ESP_GNUGO_STATE_GAME_OVER,
} esp_gnugo_state_t;

// Engine => UI
typedef enum
{
    ESP_GNUGO_EVENT_NONE,
    ESP_GNUGO_EVENT_PLAY,
    ESP_GNUGO_EVENT_ILLEGAL,
    ESP_GNUGO_EVENT_CAPTURE,
    ESP_GNUGO_EVENT_PASS,
    ESP_GNUGO_EVENT_WIN,
    ESP_GNUGO_EVENT_RESIGN,
    ESP_GNUGO_EVENT_UNDO,
    ESP_GNUGO_EVENT_RESTART
} esp_gnugo_event_t;

// Engine => UI
typedef struct
{
    uint8_t board[19][19];
    char white_last_move[20];
    char black_last_move[20];
    float score;
    int black_captured;
    int white_captured;
    int move_number;
    int white_turn;
    int level;
    int board_size;
    esp_gnugo_state_t state;
    esp_gnugo_event_t last_event;
} esp_gnugo_game_state_t;

// UI => Engine
typedef enum
{
    COMMAND_NONE,
    COMMAND_PASS,
    COMMAND_PLAY,
    COMMAND_RESIGN,
    COMMAND_RESTART,
    COMMAND_SAVE,
    COMMAND_FORCEQUIT,
    COMMAND_UNDO,
} go_command_t;

// UI => Engine
typedef struct __attribute__((packed))
{
    go_command_t cmd : 16;
    int pos : 16;
} engine_signal_t;

// UI => Engine
typedef void (*board_update_callback)(const esp_gnugo_game_state_t *);
// UI => Engine
typedef struct
{
    bool player_is_white;
    bool undo_allowed;
    bool autolevel;
    int start_level;
    float komi;
    int random_seed;
    int memory_mb;
    int requested_handicap;
    char *infile;
    char *outfile;
    board_update_callback update_callback;
    int board_size;
} esp_gnugo_game_init_t;

esp_gnugo_state_t esp_gnugo_start(esp_gnugo_game_init_t, bool*);
void esp_gnugo_restart(int level, bool player_is_white);
esp_gnugo_state_t esp_gnugo_get_computer_move(void);
int esp_gnugo_set_player_command(engine_signal_t);
int esp_gnugo_pos_from_xy(int x, int y);
esp_gnugo_game_state_t *esp_gnugo_get_game_state(void);
esp_gnugo_state_t esp_gnugo_get_state(void);
void esp_gnugo_dump_sgf(char *sgfname);
char *esp_gnugo_send_gtp(const char *cmd);

/* ------------------------------------------------------------------ *
 *  Thread-safe engine context for Focus UI integration                *
 * ------------------------------------------------------------------ */

#define ENGINE_STATUS_STOPPED  0
#define ENGINE_STATUS_STARTING 1
#define ENGINE_STATUS_READY    2
#define ENGINE_STATUS_THINKING 3

typedef struct {
    esp_gnugo_game_init_t init_params;
    esp_gnugo_game_state_t state_snapshot;  /* engine writes, UI reads */
    volatile int state_ready;               /* flag: new state available */
    engine_signal_t command_buffer;          /* UI writes, engine reads */
    volatile int command_ready;             /* flag: command available */
    volatile int engine_status;             /* ENGINE_STATUS_* */
    volatile int quit_requested;
    int player_is_white_out;               /* set by engine after start */
} engine_context_t;

void go_engine_thread_main(engine_context_t *ctx);

#endif
