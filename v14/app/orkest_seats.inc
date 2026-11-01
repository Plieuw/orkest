/* The GLO menu's concert seating (from the hall, the usual modern layout): first violins far left, violas and celli
 * right of centre, basses far right; harp left; woodwinds in the middle (flutes and clarinets a little left, oboes and
 * bassoons a little right); horns left behind them, trumpets, trombones and tuba right; timpani at the back in the
 * middle, the glockenspiel right. Pan -64 (left) .. +63 (right), at a spread of 100 %. */
static ORK_SEAT_DATA const struct { char name[10]; int8_t pan; } SEAT[] = {
    {"VIOLINS", -38}, {"SOLO", -24}, {"TREMOLO", -36}, {"SPICCATO", -38}, {"PIZZICATO", -30}, {"VIOLAS", 10},
    {"CELLI", 26}, {"BASSES", 42}, {"HARP", -48}, {"FLUTE", -10}, {"OBOE", 6}, {"CLARINET", -8}, {"BASSOON", 10},
    {"HORN", -24}, {"TRUMPET", 12}, {"TROMBONE", 26}, {"TUBA", 34}, {"TIMPANI", 0}, {"GLOCK", 36}};
static ORK_SEAT_CODE int32_t seat_pan(const char *name)   /* the instrument's place (0: unknown, the middle) */
{
    unsigned i, k;
    for (i = 0; i < sizeof SEAT / sizeof SEAT[0]; i++) {
        for (k = 0; SEAT[i].name[k] && name[k] == SEAT[i].name[k]; k++)
            ;
        if (!SEAT[i].name[k])
            return SEAT[i].pan;
    }
    return 0;
}
