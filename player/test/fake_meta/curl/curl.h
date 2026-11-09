// the few curl names meta.c and its headers use, for test_meta.c
#pragma once
#include <stdlib.h>
#include <string.h>
typedef void CURL;
typedef int CURLcode;
static inline char *curl_easy_escape(CURL *h, const char *s, int n) { (void)h; (void)n; return strdup(s); }
static inline void curl_free(void *p) { free(p); }
static inline void curl_easy_cleanup(CURL *h) { (void)h; }
// for test_lists.c (spotify.c)
enum { CURLE_OK, CURLE_SSL_CONNECT_ERROR = 35, CURLE_PEER_FAILED_VERIFICATION = 60 };
