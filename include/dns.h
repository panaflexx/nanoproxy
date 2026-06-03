#ifndef DNS_H
#define DNS_H
/*
 * dns.h — Tiny authoritative DNS responder for nanoserver.
 *
 * Handles A, AAAA, CNAME, NS, PTR, TXT, MX, SRV queries from a flat
 * name→records map loaded from config.json.  Returns NXDOMAIN for unknown
 * names and REFUSED for unsupported classes.
 *
 * Wire format: RFC 1035 §4, with name compression on the question name.
 */

#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include "config.h"
#include "socket_server.h"

/* ── DNS wire constants ──────────────────────────────────────────────── */

#define DNS_HDR_SIZE     12
#define DNS_CLASS_IN     1
#define DNS_FLAG_QR      0x8000  /* response */
#define DNS_FLAG_AA      0x0400  /* authoritative */
#define DNS_FLAG_RD      0x0100  /* recursion desired (copy from query) */
#define DNS_RCODE_OK     0
#define DNS_RCODE_NXDOMAIN 3
#define DNS_RCODE_REFUSED  5

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Decode a DNS QNAME starting at buf[off] into a dotted string.
 * Returns the number of bytes consumed from buf, or -1 on error. */
static inline int dns_decode_name(const uint8_t *buf, size_t buflen, size_t off,
                                   char *out, size_t outlen) {
    size_t pos = off, opos = 0;
    int jumps = 0;
    size_t first_end = 0; /* byte after the name in the original stream */
    while (pos < buflen) {
        uint8_t len = buf[pos];
        if (len == 0) {
            if (first_end == 0) first_end = pos + 1;
            break;
        }
        /* Pointer compression */
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= buflen) return -1;
            if (first_end == 0) first_end = pos + 2;
            size_t ptr = ((len & 0x3F) << 8) | buf[pos + 1];
            if (ptr >= buflen || ++jumps > 64) return -1;
            pos = ptr;
            continue;
        }
        if (len > 63 || pos + 1 + len > buflen) return -1;
        if (opos > 0 && opos < outlen - 1) out[opos++] = '.';
        for (uint8_t i = 0; i < len && opos < outlen - 1; i++) {
            char c = (char)buf[pos + 1 + i];
            /* Lower-case for matching */
            out[opos++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        }
        pos += 1 + len;
    }
    out[opos] = '\0';
    return (first_end > off) ? (int)(first_end - off) : -1;
}

/* Encode a dotted hostname into DNS label format at dst.
 * Returns bytes written, or -1 on error. */
static inline int dns_encode_name(const char *name, uint8_t *dst, size_t dstlen) {
    size_t pos = 0;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t llen = dot ? (size_t)(dot - p) : strlen(p);
        if (llen == 0 || llen > 63 || pos + 1 + llen >= dstlen) return -1;
        dst[pos++] = (uint8_t)llen;
        memcpy(dst + pos, p, llen);
        pos += llen;
        p += llen;
        if (*p == '.') p++;
    }
    if (pos >= dstlen) return -1;
    dst[pos++] = 0; /* root label */
    return (int)pos;
}

/* Write a 16-bit big-endian value at dst, advance pos. */
static inline void dns_put16(uint8_t *dst, size_t *pos, uint16_t val) {
    dst[(*pos)++] = (uint8_t)(val >> 8);
    dst[(*pos)++] = (uint8_t)(val & 0xFF);
}

/* Write a 32-bit big-endian value at dst, advance pos. */
static inline void dns_put32(uint8_t *dst, size_t *pos, uint32_t val) {
    dst[(*pos)++] = (uint8_t)(val >> 24);
    dst[(*pos)++] = (uint8_t)((val >> 16) & 0xFF);
    dst[(*pos)++] = (uint8_t)((val >> 8) & 0xFF);
    dst[(*pos)++] = (uint8_t)(val & 0xFF);
}

/* ── Response builder ────────────────────────────────────────────────── */

/* Build a DNS response packet.
 * query/qlen: the incoming query packet.
 * cfg: the parsed DNS config.
 * out/outmax: output buffer.
 * Returns the response length, or 0 on error. */
