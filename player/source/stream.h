// A song file read into memory ahead of the decoder by a thread of its own (stream.c): the decoder
// reads it as an ordinary FILE, and never waits on the card while the card is busy with something
// else. fclose it as usual; that stops its reader and frees its memory.
#pragma once
#include <3ds.h>
#include <stdio.h>

FILE *stream_open(const char *path);   // NULL: no such file, or no memory or thread for it
