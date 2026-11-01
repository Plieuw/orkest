/* The SEQ menu's scales: the notes in an octave from the root (bit k: k semitones up). ARP keeps its phrases in the
 * chosen scale (orkest_player.c scale_snap); scale 0 is off. */
#define ORK_NSCALE 16u
#ifdef ORK_SCALE_MASKS
static FM1_AUDIO_DATA const uint16_t SCALE_MASK[ORK_NSCALE] = {
#define S(a, b, c, d, e, f, g) ((1u << (a)) | (1u << (b)) | (1u << (c)) | (1u << (d)) | (1u << (e)) | (1u << (f)) | (1u << (g)))
    0x0FFFu,                  /* OFF (every note) */
    S(0, 2, 4, 5, 7, 9, 11),  /* MAJOR */
    S(0, 2, 3, 5, 7, 8, 10),  /* MINOR (natural) */
    S(0, 2, 3, 5, 7, 8, 11),  /* HARMONIC MINOR */
    S(0, 2, 3, 5, 7, 9, 11),  /* MELODIC MINOR */
    S(0, 2, 3, 5, 7, 9, 10),  /* DORIAN */
    S(0, 1, 3, 5, 7, 8, 10),  /* PHRYGIAN */
    S(0, 2, 4, 6, 7, 9, 11),  /* LYDIAN */
    S(0, 2, 4, 5, 7, 9, 10),  /* MIXOLYDIAN */
    S(0, 1, 3, 5, 6, 8, 10),  /* LOCRIAN */
    S(0, 2, 4, 7, 9, 0, 0),   /* MAJOR PENTATONIC */
    S(0, 3, 5, 7, 10, 0, 0),  /* MINOR PENTATONIC */
    S(0, 3, 5, 6, 7, 10, 0),  /* BLUES */
    S(0, 2, 4, 6, 8, 10, 0),  /* WHOLE TONE */
    S(0, 2, 3, 6, 7, 8, 11),  /* HUNGARIAN MINOR */
    S(0, 1, 4, 5, 7, 8, 10),  /* PHRYGIAN DOMINANT */
#undef S
};
#endif
#ifdef ORK_SCALE_NAMES
static const char *const SCALE_NAME[ORK_NSCALE] = {
    "OFF", "MAJOR", "MINOR", "HARM MINOR", "MEL MINOR", "DORIAN", "PHRYGIAN", "LYDIAN", "MIXOLYDIAN", "LOCRIAN",
    "MAJOR PENTA", "MINOR PENTA", "BLUES", "WHOLE TONE", "HUNGARIAN", "PHRYG DOM"};
#endif
