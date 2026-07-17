#pragma once

#include "WorldDataAgentTypes.h"

class IWorldDataAgentDiagnostics
{
public:
	virtual ~IWorldDataAgentDiagnostics() = default;

	virtual void Record(const FWorldDataAgentDiagnosticEntry& Entry) = 0;
	virtual TArray<FWorldDataAgentDiagnosticEntry> Snapshot() const = 0;
	virtual bool ExportRedacted(FString& OutAbsolutePath, FString& OutError) const = 0;
	virtual void Clear() = 0;
};
