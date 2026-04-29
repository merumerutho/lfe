#include "lfe_braids_random.h"

/* Seeded to Braids' initial default so the first call to GetWord()
 * returns the same value as the upstream module out of reset.
 * Callers that want determinism should call braids_random_seed()
 * explicitly before rendering. */
uint32_t braids_random_state = 0x21;
