# inst-efficiency

Python port of 'inst_efficiency.sh' script written at CQT, for S-Fifteen Instruments TDC1 and TDC2.

## Installation

Requires `gcc`, `make`, and uses `sudo` to write `readevents7` binary to `/usr/bin`.

```bash
git clone https://github.com/pyuxiang/inst-efficiency.git
cd inst_efficiency
make usbtmst4
pip install .
```

Kernel headers are also needed to build the kernel module:

* openSUSE: `zypper in kernel-devel`

If only using for TDC1, installation is as simple as:

```bash
pip install git+https://github.com/pyuxiang/inst-efficiency.git
```

## Usage

### Quickstart

This exposes the `inst-efficiency` tool on the command line. To measure singles from NIM pulses:

```bash
inst-efficiency singles --threshvolt=-0.5  # for TDC2
inst-efficiency singles --tdc1 --threshvolt=-0.5  # for TDC1
```

### More detailed examples

View available configuration options:

```bash
inst-efficiency --help
```

TTL input pulses with 2s integration time, with custom timestamp location:

```bash
inst-efficiency singles \
    -U /dev/ioboards/usbtmst1 \
    -S /home/sfifteen/programs/usbtmst4/apps/readevents7 \
    --threshvolt 1 \
    --time 2
```

Search for pairs between detector channels 1 and 2, over +/-250ns,
showing histogram of coincidences for each dataset:

```bash
inst-efficiency pairs -qH --ch_start 1 --ch_stop 2
```

Calculate total pairs located at +118ns delay, within a 2ns-wide
coincidence window spanning +117ns to +118ns, with only 20 bins:

```bash
inst-efficiency pairs -q --peak 118 --left=-1 --right=0 --bins 20
```

Log measurements into a file:

```bash
inst-efficiency pairs -q --logging pair_measurements
```

Save configuration from (4) into default config file:

```bash
inst-efficiency pairs -q --peak 118 -L=-1 -R 0 --bins 20 \
    --save ./inst-efficiency.default.conf
```

Default configuration files can be generated with:

```bash
inst-efficiency singles --save inst-efficiency.default.conf
```

Load multiple configuration

```bash
cat ./inst-efficiency.default.conf  # bins = 10, peak = 200
cat ./asympair  # peak = 118, time = 2
#
inst-efficiency pairs -c asympair --time 3  # output yields 'bins=10', 'peak=118', 'time=3'
```

Runs a service for other processes to remotely query timestamp:

```bash
inst-efficiency service ...  # listens on *:4440/tcp
```

## Contributing

Version tags follow [semantic versioning](https://semver.org/spec/v2.0.0.html), with a build string indicating the date of release in "YYMMDD" format, e.g. **v2.0.0+2410224**. This allows clear indication to the user whether the local version is severely outdated, while maintaining the clarity of semantic versioning. Versioning is performed by adding a basic git tag associated with the commit, with "v" prepended

Commit messages to roughly follow [Angular commit message guidelines](https://github.com/angular/angular/blob/22b96b9/CONTRIBUTING.md#-commit-message-guidelines) (which aligns with the [Conventional Commits specification](https://www.conventionalcommits.org/en/v1.0.0/)). The type should be one of the following: **feat**, **fix**, **perf**, **refactor**, **style**, **test**, **docs**, **build**.
