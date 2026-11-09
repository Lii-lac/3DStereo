// The network, shared: sockets and curl are started once for the whole app, and each thread
// that talks to the internet keeps its own curl handle (so its connections are kept warm).
#pragma once
#include <3ds.h>
#include <curl/curl.h>
#include <stddef.h>

typedef struct { char *d; size_t n; } Buf;

void net_init(void);   // on the main thread, before any thread that uses the network starts
// Sockets, curl and the CA bundle on the card, once; then is Wi-Fi on? err gets why not.
bool net_up(char *err, int cap);
bool net_wifi(void);
int net_clock_year(void);   // 0 when the 3DS's clock is believable, else the year it says: TLS fails with a clock that far off
CURL *net_handle(void);   // a new handle (net_up first); curl_easy_cleanup it yourself
void net_exit(void);      // after every handle is gone

// One request. auth: a whole "Authorization: ..." line or NULL; form: a POST body or NULL;
// quit: set it and the transfer stops. The HTTP code, or -1 with *cerr set if none came.
long net_req(CURL *h, const char *method, const char *url, const char *form, const char *auth,
             Buf *out, long *retry_after, volatile bool *quit, CURLcode *cerr);