static inline size_t dns_build_response(const uint8_t *query, size_t qlen,
                                         const DnsConfig *cfg,
                                         uint8_t *out, size_t outmax) {
    if (qlen < DNS_HDR_SIZE || outmax < 512) return 0;

    /* Parse header */
    uint16_t id     = (query[0] << 8) | query[1];
    uint16_t flags  = (query[2] << 8) | query[3];
    uint16_t qdcount = (query[4] << 8) | query[5];
    if (qdcount == 0) return 0;

    /* Decode question name */
    char qname[256] = {0};
    int name_bytes = dns_decode_name(query, qlen, DNS_HDR_SIZE, qname, sizeof(qname));
    if (name_bytes < 0) return 0;
    size_t qoff = DNS_HDR_SIZE + (size_t)name_bytes;
    if (qoff + 4 > qlen) return 0;
    uint16_t qtype  = (query[qoff] << 8) | query[qoff + 1];
    uint16_t qclass = (query[qoff + 2] << 8) | query[qoff + 3];

    /* Only answer IN class */
    uint16_t rcode = DNS_RCODE_OK;
    if (qclass != DNS_CLASS_IN) rcode = DNS_RCODE_REFUSED;

    /* Look up the name in our config */
    const DnsEntry *entry = NULL;
    if (rcode == DNS_RCODE_OK) {
        for (int i = 0; i < cfg->num_entries; i++) {
            if (strcmp(cfg->entries[i].name, qname) == 0) {
                entry = &cfg->entries[i];
                break;
            }
        }
        if (!entry) rcode = DNS_RCODE_NXDOMAIN;
    }

    /* Start building response — copy ID, build flags */
    memset(out, 0, outmax < 512 ? outmax : 512);
    size_t pos = 0;
    /* ID */
    dns_put16(out, &pos, id);
    /* Flags: QR=1, AA=1, copy RD from query */
    uint16_t rflags = DNS_FLAG_QR | DNS_FLAG_AA | (flags & DNS_FLAG_RD) | rcode;
    dns_put16(out, &pos, rflags);
    /* QDCOUNT = 1 */
    dns_put16(out, &pos, 1);
    /* ANCOUNT — fill in later at offset 6 */
    size_t ancount_off = pos;
    dns_put16(out, &pos, 0);
    /* NSCOUNT, ARCOUNT = 0 */
    dns_put16(out, &pos, 0);
    dns_put16(out, &pos, 0);

    /* Copy question section verbatim */
    size_t q_total = (size_t)name_bytes + 4; /* name + type + class */
    if (pos + q_total > outmax) return 0;
    memcpy(out + pos, query + DNS_HDR_SIZE, q_total);
    /* Record where the question name starts (for compression pointer) */
    size_t qname_ptr_off = pos;
    pos += q_total;

    /* Build answer records */
    uint16_t ancount = 0;
    if (entry && rcode == DNS_RCODE_OK) {
        for (int i = 0; i < entry->num_records; i++) {
            const DnsRecord *rec = &entry->records[i];

            /* Filter: respond with matching type, or all types for ANY (255),
             * and always include CNAME even when queried type differs. */
            if (qtype != 255 && (uint16_t)rec->type != qtype && rec->type != DNS_CNAME)
                continue;

            /* Name pointer to the question name (compression) */
            if (pos + 2 > outmax) break;
            out[pos++] = 0xC0 | ((qname_ptr_off >> 8) & 0x3F);
            out[pos++] = qname_ptr_off & 0xFF;

            /* TYPE, CLASS, TTL */
            if (pos + 8 > outmax) break;
            dns_put16(out, &pos, (uint16_t)rec->type);
            dns_put16(out, &pos, DNS_CLASS_IN);
            dns_put32(out, &pos, rec->ttl);

            /* RDLENGTH + RDATA — reserve 2 bytes for length */
            size_t rdlen_off = pos;
            dns_put16(out, &pos, 0); /* placeholder */

            size_t rd_start = pos;
            switch (rec->type) {
            case DNS_A:
                if (pos + 4 > outmax) break;
                memcpy(out + pos, &rec->a, 4);
                pos += 4;
                break;
            case DNS_AAAA:
                if (pos + 16 > outmax) break;
                memcpy(out + pos, &rec->aaaa, 16);
                pos += 16;
                break;
            case DNS_CNAME:
            case DNS_NS:
            case DNS_PTR: {
                const char *hname = (rec->type == DNS_CNAME) ? rec->cname
                                  : (rec->type == DNS_NS) ? rec->ns : rec->ptr;
                int nlen = dns_encode_name(hname, out + pos, outmax - pos);
                if (nlen < 0) break;
                pos += (size_t)nlen;
                break;
            }
            case DNS_MX: {
                if (pos + 2 > outmax) break;
                dns_put16(out, &pos, rec->mx.priority);
                int nlen = dns_encode_name(rec->mx.host, out + pos, outmax - pos);
                if (nlen < 0) break;
                pos += (size_t)nlen;
                break;
            }
            case DNS_TXT: {
                /* TXT RDATA: one or more <length><string> chunks (max 255 each) */
                size_t tlen = strlen(rec->txt);
                const char *tp = rec->txt;
                while (tlen > 0) {
                    size_t chunk = tlen > 255 ? 255 : tlen;
                    if (pos + 1 + chunk > outmax) break;
                    out[pos++] = (uint8_t)chunk;
                    memcpy(out + pos, tp, chunk);
                    pos += chunk;
                    tp += chunk;
                    tlen -= chunk;
                }
                break;
            }
            case DNS_SRV: {
                if (pos + 6 > outmax) break;
                dns_put16(out, &pos, rec->srv.priority);
                dns_put16(out, &pos, rec->srv.weight);
                dns_put16(out, &pos, rec->srv.port);
                int nlen = dns_encode_name(rec->srv.target, out + pos, outmax - pos);
                if (nlen < 0) break;
                pos += (size_t)nlen;
                break;
            }
            default:
                break;
            }

            /* Patch RDLENGTH */
            uint16_t rdlen = (uint16_t)(pos - rd_start);
            out[rdlen_off]     = (uint8_t)(rdlen >> 8);
            out[rdlen_off + 1] = (uint8_t)(rdlen & 0xFF);
            ancount++;
        }
    }

    /* Patch ANCOUNT in header */
    out[ancount_off]     = (uint8_t)(ancount >> 8);
    out[ancount_off + 1] = (uint8_t)(ancount & 0xFF);

    return pos;
}

/* ── Socket handler ──────────────────────────────────────────────────── */

/* Global pointer to the DNS config — set from server.c after loading. */
static const DnsConfig *g_dns_config = NULL;

/* DNS socket handler: receives raw UDP, builds response, sends back. */
static inline void dns_socket_handler(int fd, const char *data, size_t len,
                                       struct client_info *info) {
    if (!g_dns_config || len < DNS_HDR_SIZE) return;
    (void)info;

    uint8_t resp[4096];
    size_t rlen = dns_build_response((const uint8_t *)data, len,
                                      g_dns_config, resp, sizeof(resp));
    if (rlen > 0) {
        socket_write(fd, resp, rlen);
    }
}

#endif /* DNS_H */
