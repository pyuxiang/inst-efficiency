#!/usr/bin/env python3
"""Simple script to continuously feed pair source optimization statistics

Python port of 'inst_efficiency.sh' functionality written in CQT.
Supports reading of singles, pairs, and other miscellaneous features.
Interface via CLI only, to avoid unnecessary GUI dependencies.

By default, the start and stop channels are set as channels 1 and 4.
This can be configured in advanced options - see advanced help with '-hh'.

Changelog:
    2022-12-01, Justin: Initial code

Examples:

    1. View available configuration options

       ./inst_efficiency.py --help


    2. TTL input pulses with 2s integration time

       ./inst_efficiency.py singles \
           -U /dev/ioboards/usbtmst1 \
           -S /home/sfifteen/programs/usbtmst4/apps/readevents7 \
           --threshvolt 1 \
           --time 2


    3. Search for pairs between detector channels 1 and 2, over +/-250ns,
       showing histogram of coincidences for each dataset

       ./inst_efficiency.py pairs -qH --ch_start 1 --ch_stop 2


    4. Calculate total pairs located at +118ns delay, within a 2ns-wide
       coincidence window spanning +117ns to +118ns, with only 20 bins

       ./inst_efficiency.py pairs -q --peak 118 --left=-1 --right=0 --bins 20


    5. Log measurements into a file

       ./inst_efficiency.py pairs -q --logging pair_measurements


    6. Save configuration from (4) into default config file

       ./inst_efficiency.py pairs -q --peak 118 -L=-1 -R 0 --bins 20 \
           --save ./inst_efficiency.py.default.conf


    7. Load multiple configuration

       > cat ./inst_efficiency.py.default.conf
       bins = 10
       peak = 200

       > cat ./asympair
       peak = 118
       time = 2

       # Output yields 'bins=10', 'peak=118', 'time=3'
       ./inst_efficiency.py pairs -c asympair --time 3
"""

import datetime as dt
import sys
from copy import deepcopy

import numpy as np
from S15lib.instruments import TimestampTDC2

import kochen.scriptutil
import kochen.logging

import inst_efficiency.lib.g2lib as g2
from inst_efficiency.lib.color import nostyle as style, get_style, len_ansi, strip_ansi

logger = kochen.logging.get_logger(__name__)

# Constants
INT_MIN = np.iinfo(np.int64).min  # indicate invalid value in int64 array


def print_fixedwidth(*values, width=7, out=None, pbar=None, end="\n"):
    """Prints right-aligned columns of fixed width.

    Note:
        The default column width of 7 is predicated on the fact that
        10 space-separated columns can be comfortably squeezed into a
        80-width terminal (with an extra buffer for newline depending
        on the shell).
    """
    row = []
    for value in values:
        if value == INT_MIN:
            row.append(" " * width)
        else:
            # Measure length with ANSI control chars removed
            value = str(value)
            slen = max(0, width - len_ansi(value))
            row.append(" " * slen + value)
    line = " ".join(row)

    if pbar:
        pbar.set_description(line)
    else:
        print(line, end=end)
    if out:
        line = " ".join(
            [
                f"{strip_ansi(str(value)) if value != INT_MIN else ' ': >{width}s}"
                for value in values
            ]
        )
        with open(out, "a") as f:
            f.write(line + "\n")


#############
#  SCRIPTS  #
#############

# Collect program names
PROGRAMS = {}


def _collect_as_script(alias=None):
    """Decorator to dynamically collect functions for use as scripts."""

    def collector(f):
        nonlocal alias
        if alias is None:
            alias = f.__name__
        PROGRAMS[alias] = f
        return f

    return collector


