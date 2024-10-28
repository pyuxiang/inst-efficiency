import subprocess

from hatchling.builders.hooks.plugin.interface import BuildHookInterface


class BuildHook(BuildHookInterface):
    def initialize(self, version, build_data):
        BIN = "readevents7"
        SRC = "src/inst_efficiency/lib/usbtmst4/apps"
        TARGET = f"/usr/bin/{BIN}"

        subprocess.check_output(
            f"[ -f {TARGET} ] || {{ make -C {SRC} {BIN} && sudo cp -p {SRC}/{BIN} {TARGET}; }}",
            shell=True,
        )
