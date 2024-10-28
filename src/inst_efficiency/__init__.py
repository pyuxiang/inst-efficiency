from importlib.metadata import version, PackageNotFoundError

try:
    __version__ = version("inst_efficiency")
except PackageNotFoundError:
    pass
