/*
 * tun2socks_engine.c — native tun2socks relay with token-bucket shaping
 *
 * Reads raw IP packets from a TUN file descriptor, manages TCP and UDP
 * sessions via real kernel sockets (protected from VPN routing), and
 * applies per-direction bandwidth caps through two token-bucket rate
 * limiters.  A single-threaded epoll event loop drives all I/O.
 */

#include "tun2socks_engine.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <android/log.h>

#define TAG  "tun2socks"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* ================================================================== */
/*  Utility helpers                                                   */
/* ================================================================== */

/* set_nonblocking removed (unused) */

/* Internet checksum (RFC 1071) over @p len bytes. */
static uint16_t checksum_compute(const void *data, int len) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    for (int i = 0; i < len / 2; i++) sum += ntohs(p[i]);
    if (len & 1) sum += ((const uint8_t *)data)[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)(~sum & 0xFFFF));
}

/* Transport checksum with IPv4 pseudo-header. */
static uint16_t tcp_udp_csum_v4(uint32_t src, uint32_t dst,
                                 uint8_t proto,
                                 const void *segment, int seg_len) {
    uint32_t sum = 0;
    sum += (ntohl(src) >> 16) & 0xFFFF;
    sum +=  ntohl(src)        & 0xFFFF;
    sum += (ntohl(dst) >> 16) & 0xFFFF;
    sum +=  ntohl(dst)        & 0xFFFF;
    sum += proto;
    sum += (uint16_t)seg_len;
    const uint16_t *p = (const uint16_t *)segment;
    for (int i = 0; i < seg_len / 2; i++) sum += ntohs(p[i]);
    if (seg_len & 1) sum += ((const uint8_t *)segment)[seg_len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)(~sum & 0xFFFF));
}

/* Transport checksum with IPv6 pseudo-header. */
static uint16_t tcp_udp_csum_v6(const uint8_t src[16], const uint8_t dst[16],
                                 uint8_t proto,
                                 const void *segment, int seg_len) {
    uint32_t sum = 0;
    const uint16_t *s = (const uint16_t *)src;
    const uint16_t *d = (const uint16_t *)dst;
    for (int i = 0; i < 8; i++) { sum += ntohs(s[i]); sum += ntohs(d[i]); }
    sum += (uint16_t)seg_len;
    sum += proto;
    const uint16_t *p = (const uint16_t *)segment;
    for (int i = 0; i < seg_len / 2; i++) sum += ntohs(p[i]);
    if (seg_len & 1) sum += ((const uint8_t *)segment)[seg_len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)(~sum & 0xFFFF));
}

static int addr_eq(const ip_addr_t *a, const ip_addr_t *b) {
    if (a->version != b->version) return 0;
    if (a->version == 4) return a->a.v4 == b->a.v4;
    return memcmp(a->a.v6, b->a.v6, 16) == 0;
}

static uint32_t session_hash(const ip_addr_t *src, uint16_t sp,
                              const ip_addr_t *dst, uint16_t dp) {
    uint32_t h = (uint32_t)sp ^ ((uint32_t)dp << 16);
    if (src->version == 4) {
        h ^= src->a.v4 ^ dst->a.v4;
    } else {
        uint32_t *s32 = (uint32_t *)src->a.v6;
        uint32_t *d32 = (uint32_t *)dst->a.v6;
        h ^= s32[0] ^ s32[3] ^ d32[0] ^ d32[3];
    }
    return h % SESSION_TABLE_SIZE;
}

/* ================================================================== */
/*  JNI callback — VpnService.protect(fd)                             */
/* ================================================================== */

static int protect_socket(engine_t *e, int fd) {
    JNIEnv *env = NULL;
    int attached = 0;
    if ((*e->jvm)->GetEnv(e->jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*e->jvm)->AttachCurrentThread(e->jvm, &env, NULL) != JNI_OK) {
            LOGE("protect_socket: cannot attach JNI");
            return 0;
        }
        attached = 1;
    }
    jboolean ok = (*env)->CallBooleanMethod(env, e->vpn_ref,
                                             e->protect_mid, (jint)fd);
    if (attached) (*e->jvm)->DetachCurrentThread(e->jvm);
    return ok;
}

/* ================================================================== */
/*  TCP session management                                            */
/* ================================================================== */

