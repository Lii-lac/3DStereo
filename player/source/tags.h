// Tags, length and format details straight from the file headers, no decoder: FLAC's metadata
// blocks, ID3v2/ID3v1 and the first MP3 frame, the Ogg Vorbis and Opus header packets, and
// WAV's RIFF chunks. A scan reads a few KB a song at most.
#pragma once
#include "library.h"

bool tags_read(const char *path, int fmt, Track *t, u64 *size);   // false: not a file we can play; size: the file's
extern int tags_card_reads;   // reads from the card so far, for the scan's log line
