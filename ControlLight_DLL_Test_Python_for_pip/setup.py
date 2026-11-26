from setuptools import setup
from setuptools.dist import Distribution

# The package contains one compiled module per supported Python version,
#   control_light_api/control_light_api.cp310-win_amd64.pyd
#   control_light_api/control_light_api.cp313-win_amd64.pyd
# plus ControlLight.dll. control_light_api/__init__.py imports the module matching the running Python.
# The modules are built with ControlLight's CMake BUILD_MODE=PYD (once per Python version) and copied here
# by the build if CONTROL_LIGHT_STAGE_EXAMPLES is on. To support another Python version, build and add its module
# and add it to the classifiers below.

class BinaryDistribution(Distribution):
    def has_ext_modules(self):
        return True

setup(
    name="control_light_api",
    version="0.1.0",
    author="OpticsFoundry",
    description="Python bindings for ControlLight C++ API",
    packages=["control_light_api"],
    package_data={"control_light_api": ["*.pyd", "ControlLight.dll"]},
    include_package_data=True,
    zip_safe=False,
    distclass=BinaryDistribution,
    python_requires=">=3.10",
    classifiers=[
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.13",
        "Operating System :: Microsoft :: Windows",
    ],
)