static tcp_session_t *tcp_find(engine_t *e,
                                const ip_addr_t *src, uint16_t sp,
                                const ip_addr_t *dst, uint16_t dp) {
    uint32_t idx = session_hash(src, sp, dst, dp);
    for (tcp_session_t *s = e->tcp_table[idx]; s; s = s->next) {
        if (s->src_port == sp && s->dst_port == dp &&
            addr_eq(&s->src, src) && addr_eq(&s->dst, dst))
            return s;
    }
    return NULL;
}

static tcp_session_t *tcp_find_by_fd(engine_t *e, int fd) {
    for (int i = 0; i < SESSION_TABLE_SIZE; i++)
        for (tcp_session_t *s = e->tcp_table[i]; s; s = s->next)
            if (s->sock_fd == fd) return s;
    return NULL;
}

static void tcp_remove(engine_t *e, tcp_session_t *target) {
    uint32_t idx = session_hash(&target->src, target->src_port,
                                 &target->dst, target->dst_port);
    tcp_session_t **pp = &e->tcp_table[idx];
    while (*pp) {
        if (*pp == target) { *pp = target->next; break; }
        pp = &(*pp)->next;
    }
    epoll_ctl(e->epoll_fd, EPOLL_CTL_DEL, target->sock_fd, NULL);
    close(target->sock_fd);
    if (target->pending_data) free(target->pending_data);
    free(target);
}

/* Create a real TCP socket, protect it, start non-blocking connect. */
static tcp_session_t *tcp_create(engine_t *e,
                                  const ip_addr_t *src, uint16_t sp,
                                  const ip_addr_t *dst, uint16_t dp,
                                  uint32_t client_isn) {
    tcp_session_t *s = calloc(1, sizeof(tcp_session_t));
    if (!s) return NULL;

    s->src       = *src;
    s->dst       = *dst;
    s->src_port  = sp;
    s->dst_port  = dp;
    s->client_seq = client_isn + 1;
    s->our_seq   = (uint32_t)(time(NULL) * 1000 + sp) ^ dp;
    s->state     = TCP_SYN_SENT;
    s->last_active = time(NULL);

    int family = (dst->version == 4) ? AF_INET : AF_INET6;
    s->sock_fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (s->sock_fd < 0) { free(s); return NULL; }

    if (!protect_socket(e, s->sock_fd)) {
        LOGW("tcp_create: protect failed");
        close(s->sock_fd); free(s); return NULL;
    }

    if (family == AF_INET) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = dst->a.v4;
        sa.sin_port        = dp;
        connect(s->sock_fd, (struct sockaddr *)&sa, sizeof(sa));
    } else {
        struct sockaddr_in6 sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin6_family = AF_INET6;
        memcpy(&sa.sin6_addr, dst->a.v6, 16);
        sa.sin6_port   = dp;
        connect(s->sock_fd, (struct sockaddr *)&sa, sizeof(sa));
    }
    /* connect returns -1 / EINPROGRESS — expected for non-blocking */

    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLOUT;
    ev.data.fd = s->sock_fd;
    epoll_ctl(e->epoll_fd, EPOLL_CTL_ADD, s->sock_fd, &ev);

    uint32_t idx = session_hash(src, sp, dst, dp);
    s->next = e->tcp_table[idx];
    e->tcp_table[idx] = s;

    return s;
}

/* ================================================================== */
/*  UDP session management                                            */
/* ================================================================== */

static udp_session_t *udp_find(engine_t *e,
                                const ip_addr_t *src, uint16_t sp,
                                const ip_addr_t *dst, uint16_t dp) {
    uint32_t idx = session_hash(src, sp, dst, dp);
    for (udp_session_t *s = e->udp_table[idx]; s; s = s->next)
        if (s->src_port == sp && s->dst_port == dp &&
            addr_eq(&s->src, src) && addr_eq(&s->dst, dst))
            return s;
    return NULL;
}

static udp_session_t *udp_find_by_fd(engine_t *e, int fd) {
    for (int i = 0; i < SESSION_TABLE_SIZE; i++)
        for (udp_session_t *s = e->udp_table[i]; s; s = s->next)
            if (s->sock_fd == fd) return s;
    return NULL;
}

/* udp_remove removed (unused, cleanup handles removal) */

