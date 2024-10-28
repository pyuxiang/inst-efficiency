# inst-efficiency

Python port of 'inst_efficiency.sh' script written at CQT, for use with timestamp7.

## Installation

Requires `gcc`, `make`, and uses `sudo` to write `readevents7` binary to `/usr/bin`.

```bash
pip3 install git+https://github.com/pyuxiang/inst-efficiency.git
```

----

## Usage

Requirements:

* Python 3.8 and above, running in Linux
* `gcc` (if running `freqcd`, preferably in PATH for auto-compilation)

```bash
# Remote installation
pip3 install git+https://github.com/s-fifteen-instruments/fpfind.git

# Local installation
git clone https://github.com/s-fifteen-instruments/fpfind.git
cd fpfind && pip3 install .
```

Binaries and scripts will be exposed to the path; commonly used scripts are listed below.

```bash
fpfind -t {TIMESTAMPS1} -T {TIMESTAMPS2}
freqcd -X -udF fpipe -f 568 < {TIMESTAMPS}
[costream -V5 ... |] freqservo -V5 -udF fpipe -f 568
parse_timestamps -A1 -X -p {TIMESTAMPS}
```


## Contributing

Version tags (as of **v2**) follow [semantic versioning](https://semver.org/spec/v2.0.0.html), with a build string indicating the date of release in "YYMMDD" format, e.g. **v2.0.0+2410224**. This allows clear indication to the user whether the local version is severely outdated, while maintaining the clarity of semantic versioning. Versioning is performed by:

1. Modifying the **version** field in `pyproject.toml`, with no "v"
1. Adding a basic git tag associated with the commit, with "v" prepended

Commit messages to roughly follow [Angular commit message guidelines](https://github.com/angular/angular/blob/22b96b9/CONTRIBUTING.md#-commit-message-guidelines) (which aligns with the [Conventional Commits specification](https://www.conventionalcommits.org/en/v1.0.0/)). The type should be one of the following: **feat**, **fix**, **perf**, **refactor**, **style**, **test**, **docs**.
If a scope is provided, it should be one of:

* **fpplot**
* **freqcd**
* **freqservo**
* **parser** (for both timestamp and epoch)

Commit messages can be automatically checked using `pre-commit`, after installing:

```bash
pre-commit install --hook-type commit-msg
```
