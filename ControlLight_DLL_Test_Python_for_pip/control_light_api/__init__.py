"""Python bindings for the OpticsFoundry ControlLight API (ControlLight.dll, Windows x64).

This package contains one compiled module per supported Python version
(control_light_api.cp<version>-win_amd64.pyd). Python loads the one matching the running interpreter.
"""

import os as _os
import sys as _sys

try:
    from .control_light_api import *  # noqa: F401,F403
except ImportError as _error:
    _here = _os.path.dirname(_os.path.abspath(__file__))
    _available = sorted(f for f in _os.listdir(_here) if f.startswith("control_light_api.") and f.endswith(".pyd"))
    raise ImportError(
        "control_light_api: can't load the compiled module for Python %d.%d from %s (%s). Available modules: %s. "
        "To support this Python version, build ControlLight with BUILD_MODE=PYD using this Python and copy the "
        "resulting .pyd file and ControlLight.dll into this folder."
        % (_sys.version_info[0], _sys.version_info[1], _here, _error, ", ".join(_available) or "none")
    ) from _error