static udp_session_t *udp_create(engine_t *e,
                                  const ip_addr_t *src, uint16_t sp,
                                  const ip_addr_t *dst, uint16_t dp) {
    udp_session_t *s = calloc(1, sizeof(udp_session_t));
    if (!s) return NULL;

    s->src = *src;  s->dst = *dst;
    s->src_port = sp; s->dst_port = dp;
    s->last_active = time(NULL);

    int family = (dst->version == 4) ? AF_INET : AF_INET6;
    s->sock_fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (s->sock_fd < 0) { free(s); return NULL; }

    if (!protect_socket(e, s->sock_fd)) {
        close(s->sock_fd); free(s); return NULL;
    }

    /* Connect the UDP socket so recv() only returns packets from this peer. */
    if (family == AF_INET) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = dst->a.v4;
        sa.sin_port        = dp;
        connect(s->sock_fd, (struct sockaddr *)&sa, sizeof(sa));
    } else {
        struct sockaddr_in6 sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin6_family = AF_INET6;
        memcpy(&sa.sin6_addr, dst->a.v6, 16);
        sa.sin6_port   = dp;
        connect(s->sock_fd, (struct sockaddr *)&sa, sizeof(sa));
    }

    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = s->sock_fd;
    epoll_ctl(e->epoll_fd, EPOLL_CTL_ADD, s->sock_fd, &ev);

    uint32_t idx = session_hash(src, sp, dst, dp);
    s->next = e->udp_table[idx];
    e->udp_table[idx] = s;
    return s;
}

/* ================================================================== */
/*  Packet construction — write IP + transport to TUN                 */
/* ================================================================== */

/* Build an IPv4 TCP segment and write to TUN.
 * Addresses are swapped: remote → local (response direction). */
static void write_tcp_v4(engine_t *e, tcp_session_t *s,
                          uint8_t flags,
                          const uint8_t *payload, int pay_len) {
    const int ip_len  = 20;
    const int tcp_len = 20;
    int total = ip_len + tcp_len + pay_len;
    uint8_t pkt[MAX_PACKET_SIZE];
    if (total > (int)sizeof(pkt)) return;
    memset(pkt, 0, (size_t)total);

    /* --- IPv4 header ------------------------------------------------ */
    ipv4_hdr_t *ip = (ipv4_hdr_t *)pkt;
    ip->ver_ihl    = 0x45;
    ip->total_len  = htons((uint16_t)total);
    ip->ident      = htons((uint16_t)(s->our_seq & 0xFFFF));
    ip->flags_frag = htons(0x4000);            /* DF */
    ip->ttl        = 64;
    ip->protocol   = 6;                        /* TCP */
    ip->src_addr   = s->dst.a.v4;              /* swap */
    ip->dst_addr   = s->src.a.v4;
    ip->hdr_csum   = 0;
    ip->hdr_csum   = checksum_compute(ip, ip_len);

    /* --- TCP header ------------------------------------------------- */
    tcp_hdr_t *tcp = (tcp_hdr_t *)(pkt + ip_len);
    tcp->src_port  = s->dst_port;
    tcp->dst_port  = s->src_port;
    tcp->seq       = htonl(s->our_seq);
    tcp->ack       = htonl(s->client_seq);
    tcp->off_rsvd  = 0x50;                     /* offset = 5 words */
    tcp->flags     = flags;
    tcp->window    = htons(65535);

    if (pay_len > 0 && payload) {
        memcpy(pkt + ip_len + tcp_len, payload, (size_t)pay_len);
    }

    tcp->csum = 0;
    tcp->csum = tcp_udp_csum_v4(s->dst.a.v4, s->src.a.v4, 6,
                                 tcp, tcp_len + pay_len);

    write(e->tun_fd, pkt, (size_t)total);

    /* advance our seq */
    if (flags & TF_SYN) s->our_seq++;
    if (flags & TF_FIN) s->our_seq++;
    s->our_seq += (uint32_t)pay_len;
}

