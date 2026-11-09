// Listens: a song counts once it's been heard for 30 seconds (half of it, if it's shorter than
// a minute), and every second actually heard adds up too. Kept by path in sdmc:/3ds/player/stats,
// so songs that leave the card keep their numbers for when they come back.
#pragma once
#include <3ds.h>

void stats_load(void);                                   // after library_scan
void stats_heard(int track, double pos, bool playing);   // every frame
u32 stats_plays(int track);
float stats_secs(int track);
u32 stats_total_plays(void);
double stats_total_secs(void);
bool stats_changed(void);   // a listen counted since the last time anyone asked
void stats_flush(void);     // before library_exit, so the last seconds reach the card
