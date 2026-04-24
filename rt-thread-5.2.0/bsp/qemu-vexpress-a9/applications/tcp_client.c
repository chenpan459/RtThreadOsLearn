/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * TCP client: connect and send packets (msh: tcp_send).
 */

#include <rtthread.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#if !defined(SAL_USING_POSIX)
#error "Enable SAL_USING_POSIX (Network + lwIP + SAL) for BSD sockets."
#endif

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <finsh.h>

#define TCP_SEND_DEFAULT_HOST   "172.21.65.240"
#define TCP_SEND_DEFAULT_PORT   60000
#define TCP_SEND_DEFAULT_LEN    256
#define TCP_SEND_DEFAULT_COUNT  1
#define TCP_SEND_THREAD_STACK   4096
#define TCP_SEND_THREAD_PRIO    20

struct tcp_send_param
{
    char host[64];
    int port;
    rt_size_t len;
    int count;
};

/* Same layout as BSD / lwIP struct linger (not always in <sys/socket.h> on newlib). */
struct tcp_send_linger
{
    int l_onoff;
    int l_linger;
};

static void tcp_send_usage(void)
{
    rt_kprintf("tcp_send [-h host] [-p port] [-l len] [-n count]\n");
    rt_kprintf("  default: %s:%d, len=%d, count=%d\n",
               TCP_SEND_DEFAULT_HOST, TCP_SEND_DEFAULT_PORT,
               TCP_SEND_DEFAULT_LEN, TCP_SEND_DEFAULT_COUNT);
}

/* Send until all bytes accepted by stack (handles partial send). */
static int tcp_send_all(int sock, const void *data, rt_size_t len, int flags)
{
    const rt_uint8_t *p = (const rt_uint8_t *)data;
    rt_size_t left = len;

    while (left > 0)
    {
        int n = send(sock, p, left, flags);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += (rt_size_t)n;
        left -= (rt_size_t)n;
    }
    return 0;
}

/* Half-close then drain peer (or timeout) so FIN/RST ordering matches common servers. */
static void tcp_shutdown_graceful(int sock)
{
    struct timeval tv;
    char drain[256];
    int n;

    shutdown(sock, SHUT_WR);

    memset(&tv, 0, sizeof(tv));
    tv.tv_sec = 2;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while ((n = recv(sock, drain, sizeof(drain), 0)) > 0)
        ;
}

static int do_tcp_send(const char *host, int port, rt_size_t pkt_len, int count)
{
    int sock = -1;
    rt_uint8_t *buf = RT_NULL;
    struct sockaddr_in addr;
    int i, total = 0;

    if (pkt_len == 0 || count < 1)
    {
        rt_kprintf("tcp_send: invalid len or count\n");
        return -RT_EINVAL;
    }

    buf = (rt_uint8_t *)rt_malloc(pkt_len);
    if (!buf)
    {
        rt_kprintf("tcp_send: out of memory\n");
        return -RT_ENOMEM;
    }
    for (i = 0; i < (int)pkt_len; i++)
        buf[i] = (rt_uint8_t)(i & 0xff);

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        rt_kprintf("tcp_send: socket failed, errno=%d\n", errno);
        rt_free(buf);
        return -RT_ERROR;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((rt_uint16_t)port);
    if (inet_aton(host, &addr.sin_addr) == 0)
    {
        rt_kprintf("tcp_send: bad address %s\n", host);
        closesocket(sock);
        rt_free(buf);
        return -RT_EINVAL;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        rt_kprintf("tcp_send: connect %s:%d failed, errno=%d\n", host, port, errno);
        closesocket(sock);
        rt_free(buf);
        return -RT_ERROR;
    }

    {
        int on = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    }
    {
        struct tcp_send_linger ling;

        ling.l_onoff = 1;
        ling.l_linger = 5; /* seconds: wait for unsent/unacked data on close */
        setsockopt(sock, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
    }

    rt_kprintf("tcp_send: connected, sending %d packet(s), %d bytes each\n", count, (int)pkt_len);

    for (i = 0; i < count; i++)
    {
        if (tcp_send_all(sock, buf, pkt_len, 0) != 0)
        {
            rt_kprintf("tcp_send: send failed at #%d, errno=%d\n", i + 1, errno);
            break;
        }
        total += (int)pkt_len;
    }

    tcp_shutdown_graceful(sock);

    closesocket(sock);
    rt_free(buf);
    rt_kprintf("tcp_send: done, total sent %d bytes\n", total);
    return 0;
}

static void tcp_send_thread_entry(void *parameter)
{
    struct tcp_send_param *p = parameter;

    do_tcp_send(p->host, p->port, p->len, p->count);
    rt_free(p);
}

static int cmd_tcp_send(int argc, char **argv)
{
    char host[64];
    int port = TCP_SEND_DEFAULT_PORT;
    rt_size_t len = TCP_SEND_DEFAULT_LEN;
    int count = TCP_SEND_DEFAULT_COUNT;
    int i;

    rt_strncpy(host, TCP_SEND_DEFAULT_HOST, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';

    for (i = 1; i < argc; i++)
    {
        if (rt_strcmp(argv[i], "-h") == 0 && i + 1 < argc)
        {
            rt_strncpy(host, argv[++i], sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
        }
        else if (rt_strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (rt_strcmp(argv[i], "-l") == 0 && i + 1 < argc)
            len = (rt_size_t)atoi(argv[++i]);
        else if (rt_strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            count = atoi(argv[++i]);
        else if (rt_strcmp(argv[i], "--help") == 0)
        {
            tcp_send_usage();
            return 0;
        }
    }

    if (port <= 0 || port > 65535)
    {
        rt_kprintf("tcp_send: invalid port\n");
        return -1;
    }

    {
        struct tcp_send_param *p = rt_malloc(sizeof(*p));

        if (!p)
        {
            rt_kprintf("tcp_send: no memory for thread param\n");
            return -1;
        }
        rt_strncpy(p->host, host, sizeof(p->host) - 1);
        p->host[sizeof(p->host) - 1] = '\0';
        p->port = port;
        p->len = len;
        p->count = count;

        {
            rt_thread_t tid = rt_thread_create("tcp_send", tcp_send_thread_entry, p,
                                               TCP_SEND_THREAD_STACK, TCP_SEND_THREAD_PRIO, 10);
            if (!tid)
            {
                rt_kprintf("tcp_send: create thread failed\n");
                rt_free(p);
                return -1;
            }
            rt_thread_startup(tid);
        }
    }
    return 0;
}

MSH_CMD_EXPORT_ALIAS(cmd_tcp_send, tcp_send, TCP client send to default or -h -p -l -n);
