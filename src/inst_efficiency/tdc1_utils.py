import time
import pathlib
import sys

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
    while timestamp._com.in_waiting:
        timestamp._com.readlines()  # empties buffer
    return timestamp


def g2_extr(
    timestamp, duration, channel_start, channel_stop, min_range, bins, bin_width
):
    t, p = timestamp.get_timestamps(duration)
    p = np.array([int(p_, base=2) for p_ in p])

    # Adapted from S15lib.g2lib.g2lib.g2_extr()
    t1 = t[(p & (1 << channel_start)).astype(bool)].astype(np.float64)
    t2 = t[(p & (1 << channel_stop)).astype(bool)].astype(np.float64)
    if t1.size == 0 and t2.size == 0:
        raise RuntimeError(
            "No timestamp events recorded in channels "
            f"{channel_start + 1} and {channel_stop + 1}."
        )
    hist = g2.delta_loop(t1, t2 - min_range, bins=bins, bin_width_ns=bin_width)
    t_max = 0
    if len(t) > 0:
        t_max = t[-1] - t[0]
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