static void write_tcp_v6(engine_t *e, tcp_session_t *s,
                          uint8_t flags,
                          const uint8_t *payload, int pay_len) {
    const int ip_len  = 40;
    const int tcp_len = 20;
    int total = ip_len + tcp_len + pay_len;
    uint8_t pkt[MAX_PACKET_SIZE];
    if (total > (int)sizeof(pkt)) return;
    memset(pkt, 0, (size_t)total);

    ipv6_hdr_t *ip6 = (ipv6_hdr_t *)pkt;
    ip6->ver_tc_flow   = htonl(0x60000000);
    ip6->payload_len   = htons((uint16_t)(tcp_len + pay_len));
    ip6->next_hdr      = 6;
    ip6->hop_limit     = 64;
    memcpy(ip6->src_addr, s->dst.a.v6, 16);
    memcpy(ip6->dst_addr, s->src.a.v6, 16);

    tcp_hdr_t *tcp = (tcp_hdr_t *)(pkt + ip_len);
    tcp->src_port  = s->dst_port;
    tcp->dst_port  = s->src_port;
    tcp->seq       = htonl(s->our_seq);
    tcp->ack       = htonl(s->client_seq);
    tcp->off_rsvd  = 0x50;
    tcp->flags     = flags;
    tcp->window    = htons(65535);

    if (pay_len > 0 && payload)
        memcpy(pkt + ip_len + tcp_len, payload, (size_t)pay_len);

    tcp->csum = 0;
    tcp->csum = tcp_udp_csum_v6(s->dst.a.v6, s->src.a.v6, 6,
                                 tcp, tcp_len + pay_len);

    write(e->tun_fd, pkt, (size_t)total);

    if (flags & TF_SYN) s->our_seq++;
    if (flags & TF_FIN) s->our_seq++;
    s->our_seq += (uint32_t)pay_len;
}

static void write_tcp(engine_t *e, tcp_session_t *s,
                       uint8_t flags,
                       const uint8_t *payload, int pay_len) {
    if (s->src.version == 4)
        write_tcp_v4(e, s, flags, payload, pay_len);
    else
        write_tcp_v6(e, s, flags, payload, pay_len);
}

/* Build an IPv4 UDP datagram and write to TUN. */
static void write_udp_v4(engine_t *e, udp_session_t *s,
                           const uint8_t *payload, int pay_len) {
    const int ip_len  = 20;
    const int udp_len = 8;
    int total = ip_len + udp_len + pay_len;
    uint8_t pkt[MAX_PACKET_SIZE];
    if (total > (int)sizeof(pkt)) return;
    memset(pkt, 0, (size_t)total);

    ipv4_hdr_t *ip = (ipv4_hdr_t *)pkt;
    ip->ver_ihl    = 0x45;
    ip->total_len  = htons((uint16_t)total);
    ip->flags_frag = htons(0x4000);
    ip->ttl        = 64;
    ip->protocol   = 17;
    ip->src_addr   = s->dst.a.v4;
    ip->dst_addr   = s->src.a.v4;
    ip->hdr_csum   = 0;
    ip->hdr_csum   = checksum_compute(ip, ip_len);

    udp_hdr_t *udp = (udp_hdr_t *)(pkt + ip_len);
    udp->src_port  = s->dst_port;
    udp->dst_port  = s->src_port;
    udp->length    = htons((uint16_t)(udp_len + pay_len));
    memcpy(pkt + ip_len + udp_len, payload, (size_t)pay_len);
    udp->csum = 0;
    udp->csum = tcp_udp_csum_v4(s->dst.a.v4, s->src.a.v4, 17,
                                 udp, udp_len + pay_len);
    if (udp->csum == 0) udp->csum = 0xFFFF;

    write(e->tun_fd, pkt, (size_t)total);
}

static void write_udp_v6(engine_t *e, udp_session_t *s,
                           const uint8_t *payload, int pay_len) {
    const int ip_len  = 40;
    const int udp_len = 8;
    int total = ip_len + udp_len + pay_len;
    uint8_t pkt[MAX_PACKET_SIZE];
    if (total > (int)sizeof(pkt)) return;
    memset(pkt, 0, (size_t)total);

    ipv6_hdr_t *ip6 = (ipv6_hdr_t *)pkt;
    ip6->ver_tc_flow = htonl(0x60000000);
    ip6->payload_len = htons((uint16_t)(udp_len + pay_len));
    ip6->next_hdr    = 17;
    ip6->hop_limit   = 64;
    memcpy(ip6->src_addr, s->dst.a.v6, 16);
    memcpy(ip6->dst_addr, s->src.a.v6, 16);

    udp_hdr_t *udp = (udp_hdr_t *)(pkt + ip_len);
    udp->src_port = s->dst_port;
    udp->dst_port = s->src_port;
    udp->length   = htons((uint16_t)(udp_len + pay_len));
    memcpy(pkt + ip_len + udp_len, payload, (size_t)pay_len);
    udp->csum = 0;
    udp->csum = tcp_udp_csum_v6(s->dst.a.v6, s->src.a.v6, 17,
                                 udp, udp_len + pay_len);
    if (udp->csum == 0) udp->csum = 0xFFFF;

    write(e->tun_fd, pkt, (size_t)total);
}

