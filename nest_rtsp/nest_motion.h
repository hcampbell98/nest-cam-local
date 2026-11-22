#ifndef NEST_MOTION_H
#define NEST_MOTION_H

#include <stdbool.h>
#include <stdint.h>

#define NEST_MOTION_PATH "/tmp/nest_motion.state"
#define MOTION_COLUMNS 48
#define MOTION_ROWS 32
#define MOTION_MIN_CELLS 16
#define MOTION_ON_MS 160
#define MOTION_OFF_MS 2500

typedef struct {
    uint8_t previous[MOTION_COLUMNS * MOTION_ROWS];
    uint32_t sampled_at;
    uint32_t settle_until;
    uint32_t last_motion;
    uint32_t candidate_since;
    bool have_frame;
    bool suppressed;
    bool night;
    bool candidate;
    bool active;
} motion_state_t;

/* Returns the count of changed cells with a changed neighbour. */
unsigned motion_sample(motion_state_t *state, const uint8_t *y, unsigned width,
                       unsigned height, unsigned pitch, uint32_t now, bool night);
bool motion_publish(bool active);

#endif
