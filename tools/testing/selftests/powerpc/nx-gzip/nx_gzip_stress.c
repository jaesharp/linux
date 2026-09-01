// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NX-GZIP under every architected position, error condition and isolation
 * boundary the "P9 NX Gzip Accelerator" software interface describes.
 *
 * Positions. A decompression may be suspended anywhere in the stream, and
 * Table 5-3 (section 5.2.5.5) enumerates where: inside a literal, fixed or
 * dynamic Huffman block with BFINAL 0 or 1, inside a three bit block code,
 * inside a block header, or at an end-of-block with the next code not yet
 * seen. Each is reported through the Source Final Block Type and the Source
 * Unprocessed Bit Count, and each has its own resume rule (section 5.2.5.4).
 * Rather than construct one case per row, a multi-block stream is truncated at
 * every byte length there is, every suspension is classified by the row it
 * lands in, and every one is resumed and its output compared with the original
 * data. The rows a stream never lands in are reported, so the coverage is
 * visible rather than assumed.
 *
 * Function codes. Table 6-2: compression with fixed and dynamic tables, with
 * and without LZ77 counts, the resume forms with and without history seeding,
 * decompression, decompression one block at a time, and wrap. The dynamic
 * table is built here, complete over both alphabets, because a table built by
 * zlib for the same data is not guaranteed to cover the symbols this engine's
 * match finder produces (section 2.5.9.6).
 *
 * Error conditions. Table 5-1: too little and too much source, an undefined
 * Huffman code, an invalid block header, an invalid distance and an invalid
 * dynamic table are each induced with a hand-encoded stream and the completion
 * code is asserted. Translation faults are induced with unmapped, unreadable,
 * unwritable and kernel addresses and must come back as CC 250 with the
 * failing address, never as anything else and never as a kernel message.
 *
 * Isolation. A window belongs to one address space. Another process's address
 * submitted through it must fault, not translate. A child of a process with an
 * open window inherits the paste mapping; what it can and cannot do with it is
 * measured and the parent's memory is checked afterwards. An exec replaces the
 * address space under an open window. Windows opened and closed in a loop
 * share one hardware PID, which must outlive each of them.
 *
 * Every data-carrying request is checked against zlib: the engine's CRC-32 and
 * Adler-32 against zlib's over the same bytes, and every stream the engine
 * produces inflated by zlib and compared with the source. That is the property
 * a translation bug breaks first, and the one this file exists to guard.
 *
 * Modes:
 *   nx_gzip_stress            the harness: positions, functions, errors,
 *                             isolation, in one process
 *   nx_gzip_stress worker N   N iterations of window open, random job, verify,
 *                             window close; for the multi-process driver
 *   nx_gzip_stress inflight   one 16 MiB job, for being killed underneath
 *   nx_gzip_stress exec-child internal, the image an exec case runs
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <endian.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <zlib.h>
#include "vas-api.h"

#include "utils.h"
#include "nxu.h"
#include "nx.h"
#include "copy-paste.h"

#define CC_TIMEOUT	(-1)	/* no completion within the deadline */
#define CC_NOPASTE	(-2)	/* the paste never took */

#define hwsync()	({ asm volatile("sync" ::: "memory"); })

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int nx_dbg;
FILE *nx_gzip_log;

#define KiB(x)		((x) << 10)
#define MiB(x)		((x) << 20)
#define HIST_MAX	KiB(32)		/* the deflate window, section 2.5.7 */
#define QW		16

/* Completion extension bit the kernel sets when it reports a fault. */
#define CSB_CE_TERMINATION	0x40

static long pagesz;

/* ------------------------------------------------------------------------ */
/* Data generators                                                          */

static void fill_lfsr(unsigned char *p, size_t len, unsigned int taps,
		      unsigned int seed)
{
	unsigned int state = seed ? seed : 1;
	size_t i;

	for (i = 0; i < len; i++) {
		unsigned int lsb = state & 1u;

		state >>= 1;
		if (lsb)
			state ^= taps;
		p[i] = (unsigned char)(state & 0xffu);
	}
}

/* Text-like: compressible, with matches at many distances. */
static void fill_text(unsigned char *p, size_t len, unsigned int seed)
{
	static const char * const words[] = {
		"segment ", "table ", "entry ", "nest ", "mmu ", "walks ",
		"the ", "process ", "it ", "selects ", "and ", "hashes ",
		"a ", "virtual ", "address ", "into ", "page ", "groups ",
		"slbiag ", "slbieg ", "ptesync ", "tlbie ", "window ", "paste ",
	};
	unsigned int s = seed | 1;
	size_t i = 0;

	while (i < len) {
		const char *w;

		s = s * 1103515245u + 12345u;
		w = words[(s >> 16) % (ARRAY_SIZE(words))];
		while (*w && i < len)
			p[i++] = (unsigned char)*w++;
	}
}

/* Incompressible: the engine must report TPBC > SPBC, never a bad stream. */
static void fill_random(unsigned char *p, size_t len, unsigned int seed)
{
	uint64_t x = 0x9e3779b97f4a7c15ull ^ ((uint64_t)seed << 32 | seed);
	size_t i;

	for (i = 0; i < len; i++) {
		x ^= x >> 12;
		x ^= x << 25;
		x ^= x >> 27;
		p[i] = (unsigned char)((x * 0x2545f4914f6cdd1dull) >> 56);
	}
}

/* ------------------------------------------------------------------------ */
/* A deflate bit writer, for hand-encoded streams and the dynamic table.    */
/* Deflate packs bits LSB first, except that Huffman codes are packed from  */
/* their most significant bit, so a code is reversed before it is written.  */

struct bitw {
	unsigned char *buf;
	size_t cap, pos;	/* pos in bits */
};

static void bw_init(struct bitw *w, unsigned char *buf, size_t cap)
{
	w->buf = buf;
	w->cap = cap;
	w->pos = 0;
	memset(buf, 0, cap);
}

static void bw_bits(struct bitw *w, unsigned int v, int n)
{
	int i;

	for (i = 0; i < n; i++, w->pos++) {
		if ((v >> i) & 1u)
			w->buf[w->pos >> 3] |= (unsigned char)(1u << (w->pos & 7));
	}
}

static void bw_code(struct bitw *w, unsigned int code, int n)
{
	unsigned int r = 0;
	int i;

	for (i = 0; i < n; i++)
		r |= ((code >> i) & 1u) << (n - 1 - i);
	bw_bits(w, r, n);
}

static size_t bw_bytes(const struct bitw *w)
{
	return (w->pos + 7) >> 3;
}

/*
 * Canonical Huffman codes from lengths, RFC 1951 section 3.2.2.
 */
static void canon_codes(const unsigned char *len, unsigned int *code, int n)
{
	unsigned int bl_count[16] = {}, next[16] = {};
	int i, bits;

	for (i = 0; i < n; i++)
		bl_count[len[i]]++;
	bl_count[0] = 0;
	for (bits = 1; bits < 16; bits++)
		next[bits] = (next[bits - 1] + bl_count[bits - 1]) << 1;
	for (i = 0; i < n; i++)
		code[i] = len[i] ? next[len[i]]++ : 0;
}

/*
 * Fixed Huffman code (RFC 1951 3.2.6), for hand-encoded fixed blocks.
 */
static void fixed_lit(unsigned int sym, unsigned int *code, int *n)
{
	if (sym < 144) {
		*code = 0x30 + sym; *n = 8;
	} else if (sym < 256) {
		*code = 0x190 + (sym - 144); *n = 9;
	} else if (sym < 280) {
		*code = sym - 256; *n = 7;
	} else {
		*code = 0xc0 + (sym - 280); *n = 8;
	}
}

/*
 * A complete dynamic Huffman table over all 286 literal/length and all 30
 * distance symbols, and its deflate encoding as the engine wants it in the
 * CPB: everything after the three bit block header, HLIT through the last
 * code length. 226 literal/length symbols of length 8 and 60 of length 9 is
 * exactly complete (226/256 + 60/512 = 1), as is 2 distance codes of length 4
 * and 28 of length 5. The code length code needs only the four lengths 4, 5,
 * 8 and 9, given two bits each; symbol 4 is twelfth in the transmission
 * order, which sets HCLEN.
 */
#define NLIT	286
#define NDIST	30

struct dht {
	unsigned char ll_len[NLIT], d_len[NDIST];
	unsigned char bits[DHT_MAXSZ];
	int nbits;
};

static void build_flat_dht(struct dht *d)
{
	static const unsigned char clorder[19] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
	};
	unsigned char cl_len[19] = {};
	unsigned int cl_code[19];
	struct bitw w;
	int i;

	for (i = 0; i < NLIT; i++)
		d->ll_len[i] = i < 226 ? 8 : 9;
	for (i = 0; i < NDIST; i++)
		d->d_len[i] = i < 2 ? 4 : 5;

	cl_len[4] = 2;
	cl_len[5] = 2;
	cl_len[8] = 2;
	cl_len[9] = 2;
	canon_codes(cl_len, cl_code, 19);

	bw_init(&w, d->bits, sizeof(d->bits));
	bw_bits(&w, NLIT - 257, 5);		/* HLIT  */
	bw_bits(&w, NDIST - 1, 5);		/* HDIST */
	bw_bits(&w, 12 - 4, 4);			/* HCLEN: through symbol 4 */
	for (i = 0; i < 12; i++)
		bw_bits(&w, cl_len[clorder[i]], 3);
	for (i = 0; i < NLIT; i++)
		bw_code(&w, cl_code[d->ll_len[i]], 2);
	for (i = 0; i < NDIST; i++)
		bw_code(&w, cl_code[d->d_len[i]], 2);
	d->nbits = (int)w.pos;
}

