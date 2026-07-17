#pragma once

#include "CoreMinimal.h"

class IWorldDataAgentSecurity
{
public:
	virtual ~IWorldDataAgentSecurity() = default;

	virtual bool CreateEphemeralSecret(const FString& Purpose, FString& OutHandle, FString& OutError) = 0;
	virtual bool StoreEphemeralSecret(const FString& Purpose, const FString& Secret, FString& OutHandle, FString& OutError) = 0;
	virtual bool ResolveSecretForTrustedChild(const FString& Handle, FString& OutSecret, FString& OutError) const = 0;
	virtual void RevokeSecret(const FString& Handle) = 0;
	virtual bool ComputeFileSha256(const FString& AbsolutePath, FString& OutSha256, FString& OutError) const = 0;
	virtual bool VerifyPinnedFile(const FString& AbsolutePath, const FString& ExpectedSha256, FString& OutError) const = 0;
	virtual FString GetManagedRuntimeRoot() const = 0;
};
