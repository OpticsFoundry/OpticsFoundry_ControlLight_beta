#ifndef CONTROLLIGHTAPI_H
#define CONTROLLIGHTAPI_H

// Qt wrapper for ControlLight.dll, shared by the Qt examples.
//
// ControlAPI.h of ControlLight can't be used with MinGW (it depends on MFC). This class therefore loads
// ControlLight.dll with QLibrary and resolves the API functions by name. Each API function CLA_<name>
// is available as method <name> of this class, with the same parameters (ERROR_CODE_TYPE is bool).
//
// The API specific part is generated from ControlAPI.h by generate_qt_wrapper.py into
// ControlLightAPI_generated_members.inc and ControlLightAPI_generated_methods.inc. Don't edit those files;
// rerun the script after changing ControlAPI.h. Only the DLL handling below is written by hand.
//
// Usage:
//   CControlLightAPI CA;
//   if (!CA.LoadDLL()) ...        // loads ControlLight.dll (next to the executable or on the PATH)
//   CA.Create();                  // then use the API as described in ControlAPI.h
//   ...
//   CA.Cleanup();                 // calls CLA_Cleanup and unloads the DLL (also done by the destructor)

#include <QLibrary>
#include <QString>
#include <QStringList>
#include <cstddef>
#include <cstdint>

class CControlLightAPI
{
public:
    CControlLightAPI();
    virtual ~CControlLightAPI();

    // Loads the DLL and resolves the API functions. Fails if the DLL can't be loaded or lacks an essential function.
    // Functions missing in an older DLL only produce a warning; calling them reports an error and returns false/0.
    bool LoadDLL(const QString& dll_file_name = "ControlLight.dll");
    // Calls CLA_Cleanup and unloads the DLL.
    void Cleanup();
    bool IsDLLLoaded() const { return DLL_Loaded; }
    // API functions that the loaded DLL doesn't provide
    QStringList MissingFunctions() const { return Missing_Functions; }
    void MessageBox(QString aMessage);

#include "ControlLightAPI_generated_members.inc"

private:
    void ReportMissingFunction(const char* name);

    QLibrary* CLA_Lib = nullptr;
    bool DLL_Loaded = false;
    QStringList Missing_Functions;
};

#endif // CONTROLLIGHTAPI_H
