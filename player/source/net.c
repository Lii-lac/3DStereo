#include "net.h"
#include "crumb.h"
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DIR3DS "sdmc:/3ds/player"
#define CA     DIR3DS "/cacert.pem"
#define SOC_SIZE 0x100000

extern const u8 cacert_pem[], cacert_pem_end[];   // cacert.s

static LightLock lk;
static bool lk_ok, ready;
static u32 *soc_buf;

// The CA bundle rides inside the app and goes onto the card the first time (or when a build
// brings a newer one), since curl wants it as a file.
static void ca_write(void) {
	size_t n = cacert_pem_end - cacert_pem;
	struct stat st;
	if (!stat(CA, &st) && (size_t)st.st_size == n) return;
	mkdir("sdmc:/3ds", 0777);
	mkdir(DIR3DS, 0777);
	FILE *f = fopen(CA, "wb");
	if (!f) { crumb("net: can't write " CA); return; }
	fwrite(cacert_pem, 1, n, f);
	fclose(f);
	crumb("net: wrote " CA " (%u bytes)", (unsigned)n);
}

bool net_wifi(void) {
	u32 wifi = 0;
	return ready && R_SUCCEEDED(ACU_GetWifiStatus(&wifi)) && wifi;
}

// Every certificate has dates it is good between, so a 3DS whose clock is years off (it said 2052 once) gets "SSL peer
// certificate not OK" from every HTTPS server. 0 when the clock is believable, else the year it claims.
int net_clock_year(void) {
	time_t t = time(NULL);
	struct tm tmv;
	localtime_r(&t, &tmv);
	int y = tmv.tm_year + 1900;
	return y < 2025 || y > 2036 ? y : 0;
}

void net_init(void) { if (!lk_ok) { LightLock_Init(&lk); lk_ok = true; } }

bool net_up(char *err, int cap) {
	if (!lk_ok) { if (err) snprintf(err, cap, "the network won't start"); return false; }
	LightLock_Lock(&lk);
	if (!ready) {
		soc_buf = memalign(0x1000, SOC_SIZE);
		if (soc_buf && R_SUCCEEDED(socInit(soc_buf, SOC_SIZE))) {
			acInit();
			curl_global_init(CURL_GLOBAL_DEFAULT);
			ca_write();
			ready = true;
		} else {
			free(soc_buf);
			soc_buf = NULL;
		}
	}
	LightLock_Unlock(&lk);
	if (!ready) { if (err) snprintf(err, cap, "the network won't start"); return false; }
	if (!net_wifi()) { if (err) snprintf(err, cap, "no Wi-Fi"); return false; }
	return true;
}

CURL *net_handle(void) { return ready ? curl_easy_init() : NULL; }

void net_exit(void) {
	if (!ready) return;
	curl_global_cleanup();
	acExit();
	socExit();
	free(soc_buf);
	soc_buf = NULL;
	ready = false;
}

static size_t on_body(char *p, size_t sz, size_t n, void *ud) {
	Buf *b = ud;
	size_t add_n = sz * n;
	if (b->n + add_n > (8u << 20)) return 0;   // nothing we ask for is this big
	char *g = realloc(b->d, b->n + add_n + 1);
	if (!g) return 0;
	memcpy(g + b->n, p, add_n);
	b->d = g;
	b->n += add_n;
	b->d[b->n] = 0;
	return add_n;
}

static int on_xfer(void *p, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
	(void)a; (void)b; (void)c; (void)d;
	return p && *(volatile bool *)p;   // nonzero aborts
}

long net_req(CURL *h, const char *method, const char *url, const char *form, const char *auth,
             Buf *out, long *retry_after, volatile bool *quit, CURLcode *cerr) {
	free(out->d);
	*out = (Buf){0};
	struct curl_slist *hdr = NULL;
	if (auth) hdr = curl_slist_append(hdr, auth);
	bool bodyless = !form && strcmp(method, "GET");
	if (bodyless) hdr = curl_slist_append(hdr, "Content-Length: 0");
	if (form && form[0] == '{') hdr = curl_slist_append(hdr, "Content-Type: application/json");   // the Web API's bodies
	curl_easy_reset(h);
	curl_easy_setopt(h, CURLOPT_URL, url);
	curl_easy_setopt(h, CURLOPT_CAINFO, CA);
	curl_easy_setopt(h, CURLOPT_USERAGENT, "3DStereo");
	curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, out);
	curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_xfer);
	curl_easy_setopt(h, CURLOPT_XFERINFODATA, (void *)quit);
	if (form) curl_easy_setopt(h, CURLOPT_POSTFIELDS, form);
	if (bodyless || (form && strcmp(method, "POST"))) curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method);   // a PUT with a body too
	u64 t0 = osGetTime();
	CURLcode rc = curl_easy_perform(h);
	curl_slist_free_all(hdr);
	if (cerr) *cerr = rc;
	if (rc != CURLE_OK) {
		crumb("net: %s %.80s: %s", method, url, curl_easy_strerror(rc));
		return -1;
	}
	long code = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
	if (retry_after) {
		curl_off_t ra = 0;
		*retry_after = curl_easy_getinfo(h, CURLINFO_RETRY_AFTER, &ra) == CURLE_OK ? (long)ra : 0;
	}
	crumb("net: %s %.60s %ld, %u bytes, %llu ms", method, url, code, (unsigned)out->n, osGetTime() - t0);
	return code;
}
