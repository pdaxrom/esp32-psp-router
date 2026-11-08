#include "bridge_dns.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void be16(uint8_t *p, unsigned n) { p[0] = n >> 8; p[1] = n; }
static unsigned word(const uint8_t *p) { return p[0] * 256u + p[1]; }
/* Independent wire checksum, including odd-length payloads. */
static unsigned sum(const uint8_t *p, size_t n)
{
    unsigned s = 0;
    for (size_t i = 0; i < n; ++i) s += (unsigned)p[i] << (i % 2 ? 0 : 8);
    return s;
}
static unsigned finish(unsigned s)
{
    while (s > 65535) s = (s & 65535) + (s >> 16);
    return s ^ 65535;
}
static size_t query(uint8_t *p, unsigned type)
{
    static const uint8_t q[] = {0x5a,0xc3,1,0,0,1,0,0,0,0,0,0,
                              9,'p','s','p','-','p','r','o','x','y',0,0,1,0,1};
    memcpy(p, q, sizeof(q)); be16(p + 23, type); return sizeof(q);
}
static void ip_checksum(uint8_t *f)
{
    f[24] = f[25] = 0;
    be16(f + 24, finish(sum(f + 14, (f[14] & 15) * 4)));
}
static void udp_checksum(uint8_t *f)
{
    uint8_t *u = f + 14 + (f[14] & 15) * 4;
    u[6] = u[7] = 0;
    unsigned n = word(u + 4);
    unsigned c = finish(sum(f + 26, 8) + 17 + n + sum(u, n));
    be16(u + 6, c ? c : 65535);
}
static size_t frame(uint8_t *f, unsigned ihl)
{
    memset(f, 0, 1514);
    const uint8_t ethernet[] = {0x5e,0xe9,0x1e,0x77,0x87,0x64, 0,0x23,0x4e,0x17,0x88,2, 8,0};
    memcpy(f, ethernet, 14);
    f[14] = 0x40 | (ihl / 4); f[22] = 64; f[23] = 17;
    const uint8_t addresses[] = {192,168,2,5, 192,168,2,1};
    memcpy(f + 26, addresses, 8);
    uint8_t *u = f + 14 + ihl;
    be16(u, 49152); be16(u + 2, 53);
    size_t n = query(u + 8, 1);
    be16(u + 4, 8 + n); be16(f + 16, ihl + 8 + n);
    ip_checksum(f); udp_checksum(f);
    return 14 + ihl + 8 + n;
}
static void messages(void)
{
    uint8_t q[513], r[514];
    query(q, 1);
    assert(bridge_dns_hostname("PsP-pRoXy."));
    assert(!bridge_dns_hostname(NULL));
    assert(!bridge_dns_hostname("psp-proxy.local"));
    assert(!bridge_dns_hostname("psp-proxy.evil"));
    assert(!bridge_dns_hostname("psp-prox"));
    for (size_t n = 0; n < 27; ++n) assert(!bridge_dns_message_reply(q, n, 0xc0a80204, r, sizeof(r)));
    assert(bridge_dns_message_reply(q, 27, 0xc0a80204, NULL, 0) == 43);
    memset(r, 0xa5, sizeof(r));
    assert(!bridge_dns_message_reply(q, 27, 0xc0a80204, r + 1, 42));
    for (size_t i = 0; i < sizeof(r); ++i) assert(r[i] == 0xa5);
    assert(bridge_dns_message_reply(q, 27, 0xc0a80204, r + 1, 43) == 43);
    assert(r[0] == 0xa5 && r[44] == 0xa5);
    const uint8_t answer[] = {0x5a,0xc3,0x85,0,0,1,0,1,0,0,0,0,
        9,'p','s','p','-','p','r','o','x','y',0,0,1,0,1,
        0xc0,12,0,1,0,1,0,0,0,0,0,4,192,168,2,4};
    assert(!memcmp(r + 1, answer, sizeof(answer)));
    assert(bridge_dns_message_reply(q, 27, 0x0a010203, r, sizeof(r)) == 43);
    assert(!memcmp(r + 39, "\x0a\x01\x02\x03", 4));
    q[13] = 'P';
    assert(bridge_dns_message_reply(q, 27, 0xc0a80204, r, sizeof(r)) == 43 && r[13] == 'P');
    query(q, 28);
    assert(bridge_dns_message_reply(q, 27, 1, r, sizeof(r)) == 27 && word(r + 6) == 0 && !(r[3] & 15));
    query(q, 255); assert(bridge_dns_message_reply(q, 27, 1, r, sizeof(r)) == 43);
    query(q, 1); q[11] = 1; memset(q + 27, 0xee, 11); /* EDNS suffix isn't reflected. */
    assert(bridge_dns_message_reply(q, 38, 1, r, sizeof(r)) == 43 && word(r + 10) == 0);
    const unsigned offsets[] = {2, 4, 5, 7, 9, 12, 13, 22, 26};
    const unsigned values[] = {0x81, 1, 2, 1, 1, 0xc0, 'x', 3, 3};
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(*offsets); ++i) {
        query(q, 1); q[offsets[i]] = values[i];
        assert(!bridge_dns_message_reply(q, 27, 1, r, sizeof(r)));
    }
    query(q, 1); assert(!bridge_dns_message_reply(q, 27, 0, r, sizeof(r)));
    assert(!bridge_dns_message_reply(q, 513, 1, r, sizeof(r)));
}
static void packets(void)
{
    uint8_t f[1514], r[1516];
    size_t n = frame(f, 20);
    for (size_t i = 0; i < n; ++i) assert(!bridge_dns_frame_reply(f, i, 0xc0a80204, 0xffffff00, r, sizeof(r)));
    assert(bridge_dns_frame_reply(f, n, 0xc0a80204, 0xffffff00, NULL, 0) == 85);
    memset(r, 0xa5, sizeof(r));
    assert(!bridge_dns_frame_reply(f, n, 0xc0a80204, 0xffffff00, r + 1, 84));
    assert(r[1] == 0xa5);
    assert(bridge_dns_frame_reply(f, n, 0xc0a80204, 0xffffff00, r + 1, 85) == 85);
    assert(r[0] == 0xa5 && r[86] == 0xa5);
    uint8_t *out = r + 1;
    assert(!memcmp(out, f + 6, 6) && !memcmp(out + 6, f, 6));
    assert(!memcmp(out + 26, f + 30, 4) && !memcmp(out + 30, f + 26, 4));
    assert(word(out + 16) == 71 && word(out + 34) == 53 && word(out + 36) == 49152);
    assert(word(out + 38) == 51 && !finish(sum(out + 14, 20)));
    assert(!finish(sum(out + 26, 8) + 17 + 51 + sum(out + 34, 51)));
    assert(!memcmp(out + 42 + 39, "\xc0\xa8\x02\x04", 4));
    n = frame(f, 24); /* IP options and link-layer padding must not confuse offsets. */
    assert(bridge_dns_frame_reply(f, n + 15, 0xc0a80204, 0xffffff00, r, sizeof(r)) == 85);
    n = frame(f, 20); f[40] = f[41] = 0; /* IPv4 permits omitted UDP checksum. */
    assert(bridge_dns_frame_reply(f, n, 0xc0a80204, 0xffffff00, r, sizeof(r)) == 85);
    const unsigned offset[] = {0, 6, 12, 14, 16, 20, 21, 23, 24, 26, 29, 29, 29, 30, 36, 38, 40, 55};
    const unsigned value[] = {1, 1, 0x86, 0x41, 255, 0x20, 1, 6, 1, 10, 0, 4, 255, 224, 1, 1, 1, 'x'};
    for (unsigned i = 0; i < sizeof(offset)/sizeof(*offset); ++i) {
        n = frame(f, 20); f[offset[i]] = value[i];
        if (offset[i] != 24) ip_checksum(f);
        if (offset[i] != 40 && offset[i] != 14 && offset[i] != 38) udp_checksum(f);
        assert(!bridge_dns_frame_reply(f, n, 0xc0a80204, 0xffffff00, r, sizeof(r)));
    }
    n = frame(f, 20); memset(f + 6, 0, 6);
    assert(!bridge_dns_frame_reply(f, n, 0xc0a80204, 0xffffff00, r, sizeof(r)));
    n = frame(f, 20);
    for (unsigned m = 0; m < 3; ++m) {
        const uint32_t masks[] = {0, 0xffff00ff, 0xffffffff};
        assert(!bridge_dns_frame_reply(f, n, 0xc0a80204, masks[m], r, sizeof(r)));
    }
    /* Deterministic malformed packet corpus under ASan/UBSan. */
    uint32_t rng = 7;
    for (unsigned i = 0; i < 20000; ++i) {
        n = frame(f, 20);
        rng = rng * 1664525 + 1013904223;
        size_t length = rng % sizeof(f);
        f[(rng >> 16) % sizeof(f)] ^= (rng >> 24) | 1;
        (void)bridge_dns_frame_reply(f, length, 0xc0a80204, 0xffffff00, r, sizeof(r));
        (void)bridge_dns_message_reply(f, length % 514, 0xc0a80204, r, sizeof(r));
    }
}
int main(void)
{
    messages(); packets();
    puts("PASS: bridge DNS wire replies, DHCP address changes, checksums, strict scope and malformed packets");
}
