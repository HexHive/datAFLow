#!/usr/bin/env python3

"""
Compute summary statistics for a set of coverage results (in JSON format).

Author: Adrian Herrera
"""


from pathlib import Path
import json
import sys

from bootstrapped import bootstrap as bs
import bootstrapped.stats_functions as bs_stats
import numpy as np


def get_final_cov(p: Path) -> int:
    """
    Get the last coverage element in the coverage-over-inputs list (which
    corresponds to the max. coverage).

    Each coverage element has the format:

    ```
    [TEST_CASE_ID, COV]
    ```

    We ignore the `TEST_CASE_ID` and only look at the `COV` integer.
    """
    with p.open() as inf:
        root = json.load(inf)
        return root[-1][1]


def main(args):
    """The main function."""
    if len(args) <= 1:
        print(f'usage: {args[0]} /path/to/json [...]')
        return 1

    # Get coverage
    covs = np.array([get_final_cov(Path(arg)) for arg in args[1:]])

    # Calculate mean and confidence intervals
    cov_ci = bs.bootstrap(covs, stat_func=bs_stats.mean)

    # Output
    print(f'mean coverage ({len(covs)} trials)')
    print(f'  {cov_ci.value:.02f} +/- {cov_ci.error_width() / 2:.02f}')

    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
