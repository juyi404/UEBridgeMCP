#pragma once

#include "IWorldDataAgentDiagnostics.h"
#include "IWorldDataAgentGateway.h"
#include "IWorldDataAgentSecurity.h"
#include "IWorldDataAgentRuntime.h"

class IWorldDataAgentSubsystem
{
public:
	virtual ~IWorldDataAgentSubsystem() = default;

	virtual void Initialize(const FWorldDataAgentConnectionOptions& Options) = 0;
	virtual bool ConfigureRuntime(FString& OutError) = 0;
	virtual void Shutdown() = 0;
	virtual IWorldDataAgentGateway& GetGateway() = 0;
	virtual IWorldDataAgentSecurity& GetSecurity() = 0;
	virtual IWorldDataAgentDiagnostics& GetDiagnostics() = 0;
	virtual FWorldDataAgentRuntimeStatus GetRuntimeStatus() const = 0;
};
