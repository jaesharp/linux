# SPDX-License-Identifier: GPL-2.0-or-later
"""Separate what a wake costs into the part a faster core would fix and the part it would not.

A latency measured at one clock cannot tell those apart. Measured at several,
it can: only the part done in the sending core shrinks when the core speeds up,
so a run of clocks gives

    t = fixed + cycles / GHz

and the two coefficients are the answer. The earlier version of this was a
least-squares line per placement, reported as two numbers with no uncertainty
attached, and the numbers were then hedged in prose because some of the fits
were visibly worse than others. Prose is not an error bar.

Why this model. The response is linear in 1/GHz with an unknown error scale, so
with the reference prior p(fixed, cycles, sigma^2) proportional to 1/sigma^2 the
joint posterior for the coefficients is a multivariate Student-t in closed form.
That matters for the same reason it did in analyse.py: the draws are exact,
there is no chain to converge, no burn-in to discard, and no diagnostic that can
quietly fail and leave a plausible answer behind.

The prior is the reference one and not a "weakly informative" choice, because a
weakly informative prior here would need a scale, and the scales differ by an
order of magnitude between placements -- a prior centred for the cross-chip
figure would be badly informative for the on-chip one. The reference prior has
no centre and no scale to get wrong.

What is reported per placement is a credible interval on each coefficient, and
the share of the total the fixed part accounts for at the fastest clock
measured. What is also reported, and matters more, is whether the model fits:
the posterior predictive spread against the observed spread. A tight interval
from a model that does not describe the data is a confident wrong answer, and
this hardware has already produced bimodal run distributions that no normal
likelihood can represent.

    uv run --with numpy decompose.py records.ndjson
    uv run --with numpy decompose.py --self-check
"""

import json
import sys

import numpy as np

DRAWS = 200000


def posterior(x, y, rng, draws=DRAWS):
    """Exact draws from p(coefficients | data) under the reference prior.

    The design matrix is [1, x]; with p(b, sigma^2) proportional to 1/sigma^2 the
    marginal posterior of b is multivariate-t with n - 2 degrees of freedom.
    Drawing sigma^2 from its inverse-gamma marginal and then b from the
    conditional normal reproduces that exactly, without forming the t directly.
    """
    n = len(x)
    if n < 3:
        return None

    design = np.column_stack([np.ones(n), x])
    gram = design.T @ design
    inverse = np.linalg.inv(gram)
    fit = inverse @ design.T @ y
    residual = y - design @ fit
    scale = float(residual @ residual)
    dof = n - 2

    # sigma^2 | data ~ inverse-gamma(dof/2, scale/2)
    sigma2 = scale / 2.0 / rng.gamma(dof / 2.0, 1.0, size=draws)
    # b | sigma^2, data ~ Normal(fit, sigma^2 * inverse)
    root = np.linalg.cholesky(inverse)
    noise = rng.standard_normal((draws, 2)) @ root.T
    return fit + noise * np.sqrt(sigma2)[:, None], np.sqrt(sigma2)


def interval(samples, mass=0.95):
    lo = float(np.quantile(samples, (1.0 - mass) / 2.0))
    hi = float(np.quantile(samples, 1.0 - (1.0 - mass) / 2.0))
    return lo, hi


def predictive_check(x, y, coefficients, sigma, rng):
    """Does the model produce data that looks like what was measured?

    One draw of the coefficients and the error scale per replicated dataset, so
    the replicates carry the parameter uncertainty as well as the noise. The
    statistic is the spread, because the failure this hardware actually shows is
    a wider spread than a single normal can make -- a run that lands in the
    other mode is not a tail, it is a second peak.
    """
    take = min(2000, len(sigma))
    picked = rng.choice(len(sigma), size=take, replace=False)
    replicated = (coefficients[picked, 0][:, None]
                  + coefficients[picked, 1][:, None] * x[None, :]
                  + rng.standard_normal((take, len(x))) * sigma[picked][:, None])
    observed = float(np.max(y) - np.min(y))
    spreads = np.max(replicated, axis=1) - np.min(replicated, axis=1)
    # How extreme the observed spread is among what the model would produce.
    return observed, float(np.mean(spreads >= observed))


def read(path):
    """Runs by placement, with the clock each was taken at."""
    points = {}
    environments = []
    with open(path) as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            if record.get("kind") == "environment":
                environments.append(record)
                continue
            if "ghz" not in record or "median_ns" not in record:
                continue
            if not record.get("paste", True):
                continue
            points.setdefault(record["label"], []).append(
                (record["ghz"], record["median_ns"]))
    return points, environments


