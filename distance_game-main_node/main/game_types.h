#ifndef GAME_TYPES_H
#define GAME_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define GAME_ROUNDS_PER_MATCH 3

typedef struct {
    int target_number;
    int p1_measurement;
    int p2_measurement;
    int p1_distance;
    int p2_distance;
    int p1_score;
    int p2_score;
    bool valid;
} game_round_result_t;

typedef struct {
    game_round_result_t rounds[GAME_ROUNDS_PER_MATCH];
    int total_p1_score;
    int total_p2_score;
    bool aborted;
    bool completed;
} game_session_result_t;

#endif
