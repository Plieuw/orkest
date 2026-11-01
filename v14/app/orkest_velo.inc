/* The ARP's velocity patterns, apart from the phrases: each note the phrase plays takes the next step (100 = the
 * key's velocity), so a pattern of another length than the phrase shifts against it. Pattern 0 is the phrase's own. */
#define ORK_NVELO 22u
#ifdef ORK_VELO_STEPS
static FM1_AUDIO_DATA const uint8_t VELO_STEP[] = {
    100,                                                        /* 0 PHRASE (not used: its own velocities) */
    100,                                                        /* 1 FLAT */
    120, 70,                                                    /* 2 ACCENT 2 */
    120, 70, 70,                                                /* 3 ACCENT 3 */
    125, 70, 90, 70,                                            /* 4 ACCENT 4 */
    120, 70, 70, 105, 70,                                       /* 5 ACCENT 5 */
    120, 70, 70, 105, 70, 105, 70,                              /* 6 ACCENT 7 */
    120, 70, 70, 120, 70, 70, 120, 70,                          /* 7 3 3 2 */
    70, 120,                                                    /* 8 OFFBEAT */
    120, 60, 60, 120, 60, 60, 120, 60, 60, 60, 120, 60, 120, 60, 60, 60,   /* 9 CLAVE (son 3-2) */
    115, 75, 85,                                                /* 10 TRIPLET */
    110, 45, 45, 95, 45, 45, 45, 45,                            /* 11 GHOST */
    110, 55,                                                    /* 12 ECHO */
    60, 80, 100, 120,                                           /* 13 RAMP UP 4 */
    50, 60, 70, 80, 90, 100, 110, 125,                          /* 14 RAMP UP 8 */
    120, 100, 80, 60,                                           /* 15 RAMP DOWN 4 */
    125, 110, 100, 90, 80, 70, 60, 50,                          /* 16 RAMP DOWN 8 */
    60, 80, 100, 120, 120, 100, 80, 60,                         /* 17 SWELL 8 */
    90, 110, 125, 110, 90, 70, 55, 70,                          /* 18 WAVE 8 */
    100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 125, /* 19 LAST OF 12 */
    105, 70, 120, 85, 60, 115, 90, 75, 125, 65, 95, 110, 80, 100, 70, 118,   /* 20 SCATTER */
    127, 40,                                                    /* 21 LOUD SOFT */
};
static FM1_AUDIO_DATA const struct { uint8_t first, n; } VELO[ORK_NVELO] = {
    {0, 1}, {1, 1}, {2, 2}, {4, 3}, {7, 4}, {11, 5}, {16, 7}, {23, 8}, {31, 2}, {33, 16}, {49, 3}, {52, 8}, {60, 2},
    {62, 4}, {66, 8}, {74, 4}, {78, 8}, {86, 8}, {94, 8}, {102, 12}, {114, 16}, {130, 2},
};
#endif
#ifdef ORK_VELO_NAMES
static const char *const VELO_NAME[ORK_NVELO] = {
    "PHRASE", "FLAT", "ACCENT 2", "ACCENT 3", "ACCENT 4", "ACCENT 5", "ACCENT 7", "3 3 2", "OFFBEAT", "CLAVE",
    "TRIPLET", "GHOST", "ECHO", "RAMP UP 4", "RAMP UP 8", "RAMP DOWN 4", "RAMP DOWN 8", "SWELL 8", "WAVE 8",
    "LAST OF 12", "SCATTER", "LOUD SOFT"};
#endif
