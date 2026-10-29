#include <libusb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <net/bpf.h>
#include <ifaddrs.h>
#include <netinet/in.h>

#define AWP_HOST_XFER 16384
#define AWP_RX_BUF 65536
#define AWP_CTL_BUF 1032
#define AWP_HDR 44

typedef struct awp_dev {
    libusb_device *awp_usb;
    libusb_device_handle *awp_h;
    int awp_ctl_if;
    int awp_data_if;
    uint8_t awp_ep_int;
    uint8_t awp_ep_in;
    uint8_t awp_ep_out;
    int awp_out_mps;
    uint8_t awp_mac[6];
    char awp_eth[IFNAMSIZ];
    char awp_peer[IFNAMSIZ];
    int awp_bpf;
    volatile int awp_alive;
    uint32_t awp_reqid;
    struct awp_dev *awp_next;
} awp_dev;

static awp_dev *awp_list;
static pthread_mutex_t awp_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t awp_quit;

static void awp_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("androidtetherd: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int awp_run(const char *fmt, ...)
{
    char cmd[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    return system(cmd);
}

static uint32_t awp_rd(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void awp_wr(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static int awp_dead(int r)
{
    return r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO || r == LIBUSB_ERROR_PIPE;
}

static int awp_probe(awp_dev *d)
{
    struct libusb_config_descriptor *c;
    int ctl = -1, dat = -1, i, j;
    if (libusb_get_active_config_descriptor(d->awp_usb, &c) != 0)
        return -1;
    for (i = 0; i < c->bNumInterfaces; i++) {
        const struct libusb_interface_descriptor *a = &c->interface[i].altsetting[0];
        int rn = (a->bInterfaceClass == 0xE0 && a->bInterfaceSubClass == 1 && a->bInterfaceProtocol == 3) ||
                 (a->bInterfaceClass == 0xEF && a->bInterfaceSubClass == 4 && a->bInterfaceProtocol == 1) ||
                 (a->bInterfaceClass == 2 && a->bInterfaceSubClass == 2 && a->bInterfaceProtocol == 0xFF);
        if (rn && ctl < 0) {
            ctl = a->bInterfaceNumber;
            d->awp_ep_int = 0;
            for (j = 0; j < a->bNumEndpoints; j++)
                if ((a->endpoint[j].bmAttributes & 3) == LIBUSB_TRANSFER_TYPE_INTERRUPT &&
                    (a->endpoint[j].bEndpointAddress & 0x80))
                    d->awp_ep_int = a->endpoint[j].bEndpointAddress;
        } else if (ctl >= 0 && dat < 0 && a->bInterfaceClass == 0x0A) {
            dat = a->bInterfaceNumber;
            for (j = 0; j < a->bNumEndpoints; j++) {
                if ((a->endpoint[j].bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK)
                    continue;
                if (a->endpoint[j].bEndpointAddress & 0x80)
                    d->awp_ep_in = a->endpoint[j].bEndpointAddress;
                else {
                    d->awp_ep_out = a->endpoint[j].bEndpointAddress;
                    d->awp_out_mps = a->endpoint[j].wMaxPacketSize;
                }
            }
        }
    }
    libusb_free_config_descriptor(c);
    if (ctl < 0 || dat < 0 || !d->awp_ep_in || !d->awp_ep_out)
        return -1;
    d->awp_ctl_if = ctl;
    d->awp_data_if = dat;
    return 0;
}

static int awp_ctl_send(awp_dev *d, uint8_t *msg, uint32_t len)
{
    return libusb_control_transfer(d->awp_h, 0x21, 0, 0, d->awp_ctl_if, msg, len, 2000);
}

static void awp_keepalive_reply(awp_dev *d, uint32_t id)
{
    uint8_t rep[16];
    awp_wr(rep, 0x80000008u);
    awp_wr(rep + 4, 16);
    awp_wr(rep + 8, id);
    awp_wr(rep + 12, 0);
    awp_ctl_send(d, rep, 16);
}

static int awp_xchg(awp_dev *d, uint8_t *msg, uint8_t *resp, int rmax)
{
    uint32_t type = awp_rd(msg);
    uint32_t id = ++d->awp_reqid;
    int i, r;
    awp_wr(msg + 8, id);
    if (awp_ctl_send(d, msg, awp_rd(msg + 4)) < 0)
        return -1;
    for (i = 0; i < 50; i++) {
        r = libusb_control_transfer(d->awp_h, 0xA1, 1, 0, d->awp_ctl_if, resp, rmax, 1000);
        if (r >= 12) {
            if (awp_rd(resp) == (type | 0x80000000u) && awp_rd(resp + 8) == id)
                return r;
            if (awp_rd(resp) == 8)
                awp_keepalive_reply(d, awp_rd(resp + 8));
        } else if (awp_dead(r)) {
            return -1;
        }
        usleep(40000);
    }
    return -1;
}

static int awp_rndis_init(awp_dev *d)
{
    uint8_t msg[32], resp[AWP_CTL_BUF];
    int r;
    memset(msg, 0, sizeof msg);
    awp_wr(msg, 2);
    awp_wr(msg + 4, 24);
    awp_wr(msg + 12, 1);
    awp_wr(msg + 16, 0);
    awp_wr(msg + 20, AWP_HOST_XFER);
    r = awp_xchg(d, msg, resp, sizeof resp);
    if (r < 16 || awp_rd(resp + 12) != 0) {
        awp_log("rndis init failed");
        return -1;
    }

    memset(msg, 0, sizeof msg);
    awp_wr(msg, 4);
    awp_wr(msg + 4, 28);
    awp_wr(msg + 12, 0x01010101u);
    awp_wr(msg + 16, 0);
    awp_wr(msg + 20, 20);
    r = awp_xchg(d, msg, resp, sizeof resp);
    if (r < 24 || awp_rd(resp + 12) != 0 || awp_rd(resp + 16) < 6) {
        awp_log("mac query failed");
        return -1;
    }
    memcpy(d->awp_mac, resp + 8 + awp_rd(resp + 20), 6);

    memset(msg, 0, sizeof msg);
    awp_wr(msg, 5);
    awp_wr(msg + 4, 32);
    awp_wr(msg + 12, 0x0001010Eu);
    awp_wr(msg + 16, 4);
    awp_wr(msg + 20, 20);
    awp_wr(msg + 28, 0x0F);
    r = awp_xchg(d, msg, resp, sizeof resp);
    if (r < 16 || awp_rd(resp + 12) != 0) {
        awp_log("set packet filter failed");
        return -1;
    }
    return 0;
}

static int awp_bpf_open(awp_dev *d)
{
    char path[32];
    struct ifreq ifr;
    struct timeval tv = {1, 0};
    unsigned one = 1, zero = 0;
    int i, fd = -1;
    for (i = 0; i < 256 && fd < 0; i++) {
        snprintf(path, sizeof path, "/dev/bpf%d", i);
        fd = open(path, O_RDWR);
    }
    if (fd < 0)
        return -1;
    memset(&ifr, 0, sizeof ifr);
    strlcpy(ifr.ifr_name, d->awp_peer, sizeof ifr.ifr_name);
    if (ioctl(fd, BIOCSETIF, &ifr) < 0 || ioctl(fd, BIOCIMMEDIATE, &one) < 0 ||
        ioctl(fd, BIOCSHDRCMPLT, &one) < 0 || ioctl(fd, BIOCSSEESENT, &zero) < 0 ||
        ioctl(fd, BIOCPROMISC) < 0 || ioctl(fd, BIOCSRTIMEOUT, &tv) < 0) {
        close(fd);
        return -1;
    }
    d->awp_bpf = fd;
    return 0;
}

static int awp_feth_create(awp_dev *d)
{
    int n;
    char a[IFNAMSIZ], b[IFNAMSIZ];
    for (n = 0; n < 64; n += 2) {
        snprintf(a, sizeof a, "feth%d", n);
        snprintf(b, sizeof b, "feth%d", n + 1);
        if (!if_nametoindex(a) && !if_nametoindex(b))
            break;
    }
    if (n >= 64)
        return -1;
    strlcpy(d->awp_eth, a, sizeof d->awp_eth);
    strlcpy(d->awp_peer, b, sizeof d->awp_peer);
    if (awp_run("/sbin/ifconfig %s create >/dev/null 2>&1", a) ||
        awp_run("/sbin/ifconfig %s create >/dev/null 2>&1", b) ||
        awp_run("/sbin/ifconfig %s peer %s >/dev/null 2>&1", a, b) ||
        awp_run("/sbin/ifconfig %s lladdr %02x:%02x:%02x:%02x:%02x:%02x >/dev/null 2>&1", a,
                d->awp_mac[0], d->awp_mac[1], d->awp_mac[2], d->awp_mac[3], d->awp_mac[4], d->awp_mac[5]) ||
        awp_run("/sbin/ifconfig %s up", a) || awp_run("/sbin/ifconfig %s up", b))
        return -1;
    return 0;
}

static void awp_feth_destroy(awp_dev *d)
{
    if (!d->awp_eth[0])
        return;
    awp_run("/usr/sbin/ipconfig set %s NONE >/dev/null 2>&1", d->awp_eth);
    awp_run("/sbin/ifconfig %s destroy >/dev/null 2>&1", d->awp_eth);
    awp_run("/sbin/ifconfig %s destroy >/dev/null 2>&1", d->awp_peer);
}

static void *awp_ctl_thread(void *arg)
{
    awp_dev *d = arg;
    uint8_t n[16], buf[AWP_CTL_BUF];
    int got, r, g;
    while (d->awp_alive && !awp_quit) {
        r = libusb_interrupt_transfer(d->awp_h, d->awp_ep_int, n, sizeof n, &got, 1000);
        if (r == LIBUSB_ERROR_TIMEOUT)
            continue;
        if (awp_dead(r)) {
            d->awp_alive = 0;
            break;
        }
        if (r != 0) {
            usleep(200000);
            continue;
        }
        g = libusb_control_transfer(d->awp_h, 0xA1, 1, 0, d->awp_ctl_if, buf, sizeof buf, 1000);
        if (g >= 12 && awp_rd(buf) == 8)
            awp_keepalive_reply(d, awp_rd(buf + 8));
    }
    return NULL;
}

static void *awp_rx_thread(void *arg)
{
    awp_dev *d = arg;
    uint8_t *b = malloc(AWP_RX_BUF);
    int n, r;
    uint32_t off, len, dofs, dlen;
    while (d->awp_alive && !awp_quit) {
        n = 0;
        r = libusb_bulk_transfer(d->awp_h, d->awp_ep_in, b, AWP_RX_BUF, &n, 1000);
        if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) {
            if (awp_dead(r)) {
                d->awp_alive = 0;
                break;
            }
            usleep(100000);
            continue;
        }
        off = 0;
        while (off + 8 <= (uint32_t)n) {
            len = awp_rd(b + off + 4);
            if (len < 8 || off + len > (uint32_t)n)
                break;
            if (awp_rd(b + off) == 1 && len >= AWP_HDR) {
                dofs = awp_rd(b + off + 8);
                dlen = awp_rd(b + off + 12);
                if (dlen && 8 + dofs + dlen <= len)
                    if (write(d->awp_bpf, b + off + 8 + dofs, dlen) < 0 && errno == ENXIO) {
                        d->awp_alive = 0;
                        break;
                    }
            }
            off += len;
        }
    }
    free(b);
    return NULL;
}

static void *awp_tx_thread(void *arg)
{
    awp_dev *d = arg;
    unsigned bl = 0;
    uint8_t *buf, *out, *p;
    ssize_t n;
    int sent, r;
    struct bpf_hdr *h;
    uint32_t len, total;
    ioctl(d->awp_bpf, BIOCGBLEN, &bl);
    buf = malloc(bl);
    out = malloc(AWP_HDR + 2048);
    while (d->awp_alive && !awp_quit) {
        n = read(d->awp_bpf, buf, bl);
        if (n < 0 && errno != EINTR && errno != EAGAIN) {
            d->awp_alive = 0;
            break;
        }
        for (p = buf; n > 0 && p < buf + n; p += BPF_WORDALIGN(h->bh_hdrlen + h->bh_caplen)) {
            h = (struct bpf_hdr *)p;
            len = h->bh_caplen;
            if (len == 0 || len > 2000)
                continue;
            total = AWP_HDR + len;
            memset(out, 0, AWP_HDR);
            awp_wr(out, 1);
            awp_wr(out + 4, total);
            awp_wr(out + 8, 36);
            awp_wr(out + 12, len);
            memcpy(out + AWP_HDR, p + h->bh_hdrlen, len);
            r = libusb_bulk_transfer(d->awp_h, d->awp_ep_out, out, total, &sent, 2000);
            if (r == 0 && d->awp_out_mps && total % d->awp_out_mps == 0)
                libusb_bulk_transfer(d->awp_h, d->awp_ep_out, out, 0, &sent, 1000);
            if (awp_dead(r)) {
                d->awp_alive = 0;
                break;
            }
        }
    }
    free(buf);
    free(out);
    return NULL;
}

static void awp_unlink(awp_dev *d)
{
    awp_dev **pp;
    pthread_mutex_lock(&awp_lock);
    for (pp = &awp_list; *pp; pp = &(*pp)->awp_next)
        if (*pp == d) {
            *pp = d->awp_next;
            break;
        }
    pthread_mutex_unlock(&awp_lock);
}

static int awp_healthy(const char *name)
{
    struct ifaddrs *ifa, *p;
    int up = 0, has4 = 0;
    if (getifaddrs(&ifa) != 0)
        return 1;
    for (p = ifa; p; p = p->ifa_next) {
        if (strcmp(p->ifa_name, name) != 0)
            continue;
        if (p->ifa_flags & IFF_UP)
            up = 1;
        if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
            if ((ntohl(sin->sin_addr.s_addr) >> 16) != 0xA9FE)
                has4 = 1;
        }
    }
    freeifaddrs(ifa);
    return up && has4;
}

static void awp_supervise(awp_dev *d)
{
    int bad = 0;
    while (d->awp_alive && !awp_quit) {
        sleep(2);
        if (awp_healthy(d->awp_eth)) {
            bad = 0;
            continue;
        }
        if (++bad < 2)
            continue;
        awp_log("%s lost its address, restoring", d->awp_eth);
        awp_run("/sbin/ifconfig %s up", d->awp_eth);
        awp_run("/sbin/ifconfig %s up", d->awp_peer);
        awp_run("/usr/sbin/ipconfig set %s DHCP", d->awp_eth);
        bad = 0;
        sleep(5);
    }
}

static void *awp_session(void *arg)
{
    awp_dev *d = arg;
    pthread_t t_ctl, t_rx, t_tx;
    int have_ctl = 0, claimed_c = 0, claimed_d = 0;
    struct libusb_device_descriptor dd;
    d->awp_bpf = -1;
    libusb_get_device_descriptor(d->awp_usb, &dd);
    if (libusb_open(d->awp_usb, &d->awp_h) != 0) {
        awp_log("open %04x:%04x failed", dd.idVendor, dd.idProduct);
        goto out;
    }
    if (libusb_claim_interface(d->awp_h, d->awp_ctl_if) != 0 ||
        (claimed_c = 1, libusb_claim_interface(d->awp_h, d->awp_data_if) != 0)) {
        awp_log("claim %04x:%04x failed", dd.idVendor, dd.idProduct);
        goto out;
    }
    claimed_d = 1;
    if (awp_rndis_init(d) != 0)
        goto out;
    if (awp_feth_create(d) != 0 || awp_bpf_open(d) != 0) {
        awp_log("interface setup failed");
        goto out;
    }
    d->awp_alive = 1;
    awp_log("%04x:%04x up as %s (mac %02x:%02x:%02x:%02x:%02x:%02x)", dd.idVendor, dd.idProduct, d->awp_eth,
            d->awp_mac[0], d->awp_mac[1], d->awp_mac[2], d->awp_mac[3], d->awp_mac[4], d->awp_mac[5]);
    pthread_create(&t_rx, NULL, awp_rx_thread, d);
    pthread_create(&t_tx, NULL, awp_tx_thread, d);
    if (d->awp_ep_int) {
        pthread_create(&t_ctl, NULL, awp_ctl_thread, d);
        have_ctl = 1;
    }
    awp_run("/usr/sbin/ipconfig set %s DHCP", d->awp_eth);
    awp_supervise(d);
    d->awp_alive = 0;
    pthread_join(t_rx, NULL);
    pthread_join(t_tx, NULL);
    if (have_ctl)
        pthread_join(t_ctl, NULL);
    awp_log("%s down", d->awp_eth);
out:
    awp_feth_destroy(d);
    if (d->awp_bpf >= 0)
        close(d->awp_bpf);
    if (d->awp_h) {
        if (claimed_d)
            libusb_release_interface(d->awp_h, d->awp_data_if);
        if (claimed_c)
            libusb_release_interface(d->awp_h, d->awp_ctl_if);
        libusb_close(d->awp_h);
    }
    if (!awp_quit)
        sleep(5);
    awp_unlink(d);
    libusb_unref_device(d->awp_usb);
    free(d);
    return NULL;
}

static int awp_tracked(libusb_device *dev)
{
    awp_dev *d;
    int found = 0;
    pthread_mutex_lock(&awp_lock);
    for (d = awp_list; d; d = d->awp_next)
        if (d->awp_usb == dev)
            found = 1;
    pthread_mutex_unlock(&awp_lock);
    return found;
}

static void awp_scan(void)
{
    libusb_device **devs;
    ssize_t n = libusb_get_device_list(NULL, &devs), i;
    pthread_t t;
    for (i = 0; i < n; i++) {
        awp_dev *d;
        if (awp_tracked(devs[i]))
            continue;
        d = calloc(1, sizeof *d);
        d->awp_usb = devs[i];
        if (awp_probe(d) != 0) {
            free(d);
            continue;
        }
        libusb_ref_device(devs[i]);
        pthread_mutex_lock(&awp_lock);
        d->awp_next = awp_list;
        awp_list = d;
        pthread_mutex_unlock(&awp_lock);
        if (pthread_create(&t, NULL, awp_session, d) == 0)
            pthread_detach(t);
    }
    libusb_free_device_list(devs, 1);
}

static void awp_on_signal(int s)
{
    (void)s;
    awp_quit = 1;
}

int main(void)
{
    if (geteuid() != 0) {
        fputs("androidtetherd: must run as root\n", stderr);
        return 1;
    }
    signal(SIGTERM, awp_on_signal);
    signal(SIGINT, awp_on_signal);
    signal(SIGPIPE, SIG_IGN);
    if (libusb_init(NULL) != 0)
        return 1;
    awp_log("watching for Android USB tethering");
    while (!awp_quit) {
        awp_scan();
        sleep(2);
    }
    for (;;) {
        pthread_mutex_lock(&awp_lock);
        awp_dev *d = awp_list;
        pthread_mutex_unlock(&awp_lock);
        if (!d)
            break;
        usleep(200000);
    }
    libusb_exit(NULL);
    return 0;
}
