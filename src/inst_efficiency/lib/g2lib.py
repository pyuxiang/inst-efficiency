import numpy as np

import fpfind.lib.parse_timestamps as parser
from S15lib.g2lib.delta import delta_loop


# Ported from S15lib.g2lib.g2lib, with the timestamp reading
# function replaced with the cleaner 'fpfind' version
def g2_extr(
    filename: str,
    bins: int = 100,
    bin_width: float = 2,
    min_range: int = 0,
    channel_start: int = 0,
    channel_stop: int = 1,
    c_stop_delay: int = 0,
    highres_tscard: bool = False,
    normalise: bool = False,
):
    """Generates G2 histogram from a raw timestamp file

    Args:
        filename (str): timestamp file containing raw data
        bins (int, optional):
            Number of bins for the coincidence histogram. Defaults to 100.
        bin_width (float, optional):
            Bin width of coincidence histogram in nanoseconds. Defaults to 2.
        min_range (int, optional):
            Lower range of correlation in nanoseconds. Defaults to 0.
        channel_start (int, optional): Channel of start events. Defaults to 0.
        channel_stop (int, optional): Channel of stop events. Defaults to 1.
        c_stop_delay (int, optional):
            Adds time (in nanoseconds) to the stop channel time stamps. Defaults to 0.
        highres_tscard (bool, optional):
            Setting for timestamp cards with higher time resolution. Defaults to False.
        normalise (bool, optional):
            Setting to normalise the g2 with N1*N2*dT/T . Defaults to False.

    Raises:
        ValueError: When channel is not between 0 - 3.
            (0: channel 1, 1: channel 2, 2: channel 3, 3: channel 4)

    Returns:
        [int], [float], int, int, int:
            histogram, time differences, events in channel_start,
            events in channel_stop, time at last event
    """

    if channel_start not in range(4):
        raise ValueError("Selected start channel not in range")
    if channel_stop not in range(4):
        raise ValueError("Selected stop channel not in range")

    t, p = parser.read_a1(filename, legacy=True, ignore_rollover=True)
    t1 = t[(p & (1 << channel_start)).astype(bool)].astype(np.float64)
    t2 = t[(p & (1 << channel_stop)).astype(bool)].astype(np.float64)
    if t1.size == 0 and t2.size == 0:
        raise RuntimeError(
            f"No timestamp events recorded in channels {channel_start+1} and {channel_stop+1}.",
        )

    hist = delta_loop(
        t1, t2 - min_range + c_stop_delay, bins=bins, bin_width_ns=bin_width
    )
    try:
        t_max = t[-1] - t[0]
        if normalise:
            N = len(t1) * len(t2) / t_max * bin_width
            hist = hist / N
    except IndexError:
        t_max = 0
        if normalise:
            print("Unable to normalise, intergration time error")
    dt = np.arange(0, bins * bin_width, bin_width)
    return hist, dt + min_range, len(t1), len(t2), t_max
