#ifndef TUN2SOCKS_ENGINE_H
#define TUN2SOCKS_ENGINE_H

#include <stdint.h>
#include <pthread.h>
#include <jni.h>
#include "rate_limiter.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */
#define MAX_PACKET_SIZE      65535
#define SESSION_TABLE_SIZE   2048
#define TCP_SESSION_TIMEOUT  120      /* seconds */
#define UDP_SESSION_TIMEOUT  60       /* seconds */
#define SMALL_PKT_THRESHOLD  128      /* bytes   */

/* ------------------------------------------------------------------ */
/*  Portable packet headers (packed, byte-order agnostic fields)      */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)

typedef struct {
    uint8_t  ver_ihl;           /* version(4) | IHL(4)                  */
    uint8_t  tos;
    uint16_t total_len;
    uint16_t ident;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t hdr_csum;
    uint32_t src_addr;
    uint32_t dst_addr;
} ipv4_hdr_t;

typedef struct {
    uint32_t ver_tc_flow;       /* version(4) | TC(8) | flow(20)        */
    uint16_t payload_len;
    uint8_t  next_hdr;
    uint8_t  hop_limit;
    uint8_t  src_addr[16];
    uint8_t  dst_addr[16];
} ipv6_hdr_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  off_rsvd;          /* data-offset(4) | reserved(4)         */
    uint8_t  flags;             /* CWR ECE URG ACK PSH RST SYN FIN     */
    uint16_t window;
    uint16_t csum;
    uint16_t urg_ptr;
} tcp_hdr_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t csum;
} udp_hdr_t;

#pragma pack(pop)

/* TCP flag masks */
#define TF_FIN  0x01
#define TF_SYN  0x02
#define TF_RST  0x04
#define TF_PSH  0x08
#define TF_ACK  0x10
#define TF_URG  0x20

/* ------------------------------------------------------------------ */
/*  Address abstraction (v4 / v6)                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    int version;                /* 4 or 6                               */
    union {
        uint32_t v4;
        uint8_t  v6[16];
    } a;
} ip_addr_t;

/* ------------------------------------------------------------------ */
/*  TCP session                                                       */
/* ------------------------------------------------------------------ */
typedef enum {
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,
    TCP_CLOSED
} tcp_state_t;

typedef struct tcp_session {
    ip_addr_t  src, dst;
    uint16_t   src_port, dst_port;
    int        sock_fd;
    tcp_state_t state;
    uint32_t   client_seq;      /* next expected seq from client        */
    uint32_t   our_seq;         /* our next outgoing seq                */
    time_t     last_active;
    uint8_t   *pending_data;    /* buffered data before connect done    */
    int        pending_len;     /* bytes in pending_data                */
    int        pending_cap;     /* allocated capacity of pending_data   */
    struct tcp_session *next;
} tcp_session_t;

/* ------------------------------------------------------------------ */
/*  UDP session                                                       */
/* ------------------------------------------------------------------ */
typedef struct udp_session {
    ip_addr_t  src, dst;
    uint16_t   src_port, dst_port;
    int        sock_fd;
    time_t     last_active;
    struct udp_session *next;
} udp_session_t;

/* ------------------------------------------------------------------ */
/*  Engine state                                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    int              tun_fd;
    int              mtu;
    int              epoll_fd;
    volatile int     running;

    /* sessions */
    tcp_session_t   *tcp_table[SESSION_TABLE_SIZE];
    udp_session_t   *udp_table[SESSION_TABLE_SIZE];
    pthread_mutex_t  tcp_lock;
    pthread_mutex_t  udp_lock;

    /* rate limiters */
    rate_limiter_t   ul_limiter;     /* upload   (TUN → internet) */
    rate_limiter_t   dl_limiter;     /* download (internet → TUN) */

    /* traffic counters (updated with __sync builtins) */
    volatile int64_t ul_bytes;
    volatile int64_t dl_bytes;

    /* set to 1 when a download read is skipped due to empty bucket */
    int              dl_starved;

    /* JNI */
    JavaVM          *jvm;
    jobject          vpn_ref;        /* global ref to Tun2SocksEngine  */
    jmethodID       protect_mid;    /* protectSocket(I)Z              */

    pthread_t        thread;
} engine_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */
int   engine_start(engine_t *e, int tun_fd, int mtu,
                   JavaVM *jvm, JNIEnv *env, jobject vpn_obj);
void  engine_stop(engine_t *e, JNIEnv *env);
void  engine_set_rates(engine_t *e, int64_t dl_bps, int64_t ul_bps);
void  engine_get_stats(engine_t *e, int64_t *ul, int64_t *dl);

#endif /* TUN2SOCKS_ENGINE_H */
