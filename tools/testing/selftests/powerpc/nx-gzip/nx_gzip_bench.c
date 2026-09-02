// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Throughput of the NX-GZIP accelerator, across request sizes and across
 * scatter/gather shapes.
 *
 * Two things are measured that the functional tests do not reach.
 *
 * Size, because the cost of a request is not proportional to it. A window
 * open, a paste and a completion poll are fixed costs, so small requests
 * report the overhead and large ones report the engine.
 *
 * Descriptor shape, because a request may name its buffers with one direct
 * descriptor or with a list of them, and the second is what any real caller
 * with a scattered buffer produces. The two take different paths through the
 * engine and, on a hash MMU, through the fault handler behind it.
 *
 * What it asserts, and why it is not a throughput floor. A floor fails on a
 * slower part and passes on a fast one whose accelerator is not being used,
 * so it is the wrong assertion in both directions. What is checked instead is
 * that the engine's own Adler-32 and CRC-32 outputs agree with zlib computed
 * over the same input. That is an independent reference rather than the
 * hardware agreeing with itself, and it cannot be satisfied unless the engine
 * read exactly the bytes it was given -- which is the property a scatter list
 * puts at risk. There is no software path behind this interface, so a request
 * that completes with correct checksums is proof the accelerator ran.
 *
 * Throughput is reported for tracking rather than asserted, which is how the
 * other benchmarks in this tree behave.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <endian.h>
#include <zlib.h>

#include "utils.h"
#include "nxu.h"
#include "nx.h"

#define BENCH_MAX	(16 << 20)
#define BENCH_FRAGS	16	/* extents in the scattered case */

/* The gzip_vas.c helpers log through these, as in the sibling tests. */
int nx_dbg;
FILE *nx_gzip_log;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/*
 * Fill from a maximal-length Galois LFSR, one byte per step.
 *
 * The generator matters more than it looks. Random data does not compress, so
 * the engine returns ERR_NX_TPBC_GT_SPBC and the test has to accept two
 * completion codes instead of asserting one. A constant compresses to nothing
 * and measures neither the engine nor the memory system. An LFSR sits between
 * them and, being periodic with a period that is known exactly, decides which
 * of the two happens: a buffer longer than the period compresses, because
 * everything past the first period is a back reference, and one shorter than
 * it does not.
 *
 * So the period is chosen per use. Degree 8 gives period 255, so even the
 * smallest buffer here holds sixteen repetitions and always compresses --
 * that is the control, where the completion code is asserted exactly. Degree
 * 15 gives period 32767, comparable to the deflate window, which is the more
 * representative shape for a throughput figure.
 *
 * Both are standard maximal-length polynomials and the sequences are
 * deterministic, so a checksum mismatch reproduces rather than having to be
 * caught again.
 */
#define LFSR_TAPS_D8	0x8eu		/* x^8 + x^6 + x^5 + x^4 + 1  */
#define LFSR_TAPS_D15	0x6000u		/* x^15 + x^14 + 1            */

static void fill_lfsr(char *p, size_t len, unsigned int taps)
{
	unsigned int state = 1;
	size_t i;

	for (i = 0; i < len; i++) {
		unsigned int lsb = state & 1u;

		state >>= 1;
		if (lsb)
			state ^= taps;
		p[i] = (char)(state & 0xffu);
	}
}

/*
 * A de Bruijn sequence B(256, 2): 65536 bytes in which every ordered pair of
 * byte values occurs exactly once, cyclically.
 *
 * It is the opposite corner from the LFSR. The LFSR repeats, so the match
 * finder always has something to find; this has no repeated pair anywhere, so
 * at order two it has nothing, and any match it does report has to come from
 * a longer coincidence. That makes it the adversarial input for the part of
 * the engine a throughput figure otherwise flatters, and it is a fixed vector
 * -- the same 65536 bytes on every run and every machine -- so a checksum
 * mismatch here is reproducible rather than a one-off.
 *
 * Built by the standard Lyndon word construction. Recursion depth is n, which
 * is two.
 */
static void db_visit(unsigned char *out, size_t *len, unsigned char *a,
		     int t, int p, int k, int n)
{
	int j;

	if (t > n) {
		if (n % p == 0)
			for (j = 1; j <= p; j++)
				out[(*len)++] = a[j];
		return;
	}

	a[t] = a[t - p];
	db_visit(out, len, a, t + 1, p, k, n);
	for (j = a[t - p] + 1; j < k; j++) {
		a[t] = (unsigned char)j;
		db_visit(out, len, a, t + 1, t, k, n);
	}
}

static size_t fill_de_bruijn(unsigned char *out)
{
	unsigned char a[2 * 256] = { 0 };
	size_t len = 0;

	db_visit(out, &len, a, 1, 1, 256, 2);
	return len;
}

static int submit(struct nx_gzip_crb_cpb_t *cmdp, void *handle,
		  char *src, uint32_t srclen, char *dst, uint32_t dstlen,
		  struct nx_dde_t *sgl, int frags)
{
	int tries = NX_MAX_FAULTS;
	int cc;