/* ================================================================== */
/*  Inbound from TUN — TCP                                            */
/* ================================================================== */

static void handle_tcp_tun(engine_t *e,
                            ip_addr_t *src, ip_addr_t *dst,
                            const uint8_t *seg, int seg_len,
                            int pkt_len) {
    if (seg_len < 20) return;
    const tcp_hdr_t *th = (const tcp_hdr_t *)seg;

    int hdr_len  = (th->off_rsvd >> 4) * 4;
    if (hdr_len < 20 || seg_len < hdr_len) return;

    uint16_t sp  = th->src_port;
    uint16_t dp  = th->dst_port;
    int pay_len  = seg_len - hdr_len;
    const uint8_t *payload = seg + hdr_len;

    /* rate-limit upload */
    int small = (pkt_len <= SMALL_PKT_THRESHOLD);
    int64_t wait = rate_limiter_consume(&e->ul_limiter, pkt_len, small);
    if (wait > 0) usleep((useconds_t)wait);
    __sync_fetch_and_add(&e->ul_bytes, pkt_len);

    pthread_mutex_lock(&e->tcp_lock);
    tcp_session_t *s = tcp_find(e, src, sp, dst, dp);

    /* ---- SYN (new connection) ------------------------------------- */
    if ((th->flags & TF_SYN) && !(th->flags & TF_ACK)) {
        if (s) { tcp_remove(e, s); s = NULL; }

        s = tcp_create(e, src, sp, dst, dp, ntohl(th->seq));
        if (!s) { pthread_mutex_unlock(&e->tcp_lock); return; }

        /* Do NOT send SYN-ACK yet — wait for real socket connect()   */
        /* to complete (EPOLLOUT in handle_socket_event).              */
        /* State remains TCP_SYN_SENT.                                */
        pthread_mutex_unlock(&e->tcp_lock);
        return;
    }

    if (!s) { pthread_mutex_unlock(&e->tcp_lock); return; }
    s->last_active = time(NULL);

    /* ---- RST ------------------------------------------------------ */
    if (th->flags & TF_RST) {
        s->state = TCP_CLOSED;
        tcp_remove(e, s);
        pthread_mutex_unlock(&e->tcp_lock);
        return;
    }

    /* ---- DATA ----------------------------------------------------- */
    if (pay_len > 0) {
        s->client_seq = ntohl(th->seq) + (uint32_t)pay_len;

        if (s->state == TCP_SYN_SENT) {
            /* Real socket still connecting — buffer the data */
            int need = s->pending_len + pay_len;
            if (need <= 65536) {  /* 64KB max buffer */
                if (need > s->pending_cap) {
                    int newcap = need + 4096;
                    uint8_t *nb = realloc(s->pending_data, (size_t)newcap);
                    if (nb) {
                        s->pending_data = nb;
                        s->pending_cap = newcap;
                    }
                }
                if (s->pending_data && s->pending_len + pay_len <= s->pending_cap) {
                    memcpy(s->pending_data + s->pending_len, payload, (size_t)pay_len);
                    s->pending_len += pay_len;
                }
            }
            /* Don't ACK yet — we'll ACK after connect completes */
        } else if (s->state == TCP_ESTABLISHED) {
            int sent = (int)send(s->sock_fd, payload, (size_t)pay_len, MSG_NOSIGNAL);
            if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                write_tcp(e, s, TF_RST | TF_ACK, NULL, 0);
                s->state = TCP_CLOSED;
                tcp_remove(e, s);
                pthread_mutex_unlock(&e->tcp_lock);
                return;
            }
            /* ACK the received data */
            write_tcp(e, s, TF_ACK, NULL, 0);
        }
    }

    /* ---- FIN ------------------------------------------------------ */
    if (th->flags & TF_FIN) {
        s->client_seq = ntohl(th->seq) + 1;
        write_tcp(e, s, TF_ACK, NULL, 0);
        write_tcp(e, s, TF_FIN | TF_ACK, NULL, 0);
        s->state = TCP_CLOSED;
        shutdown(s->sock_fd, SHUT_WR);
        tcp_remove(e, s);
    }

    pthread_mutex_unlock(&e->tcp_lock);
}

