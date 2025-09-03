#ifndef _ESP_WRAPPER_H
#define _ESP_WRAPPER_H
#include "stdint.h"
#include "stdbool.h"

#define GRID_EMPTY 0
#define GRID_WHITE 1
#define GRID_BLACK 2
#define GRID_STRANGE 3

#define GRID_DEAD_WHITE 11
#define GRID_DEAD_BLACK 12

#define GRID_HOSHI 10

typedef enum {
    ESP_GNUGO_STATE_NOT_STARTED,
    ESP_GNUGO_STATE_WAITING_FOR_PLAYER,
    ESP_GNUGO_STATE_WAITING_FOR_CPU,
    ESP_GNUGO_STATE_GAME_OVER,
} esp_gnugo_state_t;

typedef enum {
    ESP_GNUGO_MOVE,
    ESP_GNUGO_PASS,
    ESP_GNUGO_RESIGN,
    ESP_GNUGO_UNDO
} esp_gnugo_move_type_t;

// Events that may include animations
typedef enum {
    ESP_GNUGO_EVENT_PLAY,
    ESP_GNUGO_EVENT_ILLEGAL,
    ESP_GNUGO_EVENT_CAPTURE,
    ESP_GNUGO_EVENT_PASS,
    ESP_GNUGO_EVENT_WIN,
    ESP_GNUGO_EVENT_RESIGN,
    ESP_GNUGO_EVENT_UNDO,
    ESP_GNUGO_EVENT_RESTART
} esp_gnugo_event_t;

typedef struct {
    uint8_t board[9][9];
    char white_last_move[20];
    char black_last_move[20];
    float score;
    int black_captured;
    int white_captured;
    int move_number;
    int white_turn;
    int level;
    esp_gnugo_state_t state;
    esp_gnugo_event_t last_event;
} esp_gnugo_game_state_t;
typedef void (*board_update_callback)(const esp_gnugo_game_state_t*);

typedef struct {
    bool player_is_white;
    bool undo_allowed;
    bool autolevel;
    int start_level;
    float komi;
    int random_seed;
    int memory_mb;
    int requested_handicap;
    char* sgftree;
    board_update_callback update_callback;
} esp_gnugo_game_init_t;

void esp_gnugo_set_level(int level);
void esp_gnugo_start(esp_gnugo_game_init_t);
void esp_gnugo_get_computer_move();
int esp_gnugo_set_player_move(char *);
int esp_gnugo_set_player_move_int(int);
int esp_gnugo_pos_from_xy(int x, int y);


#endif