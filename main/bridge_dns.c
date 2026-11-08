#include "bridge_dns.h"
#include <string.h>

static uint16_t get16(const uint8_t *p) { return (uint16_t)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
static uint8_t lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

bool bridge_dns_hostname(const char *name)
{
    if (!name) return false;
    size_t n = strlen(name);
    if (n == 10 && name[9] == '.') --n;
    if (n != 9) return false;
    for (size_t i = 0; i < n; ++i) if (lower((uint8_t)name[i]) != PSP_PROXY_HOSTNAME[i]) return false;
    return true;
}

static uint32_t sum_bytes(uint32_t sum, const uint8_t *p, size_t n)
{
    while (n > 1) { sum += get16(p); p += 2; n -= 2; }
    if (n) sum += (uint16_t)p[0] << 8;
    return sum;
}

static uint16_t checksum(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

size_t bridge_dns_message_reply(const uint8_t *q, size_t length, uint32_t ip,
                                uint8_t *r, size_t capacity)
{
    /* A single uncompressed question for psp-proxy. Strip EDNS/additional
     * records rather than echoing untrusted suffixes into the response.
     */
    if (!q || length < 27 || length > 512 || !ip ||
        (get16(q + 2) & 0xfa00) || get16(q + 4) != 1 ||
        get16(q + 6) || get16(q + 8) || q[12] != 9 || q[22] || get16(q + 25) != 1) return 0;
    for (unsigned i = 0; i < 9; ++i) if (lower(q[13 + i]) != PSP_PROXY_HOSTNAME[i]) return 0;
    bool answer = get16(q + 23) == 1 || get16(q + 23) == 255;
    size_t size = 27 + (answer ? 16 : 0);
    if (!r) return size;
    if (capacity < size) return 0;
    memcpy(r, q, 27);
    put16(r + 2, 0x8400 | (get16(q + 2) & 0x0100)); /* QR, authoritative, preserve RD. */
    put16(r + 6, answer ? 1 : 0);
    put16(r + 8, 0);
    put16(r + 10, 0);
    if (answer) {
        put16(r + 27, 0xc00c);
        put16(r + 29, 1);
        put16(r + 31, 1);
        put32(r + 33, 0); /* Never cache a previous DHCP address. */
        put16(r + 37, 4);
        put32(r + 39, ip);
    }
    return size;
}

size_t bridge_dns_frame_reply(const uint8_t *f, size_t length, uint32_t ip,
                              uint32_t mask, uint8_t *r, size_t capacity)
{
    if (!f || length < 42 || length > 1514 || get16(f + 12) != 0x0800 ||
        (f[0] & 1) || (f[6] & 1) || !(f[6] | f[7] | f[8] | f[9] | f[10] | f[11]) || !ip || !mask) return 0;
    const uint8_t *in = f + 14;
    uint32_t host_bits = ~mask;
    if ((host_bits & (host_bits + 1)) || host_bits < 3) return 0;
    size_t ihl = (in[0] & 15) * 4;
    if ((in[0] >> 4) != 4 || ihl < 20 || ihl > 60 || length < 14 + ihl + 8 ||
        in[9] != 17 || (get16(in + 6) & 0x3fff) || checksum(sum_bytes(0, in, ihl))) return 0;
    size_t total = get16(in + 2);
    if (total < ihl + 8 || total > length - 14) return 0;
    uint32_t client = get32(in + 12), server = get32(in + 16);
    if (!client || client == ip || (client & mask) != (ip & mask) ||
        client == (ip & mask) || client == ((ip & mask) | ~mask) ||
        !server || server >= 0xe0000000) return 0;
    const uint8_t *udp = in + ihl;
    size_t ulen = get16(udp + 4);
    if (!get16(udp) || get16(udp + 2) != 53 || ulen < 8 || ulen != total - ihl) return 0;
    uint32_t pseudo = sum_bytes(0, in + 12, 8) + 17 + ulen;
    if (get16(udp + 6) && checksum(sum_bytes(pseudo, udp, ulen))) return 0;
    size_t dns = bridge_dns_message_reply(udp + 8, ulen - 8, ip, NULL, 0);
    size_t size = 42 + dns;
    if (!dns) return 0;
    if (!r) return size;
    if (capacity < size) return 0;
    memcpy(r, f + 6, 6);
    memcpy(r + 6, f, 6);
    put16(r + 12, 0x0800);
    uint8_t *out = r + 14;
    memset(out, 0, 28);
    out[0] = 0x45;
    put16(out + 2, 28 + dns);
    memcpy(out + 4, in + 4, 2);
    out[8] = 64;
    out[9] = 17;
    put32(out + 12, server);
    put32(out + 16, client);
    put16(out + 10, checksum(sum_bytes(0, out, 20)));
    uint8_t *reply_udp = out + 20;
    put16(reply_udp, 53);
    memcpy(reply_udp + 2, udp, 2);
    put16(reply_udp + 4, 8 + dns);
    bridge_dns_message_reply(udp + 8, ulen - 8, ip, reply_udp + 8, capacity - 42);
    uint16_t check = checksum(sum_bytes(sum_bytes(0, out + 12, 8) + 17 + 8 + dns,
                                         reply_udp, 8 + dns));
    put16(reply_udp + 6, check ? check : 0xffff);
    return size;
}
