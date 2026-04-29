/*
 * lfe_braids_excitation.h — exponential-decay excitation source.
 *
 * Ported from braids/excitation.h (MIT, (c) 2013 Emilie Gillet). Braids
 * uses this as the "strike" or "pluck" stimulus for the physical-model
 * shapes (PLUCKED, BOWED, BLOWN, FLUTED). One delay sample, one Q12
 * decay coefficient; the `Trigger` arms a one-shot impulse that bleeds
 * out across the decay envelope.
 *
 * Header-only: Process() is the inner loop of percussive voices and
 * benefits from inlining. State is owned by the caller.
 */

#ifndef LFE_BRAIDS_EXCITATION_H
#define LFE_BRAIDS_EXCITATION_H

#include <stdint.h>

typedef struct {
    uint32_t delay;      /* ticks before the impulse fires */
    uint32_t decay;      /* Q12 decay coefficient (0-4095)  */
    int32_t  counter;    /* remaining ticks to impulse, 0 = done */
    int32_t  state;      /* envelope accumulator */
    int32_t  level;      /* signed impulse magnitude */
} braids_excitation_t;

static inline void braids_excitation_init(braids_excitation_t *e)
{
    e->delay   = 0;
    e->decay   = 4093;
    e->counter = 0;
    e->state   = 0;
    e->level   = 0;
}

static inline void braids_excitation_set_delay(braids_excitation_t *e,
                                               uint16_t delay)
{
    e->delay = delay;
}

static inline void braids_excitation_set_decay(braids_excitation_t *e,
                                               uint16_t decay)
{
    e->decay = decay;
}

static inline void braids_excitation_trigger(braids_excitation_t *e,
                                             int32_t level)
{
    e->level   = level;
    e->counter = (int32_t)e->delay + 1;
}

static inline int braids_excitation_done(const braids_excitation_t *e)
{
    return e->counter == 0;
}

static inline int32_t braids_excitation_process(braids_excitation_t *e)
{
    e->state = (int32_t)((int64_t)e->state * (int64_t)e->decay >> 12);
    if (e->counter > 0) {
        --e->counter;
        if (e->counter == 0) {
            e->state += e->level < 0 ? -e->level : e->level;
        }
    }
    return e->level < 0 ? -e->state : e->state;
}

#endif /* LFE_BRAIDS_EXCITATION_H */
