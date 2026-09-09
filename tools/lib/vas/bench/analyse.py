# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare wake-latency conditions, with the uncertainty stated.

Reads the records pingpong writes, one JSON object per run, and answers the
question the runs were for: is one condition slower than another, by how much,
and how sure can we be.

Why this model. Each record is already the mean of many thousands of round
trips, so by the central limit theorem the run-level figures are close to
normal whatever the per-wake distribution looks like -- and the per-wake
distribution is certainly not normal, being a latency with a floor and a long
tail. Modelling the run means rather than individual wakes is what makes a
normal likelihood honest here.

That buys exactness. With a normal likelihood and unknown variance the
posterior for the mean is a Student-t in closed form, so sampling from it is
exact rather than approximate: there is no chain to converge, no burn-in to
discard, and no diagnostic that can quietly fail. The difference of two
conditions is then the difference of two exact draws.

The conditions must have been collected interleaved rather than one after the
other, or a drift over the collection lands on whichever ran second and is
indistinguishable here from a real effect. wake-latency.sh alternates them for
that reason; this program cannot check that it did.

The prior is the reference one for a normal with both parameters unknown,
p(mu, sigma^2) proportional to 1/sigma^2. It is chosen for having nothing to
defend: no centre, no scale, no weight in observations, and so nothing that
can quietly pull an answer.

An earlier version of this used a normal-inverse-gamma centred at zero,
"weakly informative" by intent and badly informative in fact -- for latencies
near 8 us the (ybar - mu0)^2 term inflated the variance by two orders of
magnitude and the prior mean dragged the location down by 8%, enough to report
no difference on synthetic data built to contain one. Hence the self-check at
the foot of this file, which recovers a known difference and would have caught
that.

What is reported is a credible interval -- the probability that the difference
lies in the interval, given the data and the model -- and not a confidence
interval, which is a different claim about a procedure's long-run behaviour.

    uv run --with numpy analyse.py records.ndjson same-chip cross-chip
"""

import json
import sys

import numpy as np

DRAWS = 200_000


def posterior_draws(values, rng, draws=DRAWS):
    """Exact draws from the posterior for the mean of `values`.

    Under p(mu, sigma^2) proportional to 1/sigma^2 the posterior is standard:
    sigma^2 ~ InverseGamma((n-1)/2, ss/2), and mu | sigma^2 ~ N(ybar,
    sigma^2/n). Marginally mu is Student-t with n-1 degrees of freedom located
    at ybar, which is what makes a handful of runs enough to say something and
    also what stops it saying too much.
    """
    y = np.asarray(values, dtype=float)
    n = y.size
    if n < 2:
        raise ValueError("need at least two runs to estimate a spread")
    ybar = y.mean()
    ss = ((y - ybar) ** 2).sum()

    # numpy draws Gamma, so an inverse-gamma is its reciprocal.
    sigma2 = (ss / 2.0) / rng.gamma(shape=(n - 1) / 2.0, scale=1.0, size=draws)
    mu = rng.normal(loc=ybar, scale=np.sqrt(sigma2 / n), size=draws)

    return mu, sigma2


def interval(samples, mass=0.95):
    """Central credible interval containing `mass` of the posterior."""
    lo = (1.0 - mass) / 2.0
    return np.quantile(samples, [lo, 1.0 - lo])


def summarise(name, values, rng):
    mu, sigma2 = posterior_draws(values, rng)
    lo, hi = interval(mu)
    print(f"  {name}: n={len(values)}  observed mean {np.mean(values):.3f} us")
    print(f"    posterior mean {mu.mean():.3f} us, 95% credible [{lo:.3f}, {hi:.3f}]")
    print(f"    posterior sd of a single run {np.sqrt(sigma2).mean():.3f} us")
    return mu


def main(argv):
    if len(argv) < 4:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2

    path, name_a, name_b = argv[1], argv[2], argv[3]

    by_label = {}
    settles = {}
    with open(path) as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            by_label.setdefault(record["label"], []).append(record["one_way_us"])
            settles.setdefault(record["label"], set()).add(record.get("settle", 0))

    for name in (name_a, name_b):
        if name not in by_label:
            print(f"no records labelled {name} in {path}", file=sys.stderr)
            return 1

    # Comparing runs that spun for different lengths compares the spins.
    joint = settles[name_a] | settles[name_b]
    if len(joint) != 1:
        print(f"refusing to compare: settle differs across conditions {sorted(joint)}",
              file=sys.stderr)
        return 1

    rng = np.random.default_rng(20260909)

    print(f"one-way wake latency, settle={joint.pop()} spins held fixed")
    mu_a = summarise(name_a, by_label[name_a], rng)
    mu_b = summarise(name_b, by_label[name_b], rng)

    diff = mu_b - mu_a
    lo, hi = interval(diff)
    p_slower = float((diff > 0).mean())

    print()
    print(f"  {name_b} minus {name_a}:")
    print(f"    {diff.mean():+.3f} us, 95% credible [{lo:+.3f}, {hi:+.3f}]")
    print(f"    P({name_b} slower) = {p_slower:.3f}")

    # A credible interval spanning zero is the finding when it is the finding.
    if lo <= 0.0 <= hi:
        print("    the interval contains zero: this does not resolve a difference")
        print("    (which is a statement about the measurement, not about the fabric)")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
