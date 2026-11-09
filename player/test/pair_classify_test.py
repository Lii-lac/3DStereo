# Host test of connect.c's pairing helpers (curl error / HTTP status -> plain kind, Date header parsing):
#   python -I player/test/pair_classify_test.py player/source/connect.c player/source/connect.h out.c
#   gcc -fsanitize=address,undefined out.c -o t && ./t        (the devkitarm docker image has gcc)
import sys
c = open(sys.argv[1], 'rb').read().decode('utf-8').replace('\r\n', '\n')
h = open(sys.argv[2], 'rb').read().decode('utf-8').replace('\r\n', '\n')
enum = h[h.index('enum { PZ_NONE'):h.index('\n', h.index('enum { PZ_NONE')) + 1]
mon = c[c.index('static int mon(const char *m) {'):c.index('static int64_t days_from_civil')]
dfc = c[c.index('static int64_t days_from_civil'):c.index('// "Date: Thu, 05 Nov 2026')]
pk = c[c.index('static int pair_kind(CURLcode ce, long http) {'):c.index('// "Date: Thu, 05 Nov 2026 18:30:00 GMT" -> unix seconds')]
pd = c[c.index('static int64_t parse_http_date(const char *s) {'):c.index('static int64_t pr_date_s;')]
out = r'''
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
typedef int CURLcode;
enum { CURLE_OK = 0, CURLE_COULDNT_RESOLVE_PROXY = 5, CURLE_COULDNT_RESOLVE_HOST = 6, CURLE_COULDNT_CONNECT = 7, CURLE_OPERATION_TIMEDOUT = 28,
       CURLE_SSL_CONNECT_ERROR = 35, CURLE_GOT_NOTHING = 52, CURLE_SEND_ERROR = 55, CURLE_RECV_ERROR = 56, CURLE_SSL_CERTPROBLEM = 58,
       CURLE_PEER_FAILED_VERIFICATION = 60, CURLE_SSL_CACERT_BADFILE = 77, CURLE_URL_MALFORMAT = 3 };
''' + enum + mon + dfc + pk + pd + r'''
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
	CHECK(pair_kind(CURLE_COULDNT_RESOLVE_HOST, 0) == PZ_DNS);
	CHECK(pair_kind(CURLE_COULDNT_CONNECT, 0) == PZ_CONNECT);
	CHECK(pair_kind(CURLE_OPERATION_TIMEDOUT, 0) == PZ_TIMEOUT);
	CHECK(pair_kind(CURLE_PEER_FAILED_VERIFICATION, 0) == PZ_CERT);   /* the 2052 case in the log */
	CHECK(pair_kind(CURLE_SSL_CONNECT_ERROR, 0) == PZ_CERT);
	CHECK(pair_kind(CURLE_RECV_ERROR, 0) == PZ_DROPPED);
	CHECK(pair_kind(CURLE_GOT_NOTHING, 0) == PZ_DROPPED);
	CHECK(pair_kind(CURLE_URL_MALFORMAT, 0) == PZ_UNKNOWN);
	CHECK(pair_kind(CURLE_OK, 429) == PZ_BUSY);
	CHECK(pair_kind(CURLE_OK, 503) == PZ_SERVER);
	CHECK(pair_kind(CURLE_OK, 400) == PZ_REFUSED);
	CHECK(pair_kind(CURLE_OK, 403) == PZ_REFUSED);
	CHECK(pair_kind(CURLE_PEER_FAILED_VERIFICATION, 200) == PZ_REFUSED);   /* an HTTP code wins over a stale curl code */
	/* Spotify's Date header: 5 Nov 2026 18:30:00 GMT = 1793903400 */
	CHECK(parse_http_date(" Thu, 05 Nov 2026 18:30:00 GMT\r\n") == 1793903400LL);
	CHECK(parse_http_date(" Fri, 31 Jul 2052 15:09:56 GMT") == 2606051396LL);
	CHECK(parse_http_date(" garbage") == -1);
	CHECK(parse_http_date(" Mon, 01 Foo 2026 00:00:00 GMT") == -1);
	/* the clock rule: more than 36 hours from the real time is wrong, a time zone or an AM/PM slip is not */
	long tol = 36 * 3600;
	CHECK(!(12 * 3600 > tol));
	CHECK(!(15 * 3600 > tol));
	CHECK(2606051396LL - 1793903400LL > tol);   /* 2052 vs 2026 */
	CHECK(3 * 86400 > tol);
	printf(fails ? "FAILED %d\n" : "all ok\n", fails);
	return fails != 0;
}
'''
open(sys.argv[3], 'w', newline='\n').write(out)
