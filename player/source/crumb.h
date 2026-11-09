// Breadcrumbs for a freeze: each line is appended and the file closed again, so whatever was
// written before the console locked up is still on the card after a reboot.
#pragma once
void crumb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// The same, from a thread that must never wait on the card (the audio worker): held in memory and
// written by crumb_flush, which the main thread calls every frame
void crumb_soon(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void crumb_flush(void);
// For the main thread's own lines in the middle of drawing (slow frames): a write to the card waits in line behind the
// workers' reads and writes, so these are held in memory and written by crumb_flush_late, which only a background thread
// calls (and the main thread at exit)
void crumb_late(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void crumb_flush_late(void);