/* ================================================================== */
/*  Inbound from TUN — UDP                                            */
/* ================================================================== */

static void handle_udp_tun(engine_t *e,
                            ip_addr_t *src, ip_addr_t *dst,
                            const uint8_t *seg, int seg_len,
                            int pkt_len) {
    if (seg_len < 8) return;
    const udp_hdr_t *uh = (const udp_hdr_t *)seg;

    int pay_len = ntohs(uh->length) - 8;
    if (pay_len < 0 || pay_len > seg_len - 8) return;
    const uint8_t *payload = seg + 8;
    uint16_t sp = uh->src_port, dp = uh->dst_port;

    int small = (pkt_len <= SMALL_PKT_THRESHOLD);
    int64_t wait = rate_limiter_consume(&e->ul_limiter, pkt_len, small);
    if (wait > 0) usleep((useconds_t)wait);
    __sync_fetch_and_add(&e->ul_bytes, pkt_len);

    pthread_mutex_lock(&e->udp_lock);
    udp_session_t *s = udp_find(e, src, sp, dst, dp);
    if (!s) {
        s = udp_create(e, src, sp, dst, dp);
        if (!s) { pthread_mutex_unlock(&e->udp_lock); return; }
    }
    s->last_active = time(NULL);
    pthread_mutex_unlock(&e->udp_lock);

    send(s->sock_fd, payload, (size_t)pay_len, MSG_NOSIGNAL);
}

/* ================================================================== */
/*  Response from real socket                                         */
/* ================================================================== */

static void handle_socket_event(engine_t *e, int fd, uint32_t events) {
    uint8_t buf[MAX_PACKET_SIZE];

    /* ---- TCP? ----------------------------------------------------- */
    pthread_mutex_lock(&e->tcp_lock);
    tcp_session_t *ts = tcp_find_by_fd(e, fd);
    if (ts) {
        /* Connect completion? */
        if (ts->state == TCP_SYN_SENT && (events & EPOLLOUT)) {
            int err = 0;
            socklen_t el = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err == 0) {
                /* NOW send SYN-ACK — real connection is established */
                write_tcp(e, ts, TF_SYN | TF_ACK, NULL, 0);
                ts->state = TCP_ESTABLISHED;

                /* Flush any buffered data that arrived during connect */
                if (ts->pending_data && ts->pending_len > 0) {
                    send(ts->sock_fd, ts->pending_data,
                         (size_t)ts->pending_len, MSG_NOSIGNAL);
                    /* ACK the buffered client data */
                    write_tcp(e, ts, TF_ACK, NULL, 0);
                    free(ts->pending_data);
                    ts->pending_data = NULL;
                    ts->pending_len = 0;
                    ts->pending_cap = 0;
                }

                /* Remove EPOLLOUT interest (connection done) */
                struct epoll_event ev;
                ev.events  = EPOLLIN;
                ev.data.fd = fd;
                epoll_ctl(e->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
            } else {
                write_tcp(e, ts, TF_RST, NULL, 0);
                ts->state = TCP_CLOSED;
                tcp_remove(e, ts);
            }
            pthread_mutex_unlock(&e->tcp_lock);
            return;
        }

        if (!(events & EPOLLIN)) {
            pthread_mutex_unlock(&e->tcp_lock);
            return;
        }

        int n = (int)recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            ts->last_active = time(NULL);

            /* rate-limit download */
            int small = (n <= SMALL_PKT_THRESHOLD);
            int64_t wait = rate_limiter_consume(&e->dl_limiter, n, small);
            if (wait > 0) usleep((useconds_t)wait);
            __sync_fetch_and_add(&e->dl_bytes, n);

            write_tcp(e, ts, TF_PSH | TF_ACK, buf, n);
        } else if (n == 0) {
            /* Remote closed */
            write_tcp(e, ts, TF_FIN | TF_ACK, NULL, 0);
            ts->state = TCP_FIN_WAIT;
            tcp_remove(e, ts);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            write_tcp(e, ts, TF_RST, NULL, 0);
            ts->state = TCP_CLOSED;
            tcp_remove(e, ts);
        }
        pthread_mutex_unlock(&e->tcp_lock);
        return;
    }
    pthread_mutex_unlock(&e->tcp_lock);

    /* ---- UDP? ----------------------------------------------------- */
    pthread_mutex_lock(&e->udp_lock);
    udp_session_t *us = udp_find_by_fd(e, fd);
    if (us && (events & EPOLLIN)) {
        int n = (int)recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            us->last_active = time(NULL);

            int small = (n <= SMALL_PKT_THRESHOLD);
            int64_t wait = rate_limiter_consume(&e->dl_limiter, n, small);
            if (wait > 0) usleep((useconds_t)wait);
            __sync_fetch_and_add(&e->dl_bytes, n);

            if (us->src.version == 4)
                write_udp_v4(e, us, buf, n);
            else
                write_udp_v6(e, us, buf, n);
        }
    }
    pthread_mutex_unlock(&e->udp_lock);
}

