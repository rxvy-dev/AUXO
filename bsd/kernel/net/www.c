/* The browser's loader thread ("www"): takes one URL at a time from the Chrono browser and
 * fetches it with http_get. Networking and TLS run here, on a thread built without SSE; the
 * desktop thread only reads the finished buffer. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "net.h"
#include "www.h"

struct www www;
static int started;

static void progress(size_t n) { www.bytes = n; }

static void www_thread(uintptr_t arg)
{
    (void)arg;
    for (;;) {
        while (www.state != WWW_REQUESTED) wait_tick();
        www.state = WWW_LOADING;
        www.bytes = 0;
        char url[sizeof www.url];
        strlcpy(url, www.url, sizeof url);
        if (!net_ready()) net_wait_ready(10000);
        http_get(url, www.buf, www.cap, &www.res, progress);
        www.gen++;
        www.state = WWW_DONE;
    }
}

int www_init(void)
{
    if (started) return 0;
    www.cap = WWW_BUF;
    uint64_t pg = pmm_alloc_contig(WWW_BUF / PAGE_SIZE);
    if (!pg) return -1;
    www.buf = (char *)(uintptr_t)pg;
    started = 1;
    return rs_spawn("www", www_thread, 0) < 0 ? -1 : 0;
}

void www_request(const char *url)
{
    if (www.state == WWW_LOADING) return;    /* one at a time; the browser shows "loading" */
    strlcpy(www.url, url, sizeof www.url);
    www.state = WWW_REQUESTED;
}
