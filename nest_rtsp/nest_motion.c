#include "nest_motion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CELL_COUNT (MOTION_COLUMNS * MOTION_ROWS)
#define MAX_LUMA_DELTA 255
#define IR_SETTLE_MS 1200

static void sample_cells(uint8_t *cells, const uint8_t *y, unsigned width,
                         unsigned height, unsigned pitch) {
    unsigned ox = width / (4 * MOTION_COLUMNS);
    unsigned oy = height / (4 * MOTION_ROWS);
    for (unsigned row = 0; row < MOTION_ROWS; row++) {
        unsigned py = (2 * row + 1) * height / (2 * MOTION_ROWS);
        const uint8_t *top = y + (py - oy) * pitch;
        const uint8_t *bottom = y + (py + oy) * pitch;
        for (unsigned col = 0; col < MOTION_COLUMNS; col++) {
            unsigned px = (2 * col + 1) * width / (2 * MOTION_COLUMNS);
            cells[row * MOTION_COLUMNS + col] =
                (top[px - ox] + top[px + ox] + bottom[px - ox] + bottom[px + ox] + 2) / 4;
        }
    }
}

static int median_delta(const unsigned *hist, unsigned count) {
    unsigned seen = 0;
    for (unsigned i = 0; i <= 2 * MAX_LUMA_DELTA; i++) {
        seen += hist[i];
        if (seen > count / 2) return (int)i - MAX_LUMA_DELTA;
    }
    return 0;
}

static unsigned changed_cells(const uint8_t *old, const uint8_t *new, bool night) {
    unsigned hist[2][2 * MAX_LUMA_DELTA + 1] = {{0}};
    unsigned counts[2] = {0};
    for (unsigned i = 0; i < CELL_COUNT; i++) {
        unsigned group = old[i] >= 96;
        int delta = (int)new[i] - old[i];
        hist[group][delta + MAX_LUMA_DELTA]++;
        counts[group]++;
    }
    /* Estimate the common exposure shift separately in shadows and highlights. */
    int shift[2] = {median_delta(hist[0], counts[0]), median_delta(hist[1], counts[1])};
    if (counts[0] < CELL_COUNT / 16) shift[0] = shift[1];
    if (counts[1] < CELL_COUNT / 16) shift[1] = shift[0];

    bool marked[CELL_COUNT];
    int threshold = night ? 20 : 16;
    for (unsigned i = 0; i < CELL_COUNT; i++) {
        int delta = (int)new[i] - old[i] - shift[old[i] >= 96];
        marked[i] = abs(delta) >= threshold;
    }

    unsigned changed = 0;
    for (unsigned i = 0; i < CELL_COUNT; i++) {
        if (!marked[i]) continue;
        unsigned col = i % MOTION_COLUMNS;
        if ((col > 0 && marked[i - 1]) ||
            (col + 1 < MOTION_COLUMNS && marked[i + 1]) ||
            (i >= MOTION_COLUMNS && marked[i - MOTION_COLUMNS]) ||
            (i + MOTION_COLUMNS < CELL_COUNT && marked[i + MOTION_COLUMNS]))
            changed++;
    }
    return changed;
}

unsigned motion_sample(motion_state_t *s, const uint8_t *y, unsigned width,
                       unsigned height, unsigned pitch, uint32_t now, bool night) {
    uint8_t cells[CELL_COUNT];
    sample_cells(cells, y, width, height, pitch);

    if (s->have_frame && s->night != night) {
        s->suppressed = true;
        s->settle_until = now + IR_SETTLE_MS;
    }
    if (s->suppressed && (int32_t)(now - s->settle_until) >= 0)
        s->suppressed = false;

    unsigned changed = 0;
    if (s->have_frame && !s->suppressed && now - s->sampled_at <= 1000)
        changed = changed_cells(s->previous, cells, night);
    memcpy(s->previous, cells, sizeof(cells));
    s->sampled_at = now;
    s->have_frame = true;
    s->night = night;

    if (changed >= MOTION_MIN_CELLS) {
        if (!s->candidate) s->candidate_since = now;
        s->candidate = true;
        if (s->active || now - s->candidate_since >= MOTION_ON_MS) {
            s->active = true;
            s->last_motion = now;
        }
    } else {
        s->candidate = false;
        if (s->active && now - s->last_motion >= MOTION_OFF_MS) s->active = false;
    }
    return changed;
}

bool motion_publish(bool active) {
    char path[] = NEST_MOTION_PATH ".XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return false;
    char data[40];
    int len = snprintf(data, sizeof(data), "%d %ld\n", active ? 1 : 0, (long)time(NULL));
    bool written = write(fd, data, (size_t)len) == len;
    close(fd);
    if (written && rename(path, NEST_MOTION_PATH) == 0) return true;
    unlink(path);
    return false;
}