/* ------------------------------------------------------------------------ */
/* Reference streams from zlib                                              */

/*
 * A raw deflate stream (no zlib header) whose block types and boundaries are
 * chosen: a sequence of segments, each emitted as its own block of the given
 * kind, separated by Z_FULL_FLUSH so no block references an earlier one and
 * the stream can be cut into blocks freely. zlib's block-type selection is
 * forced through the level and strategy: level 0 stores, Z_FIXED fixes,
 * default is dynamic.
 */
enum blk { BLK_STORED, BLK_FIXED, BLK_DYNAMIC };

static size_t zlib_multiblock(const unsigned char *src, size_t len,
			      const enum blk *kinds, int nkinds,
			      unsigned char *out, size_t outcap)
{
	z_stream s = {};
	size_t per = len / nkinds, off = 0;
	int i, rc;

	rc = deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
			  Z_DEFAULT_STRATEGY);
	if (rc != Z_OK)
		return 0;
	s.next_out = out;
	s.avail_out = outcap;
	for (i = 0; i < nkinds; i++) {
		int level = kinds[i] == BLK_STORED ? 0 : 6;
		int strat = kinds[i] == BLK_FIXED ? Z_FIXED : Z_DEFAULT_STRATEGY;
		size_t n = i == nkinds - 1 ? len - off : per;

		rc = deflateParams(&s, level, strat);
		if (rc != Z_OK)
			return 0;
		s.next_in = (unsigned char *)src + off;
		s.avail_in = n;
		rc = deflate(&s, i == nkinds - 1 ? Z_FINISH : Z_FULL_FLUSH);
		if (rc != Z_OK && rc != Z_STREAM_END)
			return 0;
		off += n;
	}
	deflateEnd(&s);
	return outcap - s.avail_out;
}

static int zlib_inflate_raw(const unsigned char *src, size_t len,
			    unsigned char *out, size_t *outlen)
{
	z_stream s = {};
	int rc;

	if (inflateInit2(&s, -15) != Z_OK)
		return -1;
	s.next_in = (unsigned char *)src;
	s.avail_in = len;
	s.next_out = out;
	s.avail_out = *outlen;
	rc = inflate(&s, Z_FINISH);
	*outlen = *outlen - s.avail_out;
	inflateEnd(&s);
	return rc == Z_STREAM_END ? 0 : -1;
}

/* ------------------------------------------------------------------------ */
/* Requests                                                                 */

struct job {
	struct nx_gzip_crb_cpb_t *cmd;
	int no_retry;	/* a fault is the expected answer here */
	struct nx_dde_t sddl[MAX_DDE_COUNT + 1] __aligned(128);
	struct nx_dde_t tddl[MAX_DDE_COUNT + 1] __aligned(128);
	int cc;
};

static struct job *job_new(void)
{
	struct job *j = calloc(1, sizeof(*j));

	if (!j)
		return NULL;
	j->cmd = aligned_alloc(sizeof(*j->cmd), sizeof(*j->cmd));
	if (!j->cmd) {
		free(j);
		return NULL;
	}
	memset(j->cmd, 0, sizeof(*j->cmd));
	return j;
}

static void job_free(struct job *j)
{
	free(j->cmd);
	free(j);
}

static void job_reset(struct job *j)
{
	memset(&j->cmd->crb, 0, sizeof(j->cmd->crb));
	memset(&j->cmd->cpb, 0, sizeof(j->cmd->cpb));
	clearp_dde(j->sddl);
	clearp_dde(j->tddl);
	put32(j->cmd->cpb, in_adler, INIT_ADLER);
	put32(j->cmd->cpb, in_crc, INIT_CRC);
}

/*
 * Paste and wait, with a deadline. The library's own submit polls for what
 * amounts to hours, which turns a request that never completes into a test
 * that never finishes; here it is a completion code of its own.
 */
struct nx_handle { int fd; int function; void *paste_addr; };

static int submit_bounded(struct nx_gzip_crb_cpb_t *c, void *handle,
			  unsigned int ms)
{
	struct nx_handle *h = handle;
	double t0, deadline = ms / 1000.0;
	int i, ret = -1;

	for (i = 0; i < 1000; i++) {
		hwsync();
		vas_copy(&c->crb, 0);
		ret = vas_paste(h->paste_addr, 0);
		hwsync();
		if (ret == 2 || ret == 3)
			break;
		usleep(10);
	}
	if (ret != 2 && ret != 3)
		return CC_NOPASTE;

	t0 = now();
	for (i = 0; getnn(c->crb.csb, csb_v) == 0; i++) {
		hwsync();
		if ((i & 0xfff) == 0xfff) {
			double el = now() - t0;

			if (el > deadline)
				return CC_TIMEOUT;
			if (el > 0.0005)
				usleep(50);
		}
	}
	hwsync();
	return getnn(c->crb.csb, csb_cc);
}

static unsigned int job_deadline_ms = 20000;

/*
 * How many times a request reporting a translation fault is resubmitted.
 *
 * CC 250 is not a failure. Section 5.2.4 calls it a partial terminate: the
 * accelerator could not translate an address, the kernel is told where, and
 * the request is the caller's to reissue. On a hash MMU a buffer in a segment
 * the mm has not driven an accelerator into before takes one of these while
 * the kernel gives the segment a table entry, so a caller that does not retry
 * sees a first-use failure for every new mapping.
 */
#define FAULT_RETRIES	16

static int nfaults;		/* how many were seen across the run */

static const char *cc_str(int cc)
{
	static char buf[32];

	if (cc == CC_TIMEOUT)
		return "no completion";
	if (cc == CC_NOPASTE)
		return "paste refused";
	snprintf(buf, sizeof(buf), "cc %d", cc);
	return buf;
}

/*
 * Submit with the DDE lists as built. The CSB is inside the CRB, as every
 * sibling test places it; NX writes it there on completion.
 */
static uint64_t job_fsaddr(struct job *j);

static int job_run(struct job *j, void *handle, unsigned int fc)
{
	struct nx_gzip_crb_cpb_t *c = j->cmd;
	uint64_t csbaddr;
	int i;

	memset((void *)&c->crb.csb, 0, sizeof(c->crb.csb));
	c->crb.source_dde = j->sddl[0];
	c->crb.target_dde = j->tddl[0];
	csbaddr = ((uint64_t)&c->crb.csb) & csb_address_mask;
	put64(c->crb, csb_address, csbaddr);
	c->crb.gzip_fc = 0;
	putnn(c->crb, gzip_fc, fc);
	c->cpb.out_spbc_comp_wrap = 0;
	c->cpb.out_spbc_comp_with_count = 0;
	c->cpb.out_spbc_decomp = 0;
	put32(c->cpb, out_crc, INIT_CRC);
	put32(c->cpb, out_adler, INIT_ADLER);

	for (i = 0; i <= FAULT_RETRIES; i++) {
		j->cc = submit_bounded(c, handle, job_deadline_ms);
		if (j->cc != ERR_NX_AT_FAULT || j->no_retry)
			break;
		nfaults++;
		/*
		 * Resubmit without touching the reported address. The kernel
		 * resolved it from the fault window before the CSB was
		 * written, which is what that path is for, and the address
		 * belongs to whichever mm the window names -- not necessarily
		 * this one, as the isolation cases submit addresses that are
		 * deliberately another process's. Only the CSB is cleared:
		 * the engine writes its valid bit and nothing else.
		 */
		memset((void *)&c->crb.csb, 0, sizeof(c->crb.csb));
	}
	return j->cc;
}

static uint32_t job_tpbc(struct job *j)
{
	return get32(j->cmd->crb.csb, tpbc);
}

static uint64_t job_fsaddr(struct job *j)
{
	return get64(j->cmd->crb.csb, fsaddr);
}

static unsigned int job_sfbt(struct job *j)
{
	return getnn(j->cmd->cpb, out_sfbt);
}

static unsigned int job_subc(struct job *j)
{
	return getnn(j->cmd->cpb, out_subc);
}

static unsigned int job_tebc(struct job *j)
{
	return getnn(j->cmd->cpb, out_tebc);
}

static uint32_t job_spbc_decomp(struct job *j)
{
	return get32(j->cmd->cpb, out_spbc_decomp);
}

static int checksums_ok(struct job *j, const unsigned char *data, size_t len)
{
	uint32_t crc = crc32(0L, Z_NULL, 0), adl = adler32(0L, Z_NULL, 0);

	crc = crc32(crc, data, len);
	adl = adler32(adl, data, len);
	if (get_cpb_crc32(j->cmd->cpb) != crc ||
	    get_cpb_adler32(j->cmd->cpb) != adl) {
		printf("  checksum mismatch over %zu bytes:\n", len);
		printf("    engine crc %08x adler %08x, zlib crc %08x adler %08x\n",
		       get_cpb_crc32(j->cmd->cpb),
		       get_cpb_adler32(j->cmd->cpb), crc, adl);
		return 0;
	}
	return 1;
}

