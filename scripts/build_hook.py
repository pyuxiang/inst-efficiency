import subprocess

from hatchling.builders.hooks.plugin.interface import BuildHookInterface

class BuildHook(BuildHookInterface):
    def initialize(self, version, build_data):
        subprocess.check_output("make -C src/inst_efficiency/lib/usbtmst4/apps readevents7", shell=True)
        subprocess.check_output("sudo cp -p src/inst_efficiency/lib/usbtmst4/apps/readevents7 /usr/bin/readevents7", shell=True)