	for (;;) {
		put32(cmdp->crb, gzip_fc, 0);
		putnn(cmdp->crb, gzip_fc, GZIP_FC_COMPRESS_RESUME_FHT);
		putnn(cmdp->cpb, in_histlen, 0);
		/*
		 * RESUME_FHT continues a running checksum rather than
		 * starting one, so both seeds have to be set for every
		 * attempt. Adler-32 begins at 1 (RFC 1950), CRC-32 at 0.
		 */
		put32(cmdp->cpb, in_adler, 1);
		put32(cmdp->cpb, in_crc, 0);
		memset((void *) &cmdp->crb.csb, 0, sizeof(cmdp->crb.csb));
		put32(cmdp->cpb, out_spbc_comp, 0);

		put64(cmdp->crb, csb_address, 0);
		put64(cmdp->crb, csb_address,
		      (uint64_t)&cmdp->crb.csb & csb_address_mask);

		if (frags <= 1) {
			clear_dde(cmdp->crb.source_dde);
			putnn(cmdp->crb.source_dde, dde_count, 0);
			put32(cmdp->crb.source_dde, ddebc, srclen);
			put64(cmdp->crb.source_dde, ddead, (uint64_t)src);
		} else {
			/*
			 * One indirect descriptor naming a list. The engine
			 * walks the list rather than one extent, which is the
			 * path a scattered buffer takes.
			 */
			uint32_t each = srclen / frags;
			int i;

			clearp_dde(sgl);
			for (i = 0; i < frags; i++)
				nx_append_dde(sgl, src + (size_t)i * each,
					      i == frags - 1
						? srclen - each * (frags - 1)
						: each);

			clear_dde(cmdp->crb.source_dde);
			putnn(cmdp->crb.source_dde, dde_count, frags);
			put32(cmdp->crb.source_dde, ddebc, srclen);
			put64(cmdp->crb.source_dde, ddead, (uint64_t)&sgl[1]);
		}

		clear_dde(cmdp->crb.target_dde);
		putnn(cmdp->crb.target_dde, dde_count, 0);
		put32(cmdp->crb.target_dde, ddebc, dstlen);
		put64(cmdp->crb.target_dde, ddead, (uint64_t)dst);

		cc = nxu_submit_job(cmdp, handle);
		if (cc != ERR_NX_AT_FAULT)
			return cc;

		/*
		 * The engine terminates a request that faults, so the retry
		 * is the caller's. See Documentation/arch/powerpc/vas-api.rst.
		 */
		if (--tries <= 0)
			return cc;
		nxu_touch_pages((void *)cmdp->crb.csb.fsaddr, 1,
				sysconf(_SC_PAGESIZE), 1);
	}
}

/*
 * @exact: assert the completion code is ERR_NX_OK rather than allowing the
 * engine to report that the output grew. Only meaningful when the source is
 * known to compress, which is what the control run arranges.
 */
