#include "Modules/ModuleManager.h"

// The core module hosts no startup logic of its own; it only provides the tool
// registry and shared helpers. The default module implementation is sufficient.
IMPLEMENT_MODULE(FDefaultModuleImpl, UEBridgeMCPCore)