def read_pairs(params, use_cache=False):
    """Compute single pass pair statistics.

    Note:
        Parameter dictionary passed instead of directly into kwargs, since:
            1. Minimize dependency with parser argument names
            2. Functions in the stack can reuse arguments,
               e.g. monitor_pairs -> read_pairs
    """

    # Unpack arguments into aliases
    bin_width = params.width
    bins = params.bins
    peak = params.peak
    roffset = params.right
    loffset = params.left
    duration = params.time
    darkcounts = [
        params.darkcount1,
        params.darkcount2,
        params.darkcount3,
        params.darkcount4,
    ]
    channel_start = params.ch_start - 1
    channel_stop = params.ch_stop - 1
    timestamp = params.timestamp

    darkcount_start = darkcounts[channel_start]
    darkcount_stop = darkcounts[channel_stop]
    window_size = roffset - loffset + 1
    acc_start = max(bins // 2, 1)  # location to compute accidentals
    while True:
        # Invoke timestamp data recording
        if not use_cache:
            timestamp._call_with_duration(["-a1", "-X"], duration=duration)

        # Extract g2 histogram and other data
        data = g2.g2_extr(
            params.tmpfile,
            channel_start=channel_start,
            channel_stop=channel_stop,
            highres_tscard=True,
            bin_width=bin_width,
            bins=bins,
            # Include window at position 1
            min_range=peak + loffset - 1,
        )
        hist = data[0]
        s1, s2 = data[2:4]
        inttime = data[4] * 1e-9  # convert to units of seconds

        # Integration time check for data validity
        if not (0.75 < inttime / duration < 2):
            continue

        # Calculate statistics
        acc = window_size * np.mean(hist[acc_start:])
        pairs = sum(hist[1 : 1 + window_size]) - acc

        # Normalize to per unit second
        s1 = s1 / inttime - darkcount_start  # timestamp data more precise
        s2 = s2 / inttime - darkcount_stop
        pairs = pairs / inttime
        acc = acc / inttime
        if params.accumulate:
            s1 = s1 * inttime
            s2 = s2 * inttime
            pairs = pairs * inttime
            acc = acc * inttime

        if s1 == 0 or s2 == 0:
            e1 = e2 = eavg = 0
        else:
            e1 = max(100 * pairs / s2, 0.0)
            e2 = max(100 * pairs / s1, 0.0)
            eavg = max(100 * pairs / (s1 * s2) ** 0.5, 0.0)

        # Single datapoint collection completed
        break

    return hist, inttime, pairs, acc, s1, s2, e1, e2, eavg


@_collect_as_script("pairs_once")
def print_pairs(params):
    """Pretty printed variant of 'read_pairs', showing pairs, acc, singles."""
    _, _, pairs, acc, s1, s2, _, _, _ = read_pairs(params)
    print_fixedwidth(
        round(pairs, 1), round(acc, 1), int(s1), int(s2),
        width=0,
    )  # fmt: skip


@_collect_as_script("pairs")
def monitor_pairs(params):
    """Prints out pair source statistics, between ch1 and ch4."""
    # Unpack arguments into aliases
    peak = params.peak
    roffset = params.right
    loffset = params.left
    hist_verbosity = params.histogram
    logfile = params.logging

    is_header_logged = False
    i = 0
    is_initialized = False
    prev = None
    longterm_data = {"count": 0, "inttime": 0, "pairs": 0, "acc": 0, "s1": 0, "s2": 0}
    while True:
        hist, inttime, pairs, acc, s1, s2, e1, e2, eavg = read_pairs(params)

        # Visualize g2 histogram
        HIST_ROWSIZE = 10
        if hist_verbosity > 1 or (hist_verbosity == 1 and not is_initialized):
            is_initialized = True
            a = np.array(hist, dtype=np.int64)
            # Append NaN values until fits number of rows
            a = np.append(a, np.resize(INT_MIN, HIST_ROWSIZE - (a.size % HIST_ROWSIZE)))
            if hist_verbosity > 0:
                print("\nObtained histogram:")
                for row in a.reshape(-1, HIST_ROWSIZE):
                    print_fixedwidth(*row)
            peakvalue = max(a)
            peakargmax = np.argmax(a)
            peakpos = peakargmax + peak + loffset - 1
            print(f"Maximum {peakvalue} @ index {peakpos}")

            # Display current window as well
            window_size = roffset - loffset + 1
            current_window = hist[1 : window_size + 1]
            print(f"Current window: {list(map(int, current_window))}")

            # Display likely window
            likely_window = [peakvalue]
            likely_left = None
            likely_right = None
            acc_bin = acc / window_size
            # Scan below
            i = 0
            while True:
                i += 1
                pos = peakargmax - i
                value = a[pos]
                if value > 2 * acc_bin:
                    likely_window = [value] + likely_window
                else:
                    likely_left = -(i - 1)
                    break
            i = 0
            while True:
                i += 1
                pos = peakargmax + i
                value = a[pos]
                if value > 2 * acc_bin:
                    likely_window = likely_window + [value]
                else:
                    likely_right = i - 1
                    break
            likely_window = a[likely_left + peakargmax : likely_right + 1 + peakargmax]
            print(f"Likely window: {list(map(int, likely_window))}")
            print(
                f"Args: --peak={peakpos} --left={likely_left} --right={likely_right}\n"
            )

        # Print the header line after every 10 lines
        if i == 0 or hist_verbosity > 1:
            i = 10
            print_fixedwidth(
                "TIME", "ITIME",
                "PAIRS", "ACC", "SINGLE1", "SINGLE2",
                "EFF1", "EFF2", "EFF_AVG",
                out=logfile if not is_header_logged else None,
            )  # fmt: skip
            is_header_logged = True
        i -= 1

        # Print statistics
        print_fixedwidth(
            style(dt.datetime.now().strftime("%H%M%S"), style="dim"),
            f"{inttime:.2f}",
            style(int(pairs), style="bright"),
            f"{acc:.1f}",
            style(int(s1), fg="yellow", style="bright"),
            style(int(s2), fg="green", style="bright"),
            f"{e1:.2f}",
            f"{e2:.2f}",
            style(f"{eavg:.2f}", fg="cyan", style="bright"),
            out=logfile,
        )

        # Print long-term statistics, only if value supplied
        if params.avgtime > 0:
            # Update first
            longterm_data["count"] += 1
            longterm_data["inttime"] += inttime
            longterm_data["pairs"] += pairs
            longterm_data["acc"] += acc
            longterm_data["s1"] += s1
            longterm_data["s2"] += s2

            # Cache long term results if reach threshold
            if longterm_data["inttime"] >= params.avgtime:
                counts = longterm_data["count"]
                inttime = longterm_data["inttime"]
                p = longterm_data["pairs"] / counts
                acc = longterm_data["acc"] / counts
                s1 = longterm_data["s1"] / counts
                s2 = longterm_data["s2"] / counts
                prev = (
                    dt.datetime.now().strftime("%H%M%S"),
                    round(inttime, 2),
                    style(int(round(p, 0)), fg="red", style="bright"),
                    round(acc, 1),
                    int(round(s1, 0)),
                    int(round(s2, 0)),
                    round(100 * p / s2, 1),
                    round(100 * p / s1, 1),
                    style(
                        round(100 * p / (s1 * s2) ** 0.5, 1), fg="red", style="bright"
                    ),
                )
                longterm_data = {k: 0 for k in longterm_data.keys()}  # reset counts

            # Print if exists
            if prev:
                print_fixedwidth(*prev, end="\r")


@_collect_as_script("singles")
def monitor_singles(params):
    """Prints out singles statistics."""
    # Unpack arguments into aliases
    duration = params.time
    logfile = params.logging

    is_header_logged = False
    i = 0
    avg = np.array([0, 0, 0, 0])  # averaging facility, e.g. for measuring dark counts
    avg_iters = 0
    while True:
        # Invoke timestamp data recording
        data = params.timestamp.get_counts(
            duration=duration,
            return_actual_duration=True,
        )
        counts = np.array(data[:4])
        inttime = data[4]

        # Rough integration time check
        if not (0.75 < inttime / duration < 2):
            continue
        if any(counts < 0):
            continue
        counts = counts / inttime - params.darkcounts
        if params.accumulate:
            counts = counts * inttime

        # Implement rolling average to avoid overflow
        if params.average:
            avg_iters += 1
            avg = (avg_iters - 1) / avg_iters * avg + np.array(counts) / avg_iters
            counts = np.round(avg, 1)

        # Print the header line after every 10 lines
        if i == 0:
            i = 10
            print_fixedwidth(
                "TIME", "INTTIME", "CH1", "CH2", "CH3", "CH4", "TOTAL",
                out=logfile if not is_header_logged else None,
            )  # fmt: skip
            is_header_logged = True
        i -= 1

        # Print statistics
        print_fixedwidth(
            style(dt.datetime.now().strftime("%H%M%S"), style="dim"),
            f"{inttime:.2f}",
            *list(map(int, counts)),
            style(int(sum(counts)), style="bright"),
            out=logfile,
        )


def read_2pairs(params):
    """Prints out pair source statistics, between ch1 and ch4."""
    # Hardcoded hohoho
    override1_params = {
        "ch_start": 1,
        "ch_stop": 2,
        "peak": 219,
        "left": -1,
        "right": 1,
    }
    override2_params = {
        "ch_start": 3,
        "ch_stop": 4,
        "peak": 190,
        "left": -1,
        "right": 1,
    }

    _params = duplicate_args(params)
    vars(_params).update(override1_params)
    _, _, p1, a1, s11, s12, *_ = read_pairs(_params)
    vars(_params).update(override2_params)
    _, _, p2, a2, s21, s22, *_ = read_pairs(_params, use_cache=True)
    return p1, a1, s11, s12, p2, a2, s21, s22


@_collect_as_script("visibility")
def print_2pairs(params):
    """Pretty printed variant of 'read_pairs', showing pairs, acc, singles."""
    p1, a1, s11, s12, p2, a2, s21, s22 = read_2pairs(params)
    print(f"{p1} {p2} {a1} {a2}")


@_collect_as_script("2pairs")
def monitor_2pairs(params):
    """Prints out pair source statistics, between ch1 and ch4."""
    logfile = params.logging
    is_header_logged = False
    i = 0
    while True:
        p1, a1, s11, s12, p2, a2, s21, s22 = read_2pairs(params)

        # Print the header line after every 10 lines
        if i == 0:
            i = 10
            print_fixedwidth(
                "TIME",
                "P1", "A1", "S11", "S12",
                "P2", "A2", "S21", "S22",
                out=logfile if not is_header_logged else None,
            )  # fmt: skip
            is_header_logged = True
        i -= 1

        # Print statistics
        print_fixedwidth(
            style(dt.datetime.now().strftime("%H%M%S"), style="dim"),
            style(int(p1), fg="yellow", style="bright"),
            style(round(a1, 1), style="bright"),
            int(s11),
            int(s12),
            style(int(p2), fg="green", style="bright"),
            style(round(a2, 1), style="bright"),
            int(s21),
            int(s22),
            out=logfile,
        )


##########################
#  PRE-SCRIPT EXECUTION  #
##########################


def duplicate_args(args):
    """Makes a copy of the argparse.Namespace object."""
    attrs = vars(args)
    attrs_copy = {k: deepcopy(v) for k, v in attrs.items()}
    return type(args)(**attrs_copy)


def main():
    global style

    # fmt: off
    def make_parser(help_verbosity: int = 1):
        adv = kochen.scriptutil.get_help_descriptor(help_verbosity >= 2)
        parser, default_config = kochen.scriptutil.generate_default_parser_config(__doc__, display_config=help_verbosity >= 2)

        # Boilerplate
        pgroup = parser.add_argument_group("display/configuration")
        pgroup.add_argument(
            "-h", "--help", action="count", default=0,
            help="Show this help message, with incremental verbosity, e.g. -hh")
        pgroup.add_argument(
            "-v", "--verbosity", action="count", default=0,
            help=adv("Specify debug verbosity, e.g. -vv for more verbosity"))
        pgroup.add_argument(
            "-L", "--logging", metavar="",
            help="Log to file, if specified. Log level follows verbosity.")
        pgroup.add_argument(
            "-q", "--quiet", action="store_true",
            help=adv("Suppress errors, does not block logging"))
        pgroup.add_argument(
            "-c", "--config", metavar="", is_config_file_arg=True,
            help=f"Path to configuration file (default: '{default_config}')")
        pgroup.add_argument(
            "--save", metavar="", is_write_out_config_file_arg=True,
            help=adv("Path to configuration file for saving, then immediately exit"))
        pgroup.add_argument(
            "--no-color", action="store_true",
            help=adv("Disable color highlighting for stdout text"))

        # Device-level argument
        pgroup = parser.add_argument_group("device configuration")
        pgroup.add_argument(
            "-U", "--device", metavar="", default="/dev/ioboards/usbtmst0",
            help="Path to timestamp device (default: '/dev/ioboards/usbtmst0')")
        pgroup.add_argument(
            "-S", "--readevents", metavar="", default="/usr/bin/readevents7",
            help=adv("Path to readevents binary (default: '/usr/bin/readevents7')"))
        pgroup.add_argument(
            "-O", "--tmpfile", metavar="", default="/tmp/quick_timestamp",
            help=adv("Path to temporary file for timestamp storage (default: '/tmp/quick_timestamp')"))
        pgroup.add_argument(
            "-t", "--threshvolt", metavar="", type=float, default="-0.4",
            help="Pulse trigger level for each detector channel, comma-delimited (default: -0.4)")
        pgroup.add_argument(
            "-f", "--fast", action="store_true",
            help="[TDC2] Enable fast event readout mode, i.e. 32-bit wide events.")

        # Script-level arguments
        pgroup = parser.add_argument_group("global configuration")
        pgroup.add_argument(
            "script", choices=PROGRAMS)
        pgroup.add_argument(
            "-T", "--time", metavar="", type=float, default=1.0,
            help="Integration time for timestamp, in s (default: 1.0)")
        pgroup.add_argument(
            "--accumulate", action="store_true",
            help=adv("Print raw counts, without normalizing to counts/s"))
        pgroup.add_argument(
            "--darkcount1", "--dc1", metavar="", type=float, default=0.0,
            help=adv("Dark-count level for channel 1, in counts/s"))
        pgroup.add_argument(
            "--darkcount2", "--dc2", metavar="", type=float, default=0.0,
            help=adv("Dark-count level for channel 2, in counts/s"))
        pgroup.add_argument(
            "--darkcount3", "--dc3", metavar="", type=float, default=0.0,
            help=adv("Dark-count level for channel 3, in counts/s"))
        pgroup.add_argument(
            "--darkcount4", "--dc4", metavar="", type=float, default=0.0,
            help=adv("Dark-count level for channel 4, in counts/s"))

        pgroup = parser.add_argument_group("[singles] options")
        pgroup.add_argument(
            "--average", action="store_true",
            help=adv("Print long-term average singles instead"))

        pgroup = parser.add_argument_group("[pairs] options")
        pgroup.add_argument(
            "-H", "--histogram", action="count", default=0,
            help="Enable histogram, e.g. '-HH' for continuous histogram")
        pgroup.add_argument(
            "-W", "--width", metavar="", type=float, default=1,
            help="Width of coincidence time bins, in ns")
        pgroup.add_argument(
            "-B", "--bins", metavar="", type=int, default=500,
            help="Number of coincidence time bins, in units of 'width'")
        pgroup.add_argument(
            "--peak", metavar="", type=int, default=-250,
            help="Absolute bin position of coincidence window, in units of 'width'")
        pgroup.add_argument(
            "--left", metavar="", type=int, default=0,
            help="Left offset of coincidence window relative to peak")
        pgroup.add_argument(
            "--right", metavar="", type=int, default=0,
            help="Right offset of coincidence window relative to peak")
        pgroup.add_argument(
            "--avgtime", metavar="", type=float, default=0.0,
            help=adv("Auxiliary long-term integration time, in seconds"))
        pgroup.add_argument(
            "--ch_start", "--start", metavar="", type=int, default=1,
            help=adv("Reference timestamp channel for calculating time delay offset"))
        pgroup.add_argument(
            "--ch_stop", "--stop", metavar="", type=int, default=4,
            help=adv("Target timestamp channel for calculating time delay offset"))

        return parser
    # fmt: on

    # Parse arguments and configure logging
    parser = make_parser()
    args = kochen.scriptutil.parse_args_or_help(parser, parser_func=make_parser)
    kwargs = {}
    if args.quiet:
        kwargs["stream"] = None
    kochen.logging.set_default_handlers(logger, file=args.logging, **kwargs)
    kochen.logging.set_logging_level(logger, args.verbosity)
    logger.debug("%s", args)

    # Silence all errors/tracebacks
    if args.quiet:
        sys.excepthook = lambda etype, e, tb: print()

    style = get_style(not args.no_color)

    # Initialize timestamp
    timestamp = TimestampTDC2(
        device_path=args.device,
        readevents_path=args.readevents,
        outfile_path=args.tmpfile,
    )
    timestamp.threshold = args.threshvolt
    timestamp.fast = args.fast

    # Collect required arguments
    args.timestamp = timestamp
    args.darkcounts = np.array(
        [
            args.darkcount1,
            args.darkcount2,
            args.darkcount3,
            args.darkcount4,
        ]
    )

    # Call script
    try:
        PROGRAMS[args.script](args)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
