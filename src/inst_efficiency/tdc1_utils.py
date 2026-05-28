import pathlib
import sys
import time
import types

import numpy as np
import S15lib.g2lib.g2lib as g2
from S15lib.instruments.usb_counter_fpga import TimestampTDC1


def find_serial_device_linux(pattern=""):
    """Copied from 'physicsutils.devices.common._serial' module."""
    pattern = "*" if pattern == "" else f"*{pattern}*"
    paths = tuple(pathlib.Path("/dev/serial/by-id/").glob(pattern))
    if len(paths) < 1:
        raise ValueError("No device found")
    elif len(paths) == 1:
        return str(paths[0])  # for compatibility with pyserial
    else:
        pathnames = tuple(map(lambda p: p.name, paths))
        raise ValueError(f"Multiple devices found: {pathnames}")


def load(args):
    # Use bootstrapped device finder instead of S15lib's probe
    if sys.platform.startswith("linux") or sys.platform.startswith("cygwin"):
        args.device = find_serial_device_linux("TDC1")

    timestamp = TimestampTDC1(device_path=args.device)
    timestamp.threshold = args.threshvolt
    timestamp.accumulated_timestamps_filename = args.tmpfile
    timestamp._com.write(b"abort\r\n")  # terminate any existing streams
    timestamp._com.readlines()  # drain buffer

    # 2026-05-25, on TDC1-0049, there is a bug that occurs after TDC1 enters timestamp mode,
    # and requires a second line termination to actually send the command. A single write will
    # thus not yield anything, e.g. calling 'timestamp.mode' will attempt to execute 'int("")'
    # that fails with ValueError. Using this test, we patch extra enter commands.
    try:
        timestamp.mode  # required!
    except ValueError:

        def write(self, message: bytes, *args, **kwargs):
            message = b";;".join(message.split(b";"))
            message = b"\n\r\n".join(message.split(b"\n"))
            self._write(message, *args, **kwargs)

        timestamp._com._write = timestamp._com.write  # pyright: ignore[reportAttributeAccessIssue]
        timestamp._com.write = types.MethodType(write, timestamp._com)
        timestamp._com.write(b"abort\r\n")  # terminate any existing streams
        timestamp._com.readlines()  # drain buffer

    return timestamp


def g2_extr(
    timestamp, duration, channel_start, channel_stop, min_range, bins, bin_width
):
    t, p = timestamp.get_timestamps(duration)
    p = np.array([int(p_, base=2) for p_ in p])

    # Adapted from S15lib.g2lib.g2lib.g2_extr()
    t1 = t[(p & (1 << channel_start)).astype(bool)].astype(np.float64)
    t2 = t[(p & (1 << channel_stop)).astype(bool)].astype(np.float64)
    t_max = 0
    if len(t) > 0:
        t_max = t[-1] - t[0]

    if t1.size == 0 or t2.size == 0:
        hist = np.zeros(bins)
    else:
        hist = g2.delta_loop(t1, t2 - min_range, bins=bins, bin_width_ns=bin_width)
    data = (hist, None, len(t1), len(t2), t_max)
    return data


def _stream_timestamps(self, filename, duration: float = 1):
    """Injected function for timestamp streaming over specified duration."""
    while self._com.in_waiting:
        self._com.readlines()  # empties buffer

    # Initialization from S15lib.instruments.TimestampTDC1
    # '_continuous_stream_timestamps_to_file()'
    self.mode = "singles"
    level = float(self.level.split()[0])
    level_str = "NEG" if level < 0 else "POS"
    self._com.readlines()  # empties buffer
    cmd_str = f"INPKT;{level_str} {level};time 0;timestamp;counts?;"
    self._com.write(f"{cmd_str}\r\n".encode())

    # Naive timestamp streaming
    end_time = time.time() + duration
    with open(filename, "wb+") as f:
        while time.time() <= end_time:
            buffer = self._com.read((1 << 20) * 4)
            f.write(buffer)

    # Terminate streaming
    self._com.write(b"abort\r\n")
    self._com.readlines()
