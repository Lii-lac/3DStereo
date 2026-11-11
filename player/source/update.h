// Settings > Update: the newest release on GitHub (Lii-lac/3DStereo), downloaded and put in place
// of this one. Started from a CIA, the release's .cia goes straight into the system's installer as
// it comes in; started from the Homebrew Launcher, its .3dsx replaces the file we were started from.
#pragma once
#include <3ds.h>

enum { U_IDLE, U_CHECK, U_GET, U_FINISH, U_CURRENT, U_DONE, U_FAIL };

void upd_init(void);   // on the main thread, before upd_start
// Look at the latest release and, if it's newer than VERSION, download and install it. Does
// nothing while a run is going.
void upd_start(void);
bool upd_busy(void);
bool upd_done(void);   // a newer version is installed: the next launch is it
// The Settings row's second line, as things stand.
void upd_line(char *out, int cap);
// Where a run is (U_*), and in *pct the download's percent: -1 until its size is known, and on
// U_FAIL whatever it had reached, for the row to drain from.
int upd_state(int *pct);
// The restart into the new version leaves a note on the card; the next launch takes it (true
// once) and says so.
void upd_mark(void);
bool upd_just_updated(void);
void upd_exit(void);   // stops a run (a half-done CIA install is cancelled) and joins the thread
