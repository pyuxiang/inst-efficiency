# inst-efficiency

Python port of 'inst_efficiency.sh' script written at CQT, for use with timestamp7.

## Installation and usage

Requires `gcc`, `make`, and uses `sudo` to write `readevents7` binary to `/usr/bin`.

```bash
git clone --recursive git+https://github.com/pyuxiang/inst-efficiency.git
cd inst_efficiency
make usbtmst4
pip install .

# optional: patch with fpfind for ignore rollover behaviour
cd src/inst_efficiency/lib/pyS15
git apply ../S15lib_w_fpfind.patch
pip install .
```

This exposes the `inst-efficiency` tool on the command line. Some common usage:

```bash
inst-efficiency singles --threshvolt=-0.5
inst-efficiency pairs --config inst-efficiency.findpeak.conf
```

Default configuration files can be generated with:

```bash
inst-efficiency singles --save inst-efficiency.default.conf
```

## Contributing

Version tags follow [semantic versioning](https://semver.org/spec/v2.0.0.html), with a build string indicating the date of release in "YYMMDD" format, e.g. **v2.0.0+2410224**. This allows clear indication to the user whether the local version is severely outdated, while maintaining the clarity of semantic versioning. Versioning is performed by adding a basic git tag associated with the commit, with "v" prepended

Commit messages to roughly follow [Angular commit message guidelines](https://github.com/angular/angular/blob/22b96b9/CONTRIBUTING.md#-commit-message-guidelines) (which aligns with the [Conventional Commits specification](https://www.conventionalcommits.org/en/v1.0.0/)). The type should be one of the following: **feat**, **fix**, **perf**, **refactor**, **style**, **test**, **docs**, **build**.
