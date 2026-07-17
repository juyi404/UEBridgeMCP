#pragma once

#include "CoreMinimal.h"

// Result of the explicit setup action in the editor settings. The setup action
// can inspect known package roots and install a fixed ACP package, but it never
// makes a discovered file launchable by itself. The caller must pin the returned
// native executable with FWorldDataCodexACPClient::PinProfileExecutable first.
struct FWorldDataCodexAcpBootstrapResult
{
	bool bSuccess = false;
	bool bInstalled = false;
	FString ExecutablePath;
	FString Message;
};

class FWorldDataCodexAcpBootstrap
{
public:
	// This is intended to run from a worker thread. It only invokes npm after an
	// explicit setup click, and installs into the project's ignored Saved folder.
	static FWorldDataCodexAcpBootstrapResult FindOrInstall();
};