static void describe_failure(struct job *j, const char *what)
{
	printf("  %s: cc %d ce %02x fsaddr %016llx tpbc %u sfbt %x subc %u\n",
	       what, j->cc, get_csb_ce(j->cmd->crb.csb),
	       (unsigned long long)job_fsaddr(j), job_tpbc(j), job_sfbt(j),
	       job_subc(j));
}

/*
 * Append a buffer to a DDE list in nfrag pieces of uneven size, so that a
 * scatter list of any shape is exercised; nfrag 1 is a direct descriptor.
 */
static void add_frags(struct nx_dde_t *ddl, unsigned char *buf, size_t len,
		      int nfrag)
{
	size_t off = 0;
	int i;

	if (nfrag <= 1 || len < (size_t)nfrag) {
		nx_append_dde(ddl, buf, len);
		return;
	}
	for (i = 0; i < nfrag; i++) {
		size_t n = i == nfrag - 1 ? len - off
			   : (len / nfrag) + ((i * 7919) % 13) - 6;

		if (n > len - off)
			n = len - off;
		nx_append_dde(ddl, buf + off, n);
		off += n;
		if (off >= len)
			break;
	}
}

/* ------------------------------------------------------------------------ */
/* Placements: where the buffers sit in the address space                   */

enum place {
	PL_HEAP,	/* whatever malloc gives: the 1TB segment musl maps in */
	PL_LOW,		/* a 256MB segment, below 1TB */
	PL_SEG256,	/* straddling a 256MB segment boundary */
	PL_SEG1T,	/* straddling the 1TB boundary: segment size changes */
	PL_PAGE,	/* starting one byte before a page boundary */
	PL_NPLACE
};

static const char * const place_name[PL_NPLACE] = {
	"heap", "256MB segment", "across 256MB boundary", "across 1TB boundary",
	"page straddle"
};

/*
 * Map len bytes so that the returned pointer is where the placement says.
 * MAP_FIXED_NOREPLACE, so a placement that is already occupied is skipped
 * rather than clobbering something.
 */
