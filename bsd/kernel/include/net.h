#ifndef AUXO_NET_H
#define AUXO_NET_H
/* AuxoBSD networking: Ethernet drivers, and an IPv4 stack (ARP, ICMP, UDP, DHCP, DNS, TCP).
 * The stack is protected by one lock; blocking calls poll the card while they wait, and the
 * "net" thread polls in the background (DHCP, ARP, TCP timers, ping replies). */
#include <stdint.h>
#include <stddef.h>

struct pci_dev;

/* a network card, filled in by its driver */
struct nic {
    const char *driver;            /* "e1000", "rtl8169" */
    char        model[48];
    uint8_t     mac[6];
    int  (*send)(struct nic *n, const void *frame, size_t len);   /* 0 = ok */
    int  (*recv)(struct nic *n, void *buf, size_t cap);           /* frame length, 0 if none */
    int  (*link)(struct nic *n);                                  /* 1 when the cable is up */
    void *priv;
};

int  e1000_probe(struct pci_dev *d, struct nic *n);      /* 0 if it took the card */
int  rtl8169_probe(struct pci_dev *d, struct nic *n);

/* net.c */
void net_init(void);                 /* find a card, start the net thread (DHCP) */
int  net_present(void);              /* a card was found */
int  net_ready(void);                /* we have an address */
const struct nic *net_nic(void);
const char *net_status(void);        /* one line: "10.0.2.15 via e1000 (Intel 82540EM)" or why not */
uint32_t net_ip(void), net_mask(void), net_gateway(void), net_dns(void);   /* host byte order */
void ip_fmt(uint32_t ip, char *buf, size_t n);
int  ip_parse(const char *s, uint32_t *ip);
int  net_resolve(const char *host, uint32_t *ip, int timeout_ms);           /* 0 = ok */
int  net_ping(uint32_t ip, uint16_t seq, int timeout_ms);                   /* rtt in ms, -1 */
int  net_wait_ready(int timeout_ms);

/* TCP client sockets */
int  tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);   /* socket or <0 */
int  tcp_send(int s, const void *buf, size_t len);              /* bytes queued, -1 */
int  tcp_recv(int s, void *buf, size_t cap, int timeout_ms);    /* >0 bytes, 0 closed, -1 error, -2 timeout */
void tcp_close(int s);

/* http.c: HTTP/1.1 and HTTPS (BearSSL) */
struct http_result {
    int    status;                   /* 200, 404 … ; <0 = failed (see error) */
    char   url[512];                 /* final URL after redirects */
    char   content_type[64];
    char   error[128];
    char  *body;
    size_t len;
};
/* GET url into buf (cap bytes). Follows up to 5 redirects. progress(bytes) may be NULL. */
int http_get(const char *url, char *buf, size_t cap, struct http_result *r, void (*progress)(size_t));
void tls_seed(void);                 /* entropy for BearSSL; called by net_init */

#endif