static int run(uint32_t len, int frags, int exact,
	       struct nx_gzip_crb_cpb_t *cmdp,
	       void *handle, char *src, char *dst, struct nx_dde_t *sgl)
{
	uint32_t want_adler, want_crc, got_adler, got_crc;
	double t0, t1, secs, zsecs;
	unsigned long zlen;
	char *zbuf;
	int cc;

	nxu_touch_pages(cmdp, sizeof(*cmdp), sysconf(_SC_PAGESIZE), 1);
	nxu_touch_pages(src, len, sysconf(_SC_PAGESIZE), 0);
	nxu_touch_pages(dst, 2 * (size_t)len, sysconf(_SC_PAGESIZE), 1);

	t0 = now();
	cc = submit(cmdp, handle, src, len, dst, 2 * len, sgl, frags);
	t1 = now();

	/*
	 * Larger output than input is not a failure, only incompressible --
	 * except where the source was built to compress, and then it is.
	 */
	if (cc != ERR_NX_OK && (exact || cc != ERR_NX_TPBC_GT_SPBC)) {
		printf("  cc %d: %u bytes, %s, %s\n", cc, len,
		       frags > 1 ? "scattered" : "direct",
		       exact ? "control" : "measure");
		printf("    spbc %u of %u bytes consumed, tpbc %u produced\n",
		       get32(cmdp->cpb, out_spbc_comp), len,
		       get32(cmdp->crb.csb, tpbc));
		printf("    csb  cc %u ce %02x fsaddr %016llx\n",
		       getnn(cmdp->crb.csb, csb_cc), getnn(cmdp->crb.csb, csb_ce),
		       (unsigned long long) get64(cmdp->crb.csb, fsaddr));
		printf("    crb  %p  src %p..%p  dst %p..%p  sgl %p\n",
		       cmdp, src, src + len, dst, dst + 2 * (size_t)len, sgl);
		printf("    csb  %p  (csb_address field %016llx)\n",
		       (void *)&cmdp->crb.csb,
		       (unsigned long long) get64(cmdp->crb, csb_address));
	}
	if (exact)
		FAIL_IF(cc != ERR_NX_OK);
	else
		FAIL_IF(cc != ERR_NX_OK && cc != ERR_NX_TPBC_GT_SPBC);

	got_adler = get_cpb_adler32(cmdp->cpb);
	got_crc   = get_cpb_crc32(cmdp->cpb);
	want_adler = adler32(adler32(0, NULL, 0), (Bytef *)src, len);
	want_crc   = crc32(crc32(0, NULL, 0), (Bytef *)src, len);

	/*
	 * The independent check. These come from zlib, not from the engine,
	 * so they cannot agree unless the engine read the bytes it was given
	 * -- in order, and all of them.
	 */
	if (got_adler != want_adler || got_crc != want_crc) {
		printf("  checksum mismatch: %u bytes, %s, %s\n", len,
		       frags > 1 ? "scattered" : "direct",
		       exact ? "control" : "measure");
		printf("    adler engine %08x zlib %08x %s\n", got_adler,
		       want_adler, got_adler == want_adler ? "ok" : "MISMATCH");
		printf("    crc   engine %08x zlib %08x %s\n", got_crc,
		       want_crc, got_crc == want_crc ? "ok" : "MISMATCH");
		printf("    spbc %u of %u bytes consumed, tpbc %u produced\n",
		       get32(cmdp->cpb, out_spbc_comp), len,
		       get32(cmdp->crb.csb, tpbc));
	}
	FAIL_IF(got_adler != want_adler);
	FAIL_IF(got_crc != want_crc);

	secs = t1 - t0;

	zlen = compressBound(len);
	zbuf = malloc(zlen);
	FAIL_IF(!zbuf);
	t0 = now();
	compress2((Bytef *)zbuf, &zlen, (Bytef *)src, len, 1);
	t1 = now();
	zsecs = t1 - t0;
	free(zbuf);

	printf("  %8u bytes  %-9s  %-7s  nx %8.2f MB/s  zlib-1 %7.2f MB/s  %5.1fx\n",
	       len, frags > 1 ? "scattered" : "direct",
	       exact ? "control" : "measure",
	       len / secs / 1e6, len / zsecs / 1e6, zsecs / secs);

	return 0;
}

static int test_nx_gzip_bench(void)
{
	static const uint32_t sizes[] = { 4096, 65536, 1 << 20, BENCH_MAX };
	struct nx_gzip_crb_cpb_t *cmdp;
	struct nx_dde_t *sgl;
	char *src, *dst;
	void *handle;
	size_t i;

	handle = nx_function_begin(NX_FUNC_COMP_GZIP, 0);
	SKIP_IF_MSG(!handle, "no NX-GZIP engine available");

	FAIL_IF(posix_memalign((void **)&cmdp, 4096, sizeof(*cmdp)));
	FAIL_IF(posix_memalign((void **)&src, 4096, BENCH_MAX));
	FAIL_IF(posix_memalign((void **)&dst, 4096, 2 * (size_t)BENCH_MAX));
	FAIL_IF(posix_memalign((void **)&sgl, 4096,
			       (BENCH_FRAGS + 1) * sizeof(*sgl)));

	memset(cmdp, 0, sizeof(*cmdp));

	/*
	 * Control first, on a source short enough in period that every size
	 * compresses. A failure here is unambiguous: the engine either did
	 * not read what it was given, or did not complete cleanly on input it
	 * certainly could.
	 */
	fill_lfsr(src, BENCH_MAX, LFSR_TAPS_D8);
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		FAIL_IF(run(sizes[i], 1, 1, cmdp, handle, src, dst, sgl));
		FAIL_IF(run(sizes[i], BENCH_FRAGS, 1, cmdp, handle, src, dst,
			    sgl));
	}

	/* Then the figure worth quoting, on a period nearer the window. */
	fill_lfsr(src, BENCH_MAX, LFSR_TAPS_D15);
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		FAIL_IF(run(sizes[i], 1, 0, cmdp, handle, src, dst, sgl));
		FAIL_IF(run(sizes[i], BENCH_FRAGS, 0, cmdp, handle, src, dst,
			    sgl));
	}

	/*
	 * Finally the case with nothing to match on. Its length is not chosen:
	 * B(256, 2) is 65536 bytes and could not be any other size.
	 */
	{
		size_t dblen = fill_de_bruijn((unsigned char *)src);

		FAIL_IF(dblen != 65536);
		FAIL_IF(run((uint32_t)dblen, 1, 0, cmdp, handle, src, dst, sgl));
		FAIL_IF(run((uint32_t)dblen, BENCH_FRAGS, 0, cmdp, handle, src,
			    dst, sgl));
	}

	free(sgl);
	free(dst);
	free(src);
	free(cmdp);
	nx_function_end(handle);
	return 0;
}

int main(void)
{
	nx_dbg = 0;
	nx_gzip_log = NULL;

	test_harness_set_timeout(300);
	return test_harness(test_nx_gzip_bench, "nx_gzip_bench");
}
