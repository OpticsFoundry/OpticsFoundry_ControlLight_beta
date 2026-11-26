// Qt wrapper for ControlLight.dll, see ControlLightAPI.h

#include "ControlLightAPI.h"

#include <QDebug>

CControlLightAPI::CControlLightAPI()
{
}

CControlLightAPI::~CControlLightAPI()
{
    Cleanup();
}

void CControlLightAPI::MessageBox(QString aMessage) {
    qDebug() << aMessage;
}

bool CControlLightAPI::LoadDLL(const QString& dll_file_name) {
    if (DLL_Loaded) return true;
    CLA_Lib = new QLibrary(dll_file_name);
    if (!CLA_Lib->load()) {
        qDebug() << "Failed to load" << dll_file_name << ":" << CLA_Lib->errorString();
        delete CLA_Lib;
        CLA_Lib = nullptr;
        return false;
    }
    QStringList missing_required_functions;
    Missing_Functions.clear();
    ResolveFunctions(Missing_Functions, missing_required_functions);
    if (!missing_required_functions.isEmpty()) {
        qDebug() << dll_file_name << "doesn't provide the required functions" << missing_required_functions;
        ClearFunctions();
        CLA_Lib->unload();
        delete CLA_Lib;
        CLA_Lib = nullptr;
        return false;
    }
    if (!Missing_Functions.isEmpty()) {
        qDebug() << "Warning:" << dll_file_name << "is older than this wrapper and doesn't provide" << Missing_Functions;
    }
    DLL_Loaded = true;
    Configure(true);
    return true;
}

void CControlLightAPI::Cleanup() {
    if (CLA_Lib) {
        if (DLL_Loaded) Call_CLA_Cleanup();  // before clearing the function pointers
        ClearFunctions();
        CLA_Lib->unload();
        delete CLA_Lib;
        CLA_Lib = nullptr;
    }
    DLL_Loaded = false;
}

void CControlLightAPI::ReportMissingFunction(const char* name) {
    if (!DLL_Loaded) qDebug() << "ControlLight.dll not loaded, can't call" << name << "- call LoadDLL() first";
    else qDebug() << "The loaded ControlLight.dll doesn't provide" << name;
}

// generated wrapper methods, ResolveFunctions() and ClearFunctions()
#include "ControlLightAPI_generated_methods.inc"
