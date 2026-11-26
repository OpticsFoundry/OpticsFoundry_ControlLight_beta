# Qt wrapper for ControlLight.dll

The Qt examples (`ControlLight_DLL_Test_Qt_console`, `ControlLight_DLL_Test_Qt_GUI`) are built with MinGW, which can't use `ControlLight/ControlAPI.h` (it depends on MFC). They use the class `CControlLightAPI` from this folder instead. It loads `ControlLight.dll` with `QLibrary`, resolves the API functions by name, and offers each API function `CLA_<name>` as method `<name>`, with the same parameters (`ERROR_CODE_TYPE` is `bool`).

| File | Content |
|---|---|
| `ControlLightAPI.h`, `ControlLightAPI.cpp` | hand written: loading and unloading the DLL |
| `ControlLightAPI_generated_members.inc` | generated: method declarations and function pointers, included in the class declaration |
| `ControlLightAPI_generated_methods.inc` | generated: method definitions, resolving and clearing the function pointers |
| `generate_qt_wrapper.py` | generator |

## After changing ControlAPI.h

Nothing needs to be written by hand. The generated files are recreated from `ControlAPI.h`:

- automatically when ControlLight is built as DLL (`BUILD_MODE=DLL`) and Python 3 is installed (CMake option `CONTROL_LIGHT_GENERATE_QT_WRAPPER`, on by default), or
- manually with `python generate_qt_wrapper.py`.

Commit the regenerated `.inc` files, so that the Qt examples can be built without Python.

## Using it in a Qt project

```
CONTROL_LIGHT_QT_WRAPPER = $$PWD/../ControlLight_Qt_wrapper
INCLUDEPATH += $$CONTROL_LIGHT_QT_WRAPPER
HEADERS += $$CONTROL_LIGHT_QT_WRAPPER/ControlLightAPI.h
SOURCES += $$CONTROL_LIGHT_QT_WRAPPER/ControlLightAPI.cpp
```

```cpp
CControlLightAPI CA;
if (!CA.LoadDLL()) { ... }   // ControlLight.dll next to the executable or on the PATH
CA.Create();
CA.LoadFromJSONFile("ControlHardwareConfig.json");
CA.Initialize();
...
CA.Cleanup();                // calls CLA_Cleanup and unloads the DLL
```

`LoadDLL()` fails only if the DLL can't be loaded or lacks one of the essential functions (`CLA_Create`, `CLA_Cleanup`, `CLA_Configure`, `CLA_GetLastError`, `CLA_DidErrorOccur`). With an older DLL that lacks newer functions, `LoadDLL()` succeeds and `MissingFunctions()` lists them; calling such a function reports that via `qDebug()` and returns `false`/`0`.