def describe(name, samples, sigma, x, y, rng):
    fixed, cycles = samples[:, 0], samples[:, 1]
    flo, fhi = interval(fixed)
    clo, chi = interval(cycles)
    fastest = float(np.max(1.0 / x))
    total = fixed + cycles * (1.0 / fastest)
    share = 100.0 * fixed / total
    slo, shi = interval(share)

    observed, tail = predictive_check(x, y, samples, sigma, rng)

    print(f"  {name}")
    print(f"    fixed        {np.mean(fixed):8.1f} ns    95% [{flo:7.1f}, {fhi:7.1f}]")
    print(f"    core         {np.mean(cycles):8.0f} cycles 95% [{clo:7.0f}, {chi:7.0f}]")
    print(f"    fixed share  {np.mean(share):8.1f} %     95% [{slo:7.1f}, {shi:7.1f}]"
          f"  at {fastest:.3f} GHz")
    if flo < 0.0:
        print("    the fixed part is not resolved from zero: this placement's cost")
        print("    is consistent with being entirely core work")
    if tail < 0.02 or tail > 0.98:
        print(f"    the model does not fit: observed spread {observed:.1f} ns sits at"
              f" the {100 * tail:.1f}th percentile")
        print("    of what it would produce, so the intervals above are too narrow")
    print()


def self_check(rng):
    """Recover coefficients planted in synthetic data.

    An instrument that cannot find an effect it was handed has no business
    reporting one it was not. The planted values are deliberately of the size
    this measures: a couple of hundred nanoseconds fixed against a few hundred
    cycles, with run-to-run noise of the size actually seen.
    """
    fixed, cycles, noise = 210.0, 450.0, 6.0
    ghz = np.repeat([2.35, 2.60, 2.85, 3.08], 5)
    x = 1.0 / ghz
    y = fixed + cycles * x + rng.standard_normal(len(x)) * noise

    samples, sigma = posterior(x, y, rng)
    flo, fhi = interval(samples[:, 0])
    clo, chi = interval(samples[:, 1])

    print("self-check on synthetic data")
    print(f"  planted fixed {fixed:.0f} ns, recovered {np.mean(samples[:, 0]):.1f}"
          f" 95% [{flo:.1f}, {fhi:.1f}]")
    print(f"  planted core  {cycles:.0f} cycles, recovered {np.mean(samples[:, 1]):.0f}"
          f" 95% [{clo:.0f}, {chi:.0f}]")

    ok = flo <= fixed <= fhi and clo <= cycles <= chi
    print("  both intervals cover the planted values" if ok else
          "  FAILED: an interval missed the value it was given")
    return 0 if ok else 1


def main(argv):
    rng = np.random.default_rng(20260909)

    if len(argv) > 1 and argv[1] == "--self-check":
        return self_check(rng)
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[-2].strip(), file=sys.stderr)
        return 2

    points, environments = read(argv[1])
    if not points:
        print(f"no runs with a clock recorded in {argv[1]}", file=sys.stderr)
        return 1

    # Distinct to a tenth: the governor holds a point to about a per cent, so
    # rounding finer counts one held clock several times and overstates how
    # well the fit is constrained.
    clocks = sorted({round(g, 1) for runs in points.values() for g, _ in runs})
    if len(clocks) < 3:
        print(f"only {len(clocks)} distinct clocks in {argv[1]}: the fixed and core"
              " parts cannot be separated", file=sys.stderr)
        print("  collect with bench/clock-sweep.sh, which holds several",
              file=sys.stderr)
        return 1

    if environments:
        first = environments[0]
        print(f"on {first['release']}, {first['cores']} cores of {first['chips']}"
              f" chips")
    runs_total = sum(len(v) for v in points.values())
    print(f"{len(clocks)} held clocks from {min(clocks):.1f} to {max(clocks):.1f} GHz,"
          f" {runs_total} runs, fitting t = fixed + cycles / GHz\n")

    for name in sorted(points):
        runs = points[name]
        x = np.array([1.0 / g for g, _ in runs])
        y = np.array([t for _, t in runs])
        drawn = posterior(x, y, rng)
        if drawn is None:
            print(f"  {name}: too few runs to fit\n")
            continue
        samples, sigma = drawn
        describe(name, samples, sigma, x, y, rng)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