/* ================================================================== */
/*  Periodic cleanup of timed-out sessions                            */
/* ================================================================== */

static void cleanup(engine_t *e) {
    time_t now = time(NULL);

    pthread_mutex_lock(&e->tcp_lock);
    for (int i = 0; i < SESSION_TABLE_SIZE; i++) {
        tcp_session_t **pp = &e->tcp_table[i];
        while (*pp) {
            tcp_session_t *s = *pp;
            if (s->state == TCP_CLOSED ||
                (now - s->last_active > TCP_SESSION_TIMEOUT)) {
                *pp = s->next;
                epoll_ctl(e->epoll_fd, EPOLL_CTL_DEL, s->sock_fd, NULL);
                close(s->sock_fd);
                if (s->pending_data) free(s->pending_data);
                free(s);
            } else {
                pp = &s->next;
            }
        }
    }
    pthread_mutex_unlock(&e->tcp_lock);

    pthread_mutex_lock(&e->udp_lock);
    for (int i = 0; i < SESSION_TABLE_SIZE; i++) {
        udp_session_t **pp = &e->udp_table[i];
        while (*pp) {
            udp_session_t *s = *pp;
            if (now - s->last_active > UDP_SESSION_TIMEOUT) {
                *pp = s->next;
                epoll_ctl(e->epoll_fd, EPOLL_CTL_DEL, s->sock_fd, NULL);
                close(s->sock_fd);
                free(s);
            } else {
                pp = &s->next;
            }
        }
    }
    pthread_mutex_unlock(&e->udp_lock);
}

/* ================================================================== */
/*  Main event loop (runs on its own pthread)                         */
/* ================================================================== */

static void *engine_loop(void *arg) {
    engine_t *e = (engine_t *)arg;
    uint8_t pkt[MAX_PACKET_SIZE];
    struct epoll_event events[128];
    time_t last_cleanup = time(NULL);

    LOGI("engine_loop started  tun_fd=%d", e->tun_fd);

    while (e->running) {
        int n = epoll_wait(e->epoll_fd, events, 128, 100 /* ms */);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGE("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == e->tun_fd) {
                /* ---- packet from device -------------------------------- */
                int len = (int)read(e->tun_fd, pkt, sizeof(pkt));
                if (len <= 0) continue;

                int version = (pkt[0] >> 4) & 0x0F;
                ip_addr_t src, dst;
                uint8_t proto;
                int hdr_len;

                if (version == 4 && len >= 20) {
                    ipv4_hdr_t *ip = (ipv4_hdr_t *)pkt;
                    hdr_len = (ip->ver_ihl & 0x0F) * 4;
                    if (hdr_len < 20 || len < hdr_len) continue;
                    src.version = 4; src.a.v4 = ip->src_addr;
                    dst.version = 4; dst.a.v4 = ip->dst_addr;
                    proto = ip->protocol;
                } else if (version == 6 && len >= 40) {
                    ipv6_hdr_t *ip6 = (ipv6_hdr_t *)pkt;
                    hdr_len = 40;
                    src.version = 6; memcpy(src.a.v6, ip6->src_addr, 16);
                    dst.version = 6; memcpy(dst.a.v6, ip6->dst_addr, 16);
                    proto = ip6->next_hdr;
                } else {
                    continue;
                }

                const uint8_t *transport = pkt + hdr_len;
                int tlen = len - hdr_len;

                if (proto == 6)        /* TCP */
                    handle_tcp_tun(e, &src, &dst, transport, tlen, len);
                else if (proto == 17)  /* UDP */
                    handle_udp_tun(e, &src, &dst, transport, tlen, len);
                /* other protocols (ICMP, etc.) silently dropped */

            } else {
                /* ---- data from a real socket --------------------------- */
                handle_socket_event(e, fd, events[i].events);
            }
        }

        /* periodic session cleanup */
        time_t now = time(NULL);
        if (now - last_cleanup >= 10) {
            cleanup(e);
            last_cleanup = now;
        }
    }

    LOGI("engine_loop exiting");
    return NULL;
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