static unsigned char *place_map(enum place pl, size_t len, void **map,
				size_t *maplen)
{
	unsigned long base;
	size_t total;
	void *p;

	switch (pl) {
	case PL_HEAP:
		*maplen = len;
		*map = malloc(len);
		return *map;
	case PL_LOW:
		base = 0x20000000ul;
		break;
	case PL_SEG256:
		base = 0x30000000ul - (len / 2 & ~(pagesz - 1));
		break;
	case PL_SEG1T:
		base = (1ul << 40) - (len / 2 & ~(pagesz - 1));
		break;
	case PL_PAGE:
		base = 0x28000000ul;
		break;
	default:
		return NULL;
	}
	total = (len + 2 * pagesz + pagesz - 1) & ~(pagesz - 1);
	p = mmap((void *)base, total, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (p == MAP_FAILED)
		return NULL;
	*map = p;
	*maplen = total;
	if (pl == PL_PAGE)
		return (unsigned char *)p + pagesz - 1;
	return p;
}

static void place_unmap(enum place pl, void *map, size_t maplen)
{
	if (pl == PL_HEAP)
		free(map);
	else
		munmap(map, maplen);
}

/* ------------------------------------------------------------------------ */
/* Section 1: function codes, data kinds, placements, descriptor shapes     */

static int round_trip_fht(void *handle, unsigned char *src, size_t len,
			  unsigned char *dst, size_t dstlen, int nfrag,
			  unsigned int fc, const struct dht *dht,
			  unsigned char *ref, int expect_grows)
{
	struct job *j = job_new();
	size_t outlen = len;
	int rc = -1;

	if (!j)
		return -1;
	job_reset(j);
	if (dht) {
		memcpy(j->cmd->cpb.in_dht_char, dht->bits, (dht->nbits + 7) / 8);
		putnn(j->cmd->cpb, in_dhtlen, dht->nbits);
	}
	add_frags(j->sddl, src, len, nfrag);
	add_frags(j->tddl, dst, dstlen, nfrag);
	nxu_touch_pages(src, len, pagesz, 0);
	nxu_touch_pages(dst, dstlen, pagesz, 1);
	job_run(j, handle, fc);

	if (j->cc == ERR_NX_TPBC_GT_SPBC && expect_grows) {
		/* Output larger than input: the architected answer. */
		rc = 0;
		goto out;
	}
	if (j->cc != ERR_NX_OK) {
		describe_failure(j, "compress");
		goto out;
	}
	if (!checksums_ok(j, src, len))
		goto out;
	if (get32(j->cmd->cpb, out_spbc_comp) != len &&
	    get32(j->cmd->cpb, out_spbc_comp_with_count) != len) {
		printf("  spbc does not cover the source\n");
		goto out;
	}
	/*
	 * The engine leaves BFINAL clear (section 5.2.5.1); set it so zlib
	 * sees a complete stream, then inflate and compare.
	 */
	dst[0] |= 1;
	if (zlib_inflate_raw(dst, job_tpbc(j), ref, &outlen) || outlen != len ||
	    memcmp(ref, src, len)) {
		printf("  zlib cannot inflate the engine's stream back to the source (tpbc %u)\n",
		       job_tpbc(j));
		goto out;
	}
	if (fc == GZIP_FC_COMPRESS_FHT_COUNT || fc == GZIP_FC_COMPRESS_DHT_COUNT) {
		uint32_t eob = be32toh(j->cmd->cpb.out_lzcount[256]);

		if (eob != 1) {
			printf("  LZ count for end-of-block is %u, expected 1\n", eob);
			goto out;
		}
	}
	rc = 0;
out:
	job_free(j);
	return rc;
}

/* One cell of the sweep below: a function code, a data kind, a placement, a
 * size and a descriptor shape.
 */
struct fn_case {
	const char *name;
	unsigned int fc;
	int dht;
};

static const struct fn_case fn_cases[] = {
	{ "compress FHT", GZIP_FC_COMPRESS_FHT, 0 },
	{ "compress DHT", GZIP_FC_COMPRESS_DHT, 1 },
	{ "compress FHT with counts", GZIP_FC_COMPRESS_FHT_COUNT, 0 },
	{ "compress DHT with counts", GZIP_FC_COMPRESS_DHT_COUNT, 1 },
};

static const size_t fn_sizes[] = { 1, 7, 255, 4096, KiB(64) + 1, MiB(1) };
static const int fn_frags[] = { 1, 3, 16 };
static const char * const fn_kinds[] = { "text", "lfsr", "random" };

/*
 * Returns 1 if the case ran, 0 if the placement was not available, and sets
 * *failed when the request or its verification did not come out right.
 */
static int run_fn_case(void *handle, const struct dht *dht, unsigned char *ref,
		       unsigned int f, unsigned int k, enum place pl,
		       unsigned int s, unsigned int fr, int *failed)
{
	size_t len = fn_sizes[s], dstlen = 2 * len + 1024, smaplen, dmaplen;
	unsigned char *src, *dst;
	void *smap, *dmap;
	int rc;

	src = place_map(pl, len, &smap, &smaplen);
	if (!src)
		return 0;		/* placement occupied on this box */
	dst = place_map(PL_HEAP, dstlen, &dmap, &dmaplen);
	if (!dst) {
		place_unmap(pl, smap, smaplen);
		return 0;
	}

	if (k == 0)
		fill_text(src, len, 7);
	else if (k == 1)
		fill_lfsr(src, len, 0x6000u, 1);
	else
		fill_random(src, len, 3);

	rc = round_trip_fht(handle, src, len, dst, dstlen, fn_frags[fr],
			    fn_cases[f].fc, fn_cases[f].dht ? dht : NULL, ref,
			    k == 2 || len < 256);
	if (rc) {
		(*failed)++;
		printf("  FAIL %s, %s data, %s, %zu bytes, %d fragment(s)\n",
		       fn_cases[f].name, fn_kinds[k], place_name[pl], len,
		       fn_frags[fr]);
	}
	place_unmap(PL_HEAP, dmap, dmaplen);
	place_unmap(pl, smap, smaplen);
	return 1;
}

static int test_functions(void *handle)
{
	unsigned int f, k, p, s, fr;
	int ncase = 0, nfail = 0;
	unsigned char *ref;
	struct dht dht;

	build_flat_dht(&dht);
	ref = malloc(MiB(1));
	FAIL_IF(!ref);

	for (f = 0; f < ARRAY_SIZE(fn_cases); f++) {
		for (k = 0; k < ARRAY_SIZE(fn_kinds); k++) {
			for (p = 0; p < PL_NPLACE; p++) {
				for (s = 0; s < ARRAY_SIZE(fn_sizes); s++) {
					for (fr = 0; fr < ARRAY_SIZE(fn_frags); fr++)
						ncase += run_fn_case(handle,
								     &dht, ref,
								     f, k, p, s,
								     fr,
								     &nfail);
				}
			}
		}
	}
	printf("functions: %d cases, %d failed, %d translation fault(s) retried\n",
	       ncase, nfail, nfaults);
	free(ref);
	FAIL_IF(nfail);
	return 0;
}

/* Wrap: copy with checksums, section 5.2.5.7. */
static int test_wrap(void *handle)
{
	static const size_t sizes[] = { 1, 16, 4095, 4097, KiB(64), MiB(4) };
	unsigned char *src, *dst;
	struct job *j = job_new();
	int nfail = 0;
	unsigned int s, fr;

	FAIL_IF(!j);
	src = malloc(MiB(4));
	dst = malloc(MiB(4) + pagesz);
	FAIL_IF(!src || !dst);
	fill_text(src, MiB(4), 11);
	for (s = 0; s < ARRAY_SIZE(sizes); s++) {
		for (fr = 1; fr <= 16; fr *= 4) {
			size_t len = sizes[s];
			unsigned char *d = dst + (s % 7);	/* odd target alignment */

			memset(dst, 0xee, MiB(4) + pagesz);
			job_reset(j);
			add_frags(j->sddl, src, len, fr);
			add_frags(j->tddl, d, len, fr);
			nxu_touch_pages(dst, len + pagesz, pagesz, 1);
			job_run(j, handle, GZIP_FC_WRAP);
			if (j->cc != ERR_NX_OK || job_tpbc(j) != len ||
			    memcmp(d, src, len) || !checksums_ok(j, src, len)) {
				describe_failure(j, "wrap");
				printf("  FAIL wrap %zu bytes, %u fragment(s)\n", len, fr);
				nfail++;
			}
		}
	}
	printf("wrap: %d failed\n", nfail);
	free(src);
	free(dst);
	job_free(j);
	FAIL_IF(nfail);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Section 2: resume compression, sections 2.7, 2.8, 5.2.5.3               */

/*
 * Compress in two jobs: the first suspends at a chosen point (by giving it
 * only part of the source), the second resumes, either seeding the history
 * from the source already compressed (HistLen quadwords read again, not
 * compressed) or not (Z_FULL_FLUSH). Between them a sync flush block byte
 * aligns the stream, Table 5-2. The concatenation must inflate to the source.
 */
static int resume_compress_once(void *handle, unsigned char *src, size_t len,
				size_t cut, int seed_history,
				unsigned char *dst, size_t dstlen,
				unsigned char *ref)
{
	struct job *j = job_new();
	size_t outlen = len, hist, histqw, pos;
	unsigned int tebc;
	uint32_t crc_out, adler_out;
	int rc = -1;

	if (!j)
		return -1;
	job_reset(j);
	nx_append_dde(j->sddl, src, cut);
	nx_append_dde(j->tddl, dst, dstlen);
	nxu_touch_pages(dst, dstlen, pagesz, 1);
	job_run(j, handle, GZIP_FC_COMPRESS_FHT);
	/*
	 * A few bytes of input produce a block larger than themselves, which
	 * the engine reports with ERR_NX_TPBC_GT_SPBC and a valid target. The
	 * stream is well formed either way; what matters is that the source
	 * was consumed.
	 */
	if (j->cc != ERR_NX_OK && j->cc != ERR_NX_TPBC_GT_SPBC) {
		describe_failure(j, "first part");
		goto out;
	}
	if (get32(j->cmd->cpb, out_spbc_comp) != cut) {
		printf("  first part consumed %u of %zu\n",
		       get32(j->cmd->cpb, out_spbc_comp), cut);
		goto out;
	}
	pos = job_tpbc(j);
	tebc = job_tebc(j);
	crc_out = j->cmd->cpb.out_crc;
	adler_out = j->cmd->cpb.out_adler;
	dst[0] &= ~1;	/* BFINAL off: more follows */

	/*
	 * Sync flush, Table 5-2: the block ends on any bit; append an empty
	 * stored block whose header starts at the first unused bit.
	 */
	if (tebc) {
		struct bitw w;
		unsigned char tail[8];

		bw_init(&w, tail, sizeof(tail));
		w.pos = tebc;
		tail[0] = dst[pos - 1] & ((1u << tebc) - 1);
		bw_bits(&w, 0, 1);	/* BFINAL */
		bw_bits(&w, 0, 2);	/* BTYPE stored */
		while (w.pos & 7)
			bw_bits(&w, 0, 1);
		bw_bits(&w, 0x0000, 16);
		bw_bits(&w, 0xffff, 16);
		memcpy(dst + pos - 1, tail, bw_bytes(&w));
		pos = pos - 1 + bw_bytes(&w);
	}

	/* The resume job: history from the source, then the rest of it. */
	hist = seed_history ? (cut < HIST_MAX ? cut : HIST_MAX) : 0;
	hist &= ~(size_t)(QW - 1);	/* leading bytes dropped, section 2.8.1 */
	histqw = hist / QW;

	job_reset(j);
	j->cmd->cpb.in_crc = crc_out;
	j->cmd->cpb.in_adler = adler_out;
	putnn(j->cmd->cpb, in_histlen, histqw);
	nx_append_dde(j->sddl, src + cut - hist, hist + (len - cut));
	nx_append_dde(j->tddl, dst + pos, dstlen - pos);
	job_run(j, handle, GZIP_FC_COMPRESS_RESUME_FHT);
	if (j->cc != ERR_NX_OK && j->cc != ERR_NX_TPBC_GT_SPBC) {
		describe_failure(j, "resume");
		goto out;
	}
	if (get32(j->cmd->cpb, out_spbc_comp) != hist + (len - cut)) {
		printf("  resume consumed %u of %zu\n",
		       get32(j->cmd->cpb, out_spbc_comp), hist + (len - cut));
		goto out;
	}
	dst[pos] |= 1;	/* BFINAL on the last block */
	pos += job_tpbc(j);

	if (!checksums_ok(j, src, len))
		goto out;
	if (zlib_inflate_raw(dst, pos, ref, &outlen) || outlen != len ||
	    memcmp(ref, src, len)) {
		printf("  resumed stream does not inflate to the source (cut %zu, hist %zu)\n",
		       cut, hist);
		goto out;
	}
	rc = 0;
out:
	job_free(j);
	return rc;
}

static int test_resume_compress(void *handle)
{
	static const size_t cuts[] = { 1, 15, 16, 17, 1000, KiB(32) - 1, KiB(32),
				       KiB(32) + 1, KiB(200) };
	size_t len = KiB(256), dstlen = 2 * len + 4096;
	unsigned char *src = malloc(len), *dst = malloc(dstlen), *ref = malloc(len);
	int nfail = 0;
	unsigned int c, h;

	FAIL_IF(!src || !dst || !ref);
	fill_text(src, len, 5);
	for (c = 0; c < ARRAY_SIZE(cuts); c++) {
		for (h = 0; h < 2; h++) {
			if (resume_compress_once(handle, src, len, cuts[c], h, dst,
						 dstlen, ref)) {
				printf("  FAIL resume compress, cut %zu, %s history\n",
				       cuts[c], h ? "seeded" : "no");
				nfail++;
			}
		}
	}
	printf("resume compress: %d failed\n", nfail);
	free(src);
	free(dst);
	free(ref);
	FAIL_IF(nfail);
	return 0;
}

/*
 * A stream of fixed Huffman blocks written back to back with no alignment
 * between them, the last one final.
 *
 * zlib cannot produce this: its flushes byte align every block, so a
 * suspension at a block boundary always lands with SUBC 0, and the rows of
 * Table 5-3 for one and two bits into the next block's three bit code are
 * never reached. Written by hand, each block ends on whatever bit its EOB
 * ends on, so the next header starts there and the byte the source is cut at
 * falls anywhere in it.
 */
static size_t handbuilt_unaligned(const unsigned char *src, size_t len,
				  int nblk, unsigned char *out, size_t cap)
{
	struct bitw w;
	size_t per = len / nblk, off = 0;
	unsigned int code;
	int b, n;

	bw_init(&w, out, cap);
	for (b = 0; b < nblk; b++) {
		size_t n_this = (b == nblk - 1) ? len - off : per;
		size_t i;

		bw_bits(&w, b == nblk - 1 ? 1 : 0, 1);	/* BFINAL */
		bw_bits(&w, 1, 2);			/* fixed Huffman */
		for (i = 0; i < n_this; i++) {
			fixed_lit(src[off + i], &code, &n);
			bw_code(&w, code, n);
		}
		fixed_lit(256, &code, &n);		/* end of block */
		bw_code(&w, code, n);
		off += n_this;
	}
	return bw_bytes(&w);
}

/* ------------------------------------------------------------------------ */
/* Section 3: every suspend position of decompression, Table 5-3            */

/*
 * The rows of Table 5-3, by SFBT and where SUBC falls.
 */
enum row {
	ROW_FINAL,		/* 0000, SUBC 0-7: final block processed */
	ROW_TOO_MUCH,		/* 0000, SUBC > 7: extra source */
	ROW_LIT0, ROW_LIT1,	/* 1000, 1001: inside a literal block */
	ROW_FHT0, ROW_FHT1,	/* 1010, 1011: inside a fixed block */
	ROW_DHT0, ROW_DHT1,	/* 1100, 1101: inside a dynamic block */
	ROW_EOB,		/* 1110, SUBC 0: block ended on a byte boundary */
	ROW_CODE0,		/* 1110, SUBC 1-2: inside the 3 bit block code */
	ROW_HDR0,		/* 1110, SUBC >= 3: inside a block header */
	ROW_CODE1,		/* 1111, SUBC 1-2 */
	ROW_HDR1,		/* 1111, SUBC >= 3 */
	ROW_INVALID,
	ROW_N
};

static const char * const row_name[ROW_N] = {
	"final block processed", "too much source",
	"in literal block, BFINAL=0", "in literal block, BFINAL=1",
	"in fixed block, BFINAL=0", "in fixed block, BFINAL=1",
	"in dynamic block, BFINAL=0", "in dynamic block, BFINAL=1",
	"block ended on byte boundary", "in 3 bit block code, BFINAL=0",
	"in block header, BFINAL=0", "in 3 bit block code, BFINAL=1",
	"in block header, BFINAL=1", "invalid combination",
};

static enum row classify(unsigned int sfbt, unsigned int subc)
{
	switch (sfbt) {
	case 0x0:
		return subc > 7 ? ROW_TOO_MUCH : ROW_FINAL;
	case 0x8:
		return subc == 0 ? ROW_LIT0 : ROW_INVALID;
	case 0x9:
		return subc == 0 ? ROW_LIT1 : ROW_INVALID;
	case 0xa:
		return subc <= 30 ? ROW_FHT0 : ROW_INVALID;
	case 0xb:
		return subc <= 30 ? ROW_FHT1 : ROW_INVALID;
	case 0xc:
		return subc <= 47 ? ROW_DHT0 : ROW_INVALID;
	case 0xd:
		return subc <= 47 ? ROW_DHT1 : ROW_INVALID;
	case 0xe:
		if (subc == 0)
			return ROW_EOB;
		if (subc <= 2)
			return ROW_CODE0;
		return subc <= 2285 ? ROW_HDR0 : ROW_INVALID;
	case 0xf:
		if (subc >= 1 && subc <= 2)
			return ROW_CODE1;
		return subc >= 3 && subc <= 2285 ? ROW_HDR1 : ROW_INVALID;
	default:
		return ROW_INVALID;
	}
}

/*
 * Decompress a stream, suspending wherever the given source lengths make it
 * suspend, and resuming per section 5.2.5.4 until the final block. Returns
 * the row of the first suspension in *first, so a caller can count coverage.
 *
 * cutlen is the length of the first job's source; every later job gets the
 * rest. A single-block FC makes the engine stop at every block boundary as
 * well.
 */
static int decompress_resuming(void *handle, const unsigned char *stream,
			       size_t slen, size_t cutlen, unsigned int fc,
			       unsigned char *out, size_t outcap,
			       const unsigned char *expect, size_t explen,
			       enum row *first, int *njobs)
{
	struct job *j = job_new();
	unsigned char *pad;
	size_t spos = 0, tpos = 0, give;
	unsigned int rfc;
	int rc = -1, jobs = 0;

	if (!j)
		return -1;
	pad = malloc(QW);
	if (!pad) {
		job_free(j);
		return -1;
	}
	memset(pad, 0xa5, QW);
	*first = ROW_INVALID;

	job_reset(j);
	give = cutlen < slen ? cutlen : slen;
	nx_append_dde(j->sddl, (void *)stream, give);
	nx_append_dde(j->tddl, out, outcap);
	nxu_touch_pages(out, outcap, pagesz, 1);
	job_run(j, handle, fc);
	jobs++;

	for (;;) {
		unsigned int sfbt = job_sfbt(j), subc = job_subc(j);
		uint32_t spbc = job_spbc_decomp(j);
		size_t consumed, unproc, hist, histpad, histqw, hstart;
		enum row row;

		if (j->cc != ERR_NX_OK && j->cc != ERR_NX_DATA_LENGTH) {
			describe_failure(j, "decompress");
			goto out;
		}
		tpos += job_tpbc(j);
		if (tpos > explen) {
			printf("  produced %zu bytes, more than the %zu expected\n",
			       tpos, explen);
			goto out;
		}
		row = classify(sfbt, subc);
		if (jobs == 1)
			*first = row;
		if (row == ROW_INVALID) {
			printf("  invalid SFBT/SUBC %x/%u after %zu of %zu source bytes\n",
			       sfbt, subc, spos, slen);
			goto out;
		}
		if (j->cc == ERR_NX_OK) {
			if (sfbt != 0) {
				printf("  cc 0 with sfbt %x\n", sfbt);
				goto out;
			}
			break;
		}
		if (row == ROW_FINAL || row == ROW_TOO_MUCH)
			break;

		/*
		 * Suspended. Section 5.2.5.4: the next source starts SPBC
		 * bytes past this job's source, minus the bytes SUBC says
		 * were not processed (SPBC includes the history bytes, so
		 * those come off first); the resume job gets SUBC's low three
		 * bits as the bit offset into that first byte.
		 */
		consumed = spbc - (jobs > 1 ? getnn(j->cmd->cpb, in_histlen) * QW : 0);
		unproc = (subc + 7) >> 3;
		if (consumed < unproc) {
			printf("  spbc %zu smaller than the %zu unprocessed bytes\n",
			       consumed, unproc);
			goto out;
		}
		spos += consumed - unproc;
		if (spos >= slen) {
			printf("  suspended with no source left (row: %s)\n",
			       row_name[row]);
			goto out;
		}

		/* CPB output qw24-42 become input qw0-18, then HistLen. */
		memcpy(&j->cmd->cpb.qw0, (void *)&j->cmd->cpb.qw24, 19 * QW);

		hist = tpos < HIST_MAX ? tpos : HIST_MAX;
		hstart = tpos - hist;
		histpad = (QW - (hist % QW)) % QW;
		histqw = (hist + histpad) / QW;
		putnn(j->cmd->cpb, in_histlen, histqw);
		putnn(j->cmd->cpb, in_subc, subc & 7);

		clearp_dde(j->sddl);
		clearp_dde(j->tddl);
		if (histpad)
			nx_append_dde(j->sddl, pad, histpad);
		if (hist)
			nx_append_dde(j->sddl, out + hstart, hist);
		nx_append_dde(j->sddl, (void *)(stream + spos), slen - spos);
		nx_append_dde(j->tddl, out + tpos, outcap - tpos);

		rfc = (fc == GZIP_FC_DECOMPRESS_SINGLE_BLK_N_SUSPEND)
		      ? GZIP_FC_DECOMPRESS_RESUME_SINGLE_BLK_N_SUSPEND
		      : GZIP_FC_DECOMPRESS_RESUME;
		memset((void *)&j->cmd->crb.csb, 0, sizeof(j->cmd->crb.csb));
		j->cmd->crb.source_dde = j->sddl[0];
		j->cmd->crb.target_dde = j->tddl[0];
		put64(j->cmd->crb, csb_address,
		      ((uint64_t)&j->cmd->crb.csb) & csb_address_mask);
		j->cmd->crb.gzip_fc = 0;
		putnn(j->cmd->crb, gzip_fc, rfc);
		j->cmd->cpb.out_spbc_decomp = 0;
		j->cc = submit_bounded(j->cmd, handle, job_deadline_ms);
		jobs++;
		if (jobs > 4096) {
			printf("  no progress after %d jobs\n", jobs);
			goto out;
		}
	}

	if (tpos != explen || memcmp(out, expect, explen)) {
		printf("  output differs from the source (%zu of %zu bytes)\n",
		       tpos, explen);
		goto out;
	}
	if (!checksums_ok(j, expect, explen))
		goto out;
	rc = 0;
out:
	*njobs = jobs;
	free(pad);
	job_free(j);
	return rc;
}

/*
 * Cut a stream at every byte length, resume from wherever the engine
 * suspends, and record which row of Table 5-3 the first suspension landed in.
 */
static int sweep_stream(void *handle, const char *what,
			const unsigned char *stream, size_t slen,
			const unsigned char *src, size_t len,
			unsigned char *out, size_t outcap,
			unsigned int *cover, int *ncase)
{
	int nfail = 0, njobs;
	enum row first;
	size_t cut;

	printf("  %s: %zu byte stream\n", what, slen);
	for (cut = 1; cut <= slen; cut++) {
		(*ncase)++;
		if (decompress_resuming(handle, stream, slen, cut,
					GZIP_FC_DECOMPRESS, out, outcap,
					src, len, &first, &njobs)) {
			printf("    FAIL cut at %zu of %zu (first suspension: %s)\n",
			       cut, slen, first < ROW_N ? row_name[first] : "?");
			if (++nfail > 5)
				break;
		}
		if (first < ROW_N)
			cover[first]++;
	}
	return nfail;
}

static int test_positions(void *handle)
{
	/* Ends dynamic, so BFINAL=1 falls in a dynamic block. */
	static const enum blk kinds_a[] = {
		BLK_STORED, BLK_FIXED, BLK_DYNAMIC, BLK_FIXED, BLK_STORED,
		BLK_DYNAMIC
	};
	/* Ends stored, so BFINAL=1 falls in a literal block. */
	static const enum blk kinds_b[] = {
		BLK_DYNAMIC, BLK_FIXED, BLK_STORED
	};
	size_t len = 3000, slen, outcap = 2 * len + 4096;
	unsigned char *src = malloc(len), *stream = malloc(2 * len + 4096);
	unsigned char *out = malloc(outcap);
	unsigned int cover[ROW_N] = {};
	int nfail = 0, ncase = 0, njobs, r, missing = 0;
	enum row first;

	FAIL_IF(!src || !stream || !out);
	fill_text(src, len, 3);
	printf("positions: cutting each stream at every byte length\n");

	slen = zlib_multiblock(src, len, kinds_a,
			       ARRAY_SIZE(kinds_a),
			       stream, 2 * len + 4096);
	FAIL_IF(!slen);
	nfail += sweep_stream(handle, "stored/fixed/dynamic, dynamic last",
			      stream, slen, src, len, out, outcap, cover, &ncase);

	/* Blocks one at a time, section 2.11, on the same stream. */
	ncase++;
	if (decompress_resuming(handle, stream, slen, slen,
				GZIP_FC_DECOMPRESS_SINGLE_BLK_N_SUSPEND, out,
				outcap, src, len, &first, &njobs)) {
		printf("    FAIL single block and suspend\n");
		nfail++;
	} else {
		printf("  single block and suspend: %d jobs for %zu blocks\n",
		       njobs, ARRAY_SIZE(kinds_a));
		if (njobs < (int)(ARRAY_SIZE(kinds_a))) {
			printf("    FAIL single block FC did not stop at each block\n");
			nfail++;
		}
	}

	slen = zlib_multiblock(src, len, kinds_b,
			       ARRAY_SIZE(kinds_b),
			       stream, 2 * len + 4096);
	FAIL_IF(!slen);
	nfail += sweep_stream(handle, "ending in a stored block",
			      stream, slen, src, len, out, outcap, cover, &ncase);

	/*
	 * The same hand-built stream shifted by every number of bits, so the
	 * final block's three bit header starts at each offset within a byte
	 * in turn. Without this the row of Table 5-3 for a suspension one or
	 * two bits into a final block's code is reachable only by luck: it
	 * needs the byte the source is cut at to fall inside those two bits.
	 * A literal of value 255 is nine bits in the fixed code, so k of them
	 * in front move everything after by k bits.
	 */
	for (r = 0; r < 8; r++) {
		char what[64];

		memset(src, 0xff, r);
		fill_text(src + r, 600, 3);
		slen = handbuilt_unaligned(src, 600 + r, 5, stream,
					   2 * len + 4096);
		FAIL_IF(!slen);
		snprintf(what, sizeof(what),
			 "hand-built fixed blocks, shifted %d bit(s)", r);
		nfail += sweep_stream(handle, what, stream, slen, src, 600 + r,
				      out, outcap, cover, &ncase);
	}

	printf("positions: %d cases, %d failed; first suspension by Table 5-3 row:\n",
	       ncase, nfail);
	for (r = 0; r < ROW_N; r++) {
		/*
		 * Two rows are not produced by a decompression that runs out
		 * of source, which is what this sweep does.
		 *
		 * "too much source" is SFBT 0000 with SUBC past the final
		 * block, so it needs source after the stream ends rather than
		 * before; test_errors() induces it and asserts CC 3.
		 *
		 * "in 3 bit block code, BFINAL=1" is SFBT 1111 with SUBC 1-2.
		 * The sweep is exhaustive over the cut lengths that produce
		 * it: the hand-built streams end in a BFINAL=1 block, every
		 * cut length is tried, and the eight shifted variants put
		 * that block's header at all eight bit offsets, so a cut
		 * leaving one or two bits of it unprocessed was submitted
		 * many times over. Every one came back as SFBT 1110, which
		 * is the encoding whose own description says the BFINAL in
		 * it is superfluous because that bit is among the
		 * unprocessed ones. So this implementation reports a partial
		 * block code as 1110 whatever the block's BFINAL, and 1111
		 * with SUBC 1-2 is not reachable from software.
		 */
		int expected = r != ROW_TOO_MUCH && r != ROW_INVALID &&
			       r != ROW_CODE1;

		printf("  %5u  %s%s\n", cover[r], row_name[r],
		       cover[r] == 0 && expected ? "   NOT REACHED" : "");
		if (!cover[r] && expected)
			missing++;
	}
	if (!cover[ROW_CODE1])
		printf("  (SFBT 1111 with SUBC 1-2 not produced: a partial\n"
		       "   block code is reported as 1110 either way)\n");
	if (missing)
		printf("positions: %d architected row(s) never reached\n", missing);
	free(src);
	free(stream);
	free(out);
	FAIL_IF(nfail);
	FAIL_IF(missing);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Section 4: error conditions, Table 5-1 and translation faults            */

static int expect_cc(void *handle, const char *what, unsigned int fc,
		     unsigned char *src, size_t slen, unsigned char *dst,
		     size_t dlen, int want, uint64_t want_fsaddr)
{
	struct job *j = job_new();
	int ok;

	if (!j)
		return 0;
	job_reset(j);
	if (src) {
		nx_append_dde(j->sddl, src, slen);
		if (want != ERR_NX_AT_FAULT || (uint64_t)src != want_fsaddr)
			nxu_touch_pages(src, slen, pagesz, 0);
	}
	if (dst) {
		nx_append_dde(j->tddl, dst, dlen);
		if (want != ERR_NX_AT_FAULT || (uint64_t)dst != want_fsaddr)
			nxu_touch_pages(dst, dlen, pagesz, 1);
	}
	if (want == ERR_NX_AT_FAULT)
		j->no_retry = 1;
	job_run(j, handle, fc);
	ok = j->cc == want;
	if (ok && want == ERR_NX_AT_FAULT) {
		uint64_t fsa = job_fsaddr(j);

		ok = fsa >= (want_fsaddr & ~(uint64_t)(pagesz - 1)) &&
		     fsa < want_fsaddr + pagesz;
		if (!ok)
			printf("  fault reported at %016llx, expected within the page of %016llx\n",
			       (unsigned long long)fsa,
			       (unsigned long long)want_fsaddr);
	}
	printf("  %-52s %-14s %s\n", what, cc_str(j->cc),
	       ok ? "as expected" : "UNEXPECTED");
	if (!ok)
		printf("      wanted %s\n", cc_str(want));
	job_free(j);
	return ok;
}

/*
 * Hand-encoded fixed block: literal 'a', then a match of length 3 at
 * distance 5 with only one byte of history. Section 5-1: CC 67.
 */
static size_t stream_bad_distance(unsigned char *buf, size_t cap)
{
	struct bitw w;
	unsigned int code;
	int n;

	bw_init(&w, buf, cap);
	bw_bits(&w, 1, 1);	/* BFINAL */
	bw_bits(&w, 1, 2);	/* fixed */
	fixed_lit('a', &code, &n); bw_code(&w, code, n);
	fixed_lit(257, &code, &n); bw_code(&w, code, n);	/* length 3 */
	bw_code(&w, 4, 5);					/* dist code 4: 5-6 */
	bw_bits(&w, 0, 1);					/* extra: 5 */
	fixed_lit(256, &code, &n); bw_code(&w, code, n);	/* EOB */
	return bw_bytes(&w);
}

/* Fixed block using the unused code 286: CC 66, undefined Huffman code. */
static size_t stream_undefined_code(unsigned char *buf, size_t cap)
{
	struct bitw w;
	unsigned int code;
	int n;

	bw_init(&w, buf, cap);
	bw_bits(&w, 1, 1);
	bw_bits(&w, 1, 2);
	fixed_lit('a', &code, &n); bw_code(&w, code, n);
	fixed_lit(286, &code, &n); bw_code(&w, code, n);
	bw_bits(&w, 0, 8);
	return bw_bytes(&w);
}

/* Block type 11: CC 66, invalid block header. */
static size_t stream_bad_btype(unsigned char *buf, size_t cap)
{
	struct bitw w;

	bw_init(&w, buf, cap);
	bw_bits(&w, 1, 1);
	bw_bits(&w, 3, 2);
	bw_bits(&w, 0, 16);
	return bw_bytes(&w);
}

/* Dynamic block with HLIT=30 (287 codes): CC 68, invalid DHT. */
static size_t stream_bad_dht(unsigned char *buf, size_t cap)
{
	struct bitw w;
	int i;

	bw_init(&w, buf, cap);
	bw_bits(&w, 1, 1);
	bw_bits(&w, 2, 2);
	bw_bits(&w, 30, 5);	/* HLIT: 287 literal/length codes, invalid */
	bw_bits(&w, 0, 5);
	bw_bits(&w, 0, 4);
	for (i = 0; i < 4; i++)
		bw_bits(&w, 1, 3);
	bw_bits(&w, 0, 64);
	return bw_bytes(&w);
}

/* Stored block with LEN != ~NLEN: CC 66, invalid block header. */
static size_t stream_bad_stored(unsigned char *buf, size_t cap)
{
	struct bitw w;

	bw_init(&w, buf, cap);
	bw_bits(&w, 1, 1);
	bw_bits(&w, 0, 2);
	while (w.pos & 7)
		bw_bits(&w, 0, 1);
	bw_bits(&w, 5, 16);
	bw_bits(&w, 5, 16);
	bw_bits(&w, 0, 40);
	return bw_bytes(&w);
}

static int test_errors(void *handle)
{
	unsigned char *src = malloc(KiB(64)), *dst = malloc(KiB(64));
	unsigned char *good = malloc(KiB(64)), *plain = malloc(KiB(16));
	size_t glen, n;
	int ok = 0, total = 0;
	void *unmapped, *noaccess, *ro;

	FAIL_IF(!src || !dst || !good || !plain);
	fill_text(plain, KiB(16), 9);
	{
		enum blk k = BLK_DYNAMIC;

		glen = zlib_multiblock(plain, KiB(16), &k, 1, good, KiB(64));
		FAIL_IF(!glen);
	}
	printf("errors:\n");

	/* Table 5-1 rows induced with streams. */
	memcpy(src, good, glen);
	total++;
	ok += expect_cc(handle, "insufficient source (stream cut short)",
			   GZIP_FC_DECOMPRESS, src, glen / 2, dst, KiB(64),
			   ERR_NX_DATA_LENGTH, 0);
	memcpy(src, good, glen); memset(src + glen, 0, 64);
	total++;
	ok += expect_cc(handle, "too much source (64 bytes past final block)",
			   GZIP_FC_DECOMPRESS, src, glen + 64, dst, KiB(64),
			   ERR_NX_DATA_LENGTH, 0);
	n = stream_undefined_code(src, KiB(64));
	total++;
	ok += expect_cc(handle, "undefined Huffman code 286",
			   GZIP_FC_DECOMPRESS, src, n, dst, KiB(64),
			   ERR_NX_MISSING_CODE, 0);
	n = stream_bad_btype(src, KiB(64));
	total++;
	ok += expect_cc(handle, "invalid block header (BTYPE 11)",
			   GZIP_FC_DECOMPRESS, src, n, dst, KiB(64),
			   ERR_NX_MISSING_CODE, 0);
	n = stream_bad_stored(src, KiB(64));
	total++;
	ok += expect_cc(handle, "invalid block header (LEN != ~NLEN)",
			   GZIP_FC_DECOMPRESS, src, n, dst, KiB(64),
			   ERR_NX_MISSING_CODE, 0);
	n = stream_bad_distance(src, KiB(64));
	total++;
	ok += expect_cc(handle, "invalid distance (beyond history)",
			   GZIP_FC_DECOMPRESS, src, n, dst, KiB(64),
			   ERR_NX_INVALID_DIST, 0);
	n = stream_bad_dht(src, KiB(64));
	total++;
	ok += expect_cc(handle, "invalid DHT (HLIT 30)",
			   GZIP_FC_DECOMPRESS, src, n, dst, KiB(64),
			   ERR_NX_INVALID_DHT, 0);
	memcpy(src, good, glen);
	total++;
	ok += expect_cc(handle, "target too small for the output",
			   GZIP_FC_DECOMPRESS, src, glen, dst, 100,
			   ERR_NX_TARGET_SPACE, 0);
	fill_random(src, 4096, 1);
	total++;
	ok += expect_cc(handle, "incompressible input, roomy target",
			   GZIP_FC_COMPRESS_FHT, src, 4096, dst, KiB(64),
			   ERR_NX_TPBC_GT_SPBC, 0);
	total++;
	ok += expect_cc(handle, "incompressible input, target = source",
			   GZIP_FC_COMPRESS_FHT, src, 4096, dst, 4096,
			   ERR_NX_TARGET_SPACE, 0);

	/* Translation faults: each must be CC 250 at the offending address. */
	unmapped = mmap(NULL, pagesz, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	FAIL_IF(unmapped == MAP_FAILED);
	munmap(unmapped, pagesz);
	total++;
	ok += expect_cc(handle, "unmapped source", GZIP_FC_WRAP,
			   unmapped, 256, dst, KiB(64), ERR_NX_AT_FAULT,
			   (uint64_t)unmapped);
	total++;
	ok += expect_cc(handle, "unmapped target", GZIP_FC_WRAP,
			   plain, 256, unmapped, 256, ERR_NX_AT_FAULT,
			   (uint64_t)unmapped);
	noaccess = mmap(NULL, pagesz, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
			-1, 0);
	FAIL_IF(noaccess == MAP_FAILED);
	total++;
	ok += expect_cc(handle, "PROT_NONE source", GZIP_FC_WRAP,
			   noaccess, 256, dst, KiB(64), ERR_NX_AT_FAULT,
			   (uint64_t)noaccess);
	ro = mmap(NULL, pagesz, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	FAIL_IF(ro == MAP_FAILED);
	total++;
	ok += expect_cc(handle, "read-only target", GZIP_FC_WRAP,
			   plain, 256, ro, 256, ERR_NX_AT_FAULT,
			   (uint64_t)ro);
	total++;
	ok += expect_cc(handle, "kernel address as source", GZIP_FC_WRAP,
			   (void *)0xc000000000000000ul, 256, dst, KiB(64),
			   ERR_NX_AT_FAULT, 0xc000000000000000ul);
	total++;
	ok += expect_cc(handle, "address above the user limit",
			   GZIP_FC_WRAP, (void *)0x7ffffffffffff000ul, 256,
			   dst, KiB(64), ERR_NX_AT_FAULT,
			   0x7ffffffffffff000ul);
	munmap(noaccess, pagesz);
	munmap(ro, pagesz);

	/* Malformed requests: a code, not a hang and not silence. */
	{
		struct job *j = job_new();

		FAIL_IF(!j);
		job_reset(j);
		nx_append_dde(j->sddl, plain, 256);
		nx_append_dde(j->tddl, dst, KiB(64));
		job_run(j, handle, 0x1a);	/* reserved function code */
		printf("  %-52s %-14s %s\n", "reserved function code 0x1a",
		       cc_str(j->cc), j->cc != ERR_NX_OK ? "refused" : "ACCEPTED");
		total++; ok += j->cc != ERR_NX_OK;

		job_reset(j);
		nx_append_dde(j->sddl, plain, 256);
		nx_append_dde(j->tddl, dst, KiB(64));
		putp32(j->sddl, ddebc, 0);
		job_run(j, handle, GZIP_FC_WRAP);
		printf("  %-52s %-14s tpbc %u\n", "zero length source",
		       cc_str(j->cc), job_tpbc(j));
		total++; ok += job_tpbc(j) == 0;

		job_reset(j);
		nx_append_dde(j->sddl, plain, 100);
		nx_append_dde(j->sddl, plain + 100, 100);
		putp32(j->sddl, ddebc, 300);	/* list sums to 200 */
		nx_append_dde(j->tddl, dst, KiB(64));
		job_run(j, handle, GZIP_FC_WRAP);
		printf("  %-52s %-14s %s\n",
		       "indirect list shorter than its byte count", cc_str(j->cc),
		       j->cc != ERR_NX_OK ? "refused" : "ACCEPTED");
		total++; ok += j->cc != ERR_NX_OK;
		job_free(j);
	}

	printf("errors: %d of %d as expected\n", ok, total);
	free(src);
	free(dst);
	free(good);
	free(plain);
	FAIL_IF(ok != total);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Section 5: isolation                                                     */

static int wrap_ok_norty(void *handle, unsigned char *src, unsigned char *dst,
			 size_t len, int *cc, int no_retry)
{
	struct job *j = job_new();
	int rc;

	if (!j)
		return 0;
	job_reset(j);
	j->no_retry = no_retry;
	nx_append_dde(j->sddl, src, len);
	nx_append_dde(j->tddl, dst, len);
	job_run(j, handle, GZIP_FC_WRAP);
	*cc = j->cc;
	rc = j->cc == ERR_NX_OK && !memcmp(src, dst, len) &&
	     checksums_ok(j, src, len);
	job_free(j);
	return rc;
}

static int wrap_ok(void *handle, unsigned char *src, unsigned char *dst,
		   size_t len, int *cc)
{
	return wrap_ok_norty(handle, src, dst, len, cc, 0);
}

/*
 * Another process's buffer, submitted through this window. The child maps a
 * page at an address this process has nothing at, fills it, and reports the
 * address; this process must get a fault at that address and its target must
 * be untouched.
 */
static int iso_other_process(void *handle)
{
	int pipefd[2], status, cc, ok;
	unsigned long addr = 0;
	unsigned char *dst = malloc(pagesz);
	pid_t pid;

	FAIL_IF(!dst || pipe(pipefd));
	memset(dst, 0x11, pagesz);
	pid = fork();
	FAIL_IF(pid < 0);
	if (pid == 0) {
		void *p = mmap((void *)0x40000000ul, pagesz, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			       -1, 0);
		unsigned long a = p == MAP_FAILED ? 0 : (unsigned long)p;

		if (a)
			memset(p, 0x77, pagesz);
		close(pipefd[0]);
		if (write(pipefd[1], &a, sizeof(a)) != sizeof(a))
			_exit(1);
		close(pipefd[1]);
		pause();
		_exit(0);
	}
	close(pipefd[1]);
	FAIL_IF(read(pipefd[0], &addr, sizeof(addr)) != sizeof(addr));
	close(pipefd[0]);
	FAIL_IF(!addr);

	ok = !wrap_ok_norty(handle, (unsigned char *)addr, dst, 256, &cc, 1) &&
	     cc == ERR_NX_AT_FAULT && dst[0] == 0x11;
	printf("  %-52s %-14s %s\n", "another process's address as source",
	       cc_str(cc), ok ? "faulted, target untouched" : "NOT ISOLATED");
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	free(dst);
	return ok;
}

/*
 * A child of a process with an open window inherits the paste mapping. The
 * child writes its own data into a buffer it shares copy-on-write with the
 * parent and submits a wrap through the inherited window. Whatever the engine
 * does, the parent's copy of both buffers must be exactly what the parent
 * left in them.
 */
static int iso_fork_after_open(void *handle)
{
	size_t len = 4096;
	unsigned char *src = malloc(len), *dst = malloc(len);
	int status, ok;
	pid_t pid;

	FAIL_IF(!src || !dst);
	memset(src, 0x21, len);
	memset(dst, 0x22, len);
	pid = fork();
	FAIL_IF(pid < 0);
	if (pid == 0) {
		int cc, r;

		memset(src, 0x33, len);		/* child's private copy */
		r = wrap_ok(handle, src, dst, len, &cc);
		printf("  child through inherited window: cc %d, %s\n", cc,
		       r ? "copied its own data" : cc == ERR_NX_OK
		       ? "completed but did not copy the child's data"
		       : "did not complete");
		fflush(stdout);
		_exit(cc == ERR_NX_OK ? (r ? 0 : 2) : 1);
	}
	waitpid(pid, &status, 0);
	ok = src[0] == 0x21 && dst[0] == 0x22 && src[len - 1] == 0x21 &&
	     dst[len - 1] == 0x22;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		ok = 0;		/* the child copied data through the window */
	printf("  %-52s %s; child %s\n", "fork after window open",
	       ok ? "parent's memory untouched" : "NOT ISOLATED",
	       WIFSIGNALED(status) ? (WTERMSIG(status) == SIGSEGV
				      ? "died with SIGSEGV at the paste (mapping not inherited)"
				      : "died by another signal")
	       : WEXITSTATUS(status) == 0 ? "COPIED DATA" : "could not complete a request");
	free(src);
	free(dst);
	return ok;
}

/*
 * exec replaces the address space under an open window, and the descriptor
 * survives it. A mapping made from the new image would bind a window whose
 * translations belong to an address space that no longer exists.
 *
 * The library closes the window descriptor as soon as it has mapped the paste
 * address, so nothing is inherited through it; this opens the device directly
 * and keeps the descriptor, which is what a caller that wanted to remap after
 * a credit loss would do. The child image is this binary in exec-child mode.
 */
static int open_window_keep_fd(void)
{
	struct vas_tx_win_open_attr attr = { .version = 1, .vas_id = 0 };
	int fd = open("/dev/crypto/nx-gzip", O_RDWR);

	if (fd < 0)
		return -1;
	if (ioctl(fd, VAS_TX_WIN_OPEN, (unsigned long)&attr) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int iso_exec_after_open(const char *self)
{
	char fdstr[16];
	int status, fd = open_window_keep_fd();
	pid_t pid;

	if (fd < 0) {
		printf("  %-52s cannot open a window directly (%s)\n",
		       "exec after window open", strerror(errno));
		return 0;
	}
	snprintf(fdstr, sizeof(fdstr), "%d", fd);
	pid = fork();
	FAIL_IF(pid < 0);
	if (pid == 0) {
		execl(self, self, "exec-child", fdstr, (char *)NULL);
		_exit(127);
	}
	waitpid(pid, &status, 0);
	close(fd);
	printf("  %-52s child exit %d (%s)\n", "exec after window open",
	       WEXITSTATUS(status),
	       WEXITSTATUS(status) == 0 ? "nothing translated" : "SEE ABOVE");
	return WEXITSTATUS(status) == 0;
}

static int exec_child_main(int fd)
{
	/*
	 * The window descriptor was inherited across the exec. Its paste
	 * mapping did not survive with the old address space, so map it
	 * again. The kernel must refuse: this mm is not the one the window
	 * translates for. If it did not refuse, a paste here would be
	 * translated through an address space that has been torn down.
	 */
	int cc = -1, ok = 0;
	struct nx_handle h;
	unsigned char *src = malloc(4096), *dst = malloc(4096);
	void *addr;

	pagesz = sysconf(_SC_PAGESIZE);
	addr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		printf("  exec child: mapping the inherited window refused (%s)\n",
		       strerror(errno));
		return 0;
	}
	printf("  exec child: mapped the inherited window from a new address space\n");
	h.fd = fd;
	h.function = NX_FUNC_COMP_GZIP;
	h.paste_addr = (char *)addr + 0x400;
	memset(src, 0x44, 4096);
	memset(dst, 0x55, 4096);
	job_deadline_ms = 3000;
	ok = wrap_ok(&h, src, dst, 4096, &cc);
	printf("  exec child: paste through inherited window: cc %d, %s\n", cc,
	       ok ? "COPIED DATA ACROSS THE EXEC" : cc == CC_TIMEOUT
	       ? "never completed" : "no data moved");
	return ok ? 2 : 0;
}

/* Windows in a loop, several open at once, closed in odd orders: one PID. */
static int iso_window_churn(void)
{
	void *h[8];
	unsigned char *src = malloc(4096), *dst = malloc(4096);
	int i, k, cc, ok = 1;

	FAIL_IF(!src || !dst);
	fill_text(src, 4096, 13);
	for (k = 0; k < 20 && ok; k++) {
		for (i = 0; i < 8; i++) {
			h[i] = nx_function_begin(NX_FUNC_COMP_GZIP, 0);
			if (!h[i]) {
				printf("  window %d of round %d: open failed\n", i, k);
				ok = 0;
				break;
			}
		}
		if (!ok)
			break;
		/* Close half, use the rest, then close the rest. */
		for (i = 0; i < 8; i += 2)
			nx_function_end(h[i]);
		for (i = 1; i < 8; i += 2) {
			memset(dst, 0, 4096);
			if (!wrap_ok(h[i], src, dst, 4096, &cc)) {
				printf("  window %d of round %d after closing others: cc %d\n",
				       i, k, cc);
				ok = 0;
			}
		}
		for (i = 1; i < 8; i += 2)
			nx_function_end(h[i]);
	}
	printf("  %-52s %s\n", "8 windows x 20 rounds, closed in halves",
	       ok ? "all requests correct" : "FAILED");
	free(src);
	free(dst);
	return ok;
}

static const char *self_path;

static int test_isolation(void *handle)
{
	int ok = 1;

	printf("isolation:\n");
	ok &= iso_other_process(handle);
	ok &= iso_fork_after_open(handle);
	ok &= iso_exec_after_open(self_path);
	ok &= iso_window_churn();
	FAIL_IF(!ok);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Harness                                                                  */

static void *g_handle;

static int run_all(void)
{
	int rc = 0;

	/*
	 * Opened here and not in main(): test_harness() runs this in a forked
	 * child, and a window opened by the parent would make every request
	 * below a request through an inherited window, which is one of the
	 * isolation cases and not the baseline.
	 */
	g_handle = nx_function_begin(NX_FUNC_COMP_GZIP, 0);
	if (!g_handle) {
		printf("cannot open a window: %s\n", strerror(errno));
		return 1;
	}
	rc |= test_functions(g_handle);
	rc |= test_wrap(g_handle);
	rc |= test_resume_compress(g_handle);
	rc |= test_positions(g_handle);
	rc |= test_errors(g_handle);
	rc |= test_isolation(g_handle);
	nx_function_end(g_handle);
	return rc;
}

/* Worker for the multi-process driver: open, job, verify, close, repeat. */
static int worker_main(int iters)
{
	unsigned char *src = malloc(MiB(2)), *dst = malloc(MiB(4) + 4096);
	unsigned char *ref = malloc(MiB(2));
	unsigned int seed = getpid();
	int i, fails = 0;

	if (!src || !dst || !ref)
		return 2;
	for (i = 0; i < iters; i++) {
		void *h = nx_function_begin(NX_FUNC_COMP_GZIP, 0);
		size_t len;
		int rc;

		if (!h) {
			printf("worker %d: open failed at %d: %s\n", getpid(), i,
			       strerror(errno));
			return 3;
		}
		seed = seed * 1103515245u + 12345u;
		len = 1 + (seed >> 8) % MiB(2);
		if (i & 1)
			fill_text(src, len, seed);
		else
			fill_lfsr(src, len, 0x6000u, seed);
		/*
		 * A short input compresses to more than itself, which the
		 * engine reports as ERR_NX_TPBC_GT_SPBC with a valid target;
		 * the sizes here are random, so say so.
		 */
		rc = round_trip_fht(h, src, len, dst, 2 * len + 1024, 1 + (i % 5),
				    GZIP_FC_COMPRESS_FHT, NULL, ref, len < 256);
		nx_function_end(h);
		if (rc) {
			printf("worker %d: iteration %d (%zu bytes) FAILED\n",
			       getpid(), i, len);
			fails++;
		}
	}
	printf("worker %d: %d iterations, %d failed\n", getpid(), iters, fails);
	return fails ? 1 : 0;
}

static int inflight_main(void)
{
	void *h = nx_function_begin(NX_FUNC_COMP_GZIP, 0);
	size_t len = MiB(16);
	unsigned char *src = malloc(len), *dst = malloc(2 * len + 1024);
	unsigned char *ref = malloc(len);
	int rc, i;

	if (!h || !src || !dst || !ref)
		return 2;
	fill_text(src, len, 17);
	for (i = 0; i < 1000000; i++) {
		rc = round_trip_fht(h, src, len, dst, 2 * len + 1024, 1,
				    GZIP_FC_COMPRESS_FHT, NULL, ref, 0);
		if (rc)
			return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	pagesz = sysconf(_SC_PAGESIZE);
	self_path = argv[0];
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (argc > 1 && !strcmp(argv[1], "worker"))
		return worker_main(argc > 2 ? atoi(argv[2]) : 20);
	if (argc > 1 && !strcmp(argv[1], "inflight"))
		return inflight_main();
	if (argc > 1 && !strcmp(argv[1], "exec-child"))
		return exec_child_main(argc > 2 ? atoi(argv[2]) : -1);

	test_harness_set_timeout(2400);
	return test_harness(run_all, "nx_gzip_stress");
}