int engine_start(engine_t *e, int tun_fd, int mtu,
                 JavaVM *jvm, JNIEnv *env, jobject vpn_obj) {
    memset(e, 0, sizeof(*e));
    e->tun_fd  = tun_fd;
    e->mtu     = mtu;
    e->running = 1;
    e->jvm     = jvm;

    /* Cache JNI references */
    e->vpn_ref = (*env)->NewGlobalRef(env, vpn_obj);
    jclass cls = (*env)->GetObjectClass(env, vpn_obj);
    e->protect_mid = (*env)->GetMethodID(env, cls, "protectSocket", "(I)Z");
    if (!e->protect_mid) {
        LOGE("engine_start: protectSocket method not found");
        (*env)->DeleteGlobalRef(env, e->vpn_ref);
        return -1;
    }

    pthread_mutex_init(&e->tcp_lock, NULL);
    pthread_mutex_init(&e->udp_lock, NULL);

    /* Initialise limiters with effectively unlimited rate. */
    rate_limiter_init(&e->ul_limiter, 1000000000LL);  /* ~1 GB/s */
    rate_limiter_init(&e->dl_limiter, 1000000000LL);

    e->epoll_fd = epoll_create1(0);
    if (e->epoll_fd < 0) {
        LOGE("epoll_create1: %s", strerror(errno));
        return -1;
    }

    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = tun_fd;
    if (epoll_ctl(e->epoll_fd, EPOLL_CTL_ADD, tun_fd, &ev) < 0) {
        LOGE("epoll_ctl(tun_fd): %s", strerror(errno));
        close(e->epoll_fd);
        return -1;
    }

    if (pthread_create(&e->thread, NULL, engine_loop, e) != 0) {
        LOGE("pthread_create: %s", strerror(errno));
        close(e->epoll_fd);
        return -1;
    }

    LOGI("engine_start OK  fd=%d mtu=%d", tun_fd, mtu);
    return 0;
}

void engine_stop(engine_t *e, JNIEnv *env) {
    if (!e->running) return;
    e->running = 0;
    pthread_join(e->thread, NULL);

    /* tear down every session */
    for (int i = 0; i < SESSION_TABLE_SIZE; i++) {
        tcp_session_t *s = e->tcp_table[i];
        while (s) {
            tcp_session_t *nx = s->next;
            close(s->sock_fd);
            if (s->pending_data) free(s->pending_data);
            free(s);
            s = nx;
        }
        e->tcp_table[i] = NULL;

        udp_session_t *u = e->udp_table[i];
        while (u) {
            udp_session_t *nx = u->next;
            close(u->sock_fd);
            free(u);
            u = nx;
        }
        e->udp_table[i] = NULL;
    }

    close(e->epoll_fd);

    if (env && e->vpn_ref)
        (*env)->DeleteGlobalRef(env, e->vpn_ref);

    rate_limiter_destroy(&e->ul_limiter);
    rate_limiter_destroy(&e->dl_limiter);
    pthread_mutex_destroy(&e->tcp_lock);
    pthread_mutex_destroy(&e->udp_lock);

    LOGI("engine_stop done");
}

void engine_set_rates(engine_t *e, int64_t dl_bps, int64_t ul_bps) {
    rate_limiter_update(&e->dl_limiter, dl_bps);
    rate_limiter_update(&e->ul_limiter, ul_bps);
    LOGI("rates updated  dl=%lld  ul=%lld bps", (long long)dl_bps, (long long)ul_bps);
}

void engine_get_stats(engine_t *e, int64_t *ul, int64_t *dl) {
    *ul = e->ul_bytes;
    *dl = e->dl_bytes;
}
