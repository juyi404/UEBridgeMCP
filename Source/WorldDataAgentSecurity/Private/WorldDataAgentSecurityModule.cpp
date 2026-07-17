#include "IWorldDataAgentSecurityModule.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	class FWorldDataAgentSecurity final : public IWorldDataAgentSecurity
	{
	public:
		virtual bool CreateEphemeralSecret(const FString& Purpose, FString& OutHandle, FString& OutError) override
		{
			const FString Value = FGuid::NewGuid().ToString(EGuidFormats::Digits)
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
			return StoreEphemeralSecret(Purpose, Value, OutHandle, OutError);
		}

		virtual bool StoreEphemeralSecret(const FString& Purpose, const FString& Secret, FString& OutHandle, FString& OutError) override
		{
			OutError.Empty();
			OutHandle.Empty();
			if (Purpose.TrimStartAndEnd().IsEmpty() || Secret.IsEmpty())
			{
				OutError = TEXT("A non-empty secret purpose and value are required.");
				return false;
			}

			const FString Handle = FString::Printf(TEXT("secret_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			{
				FScopeLock Lock(&SecretsMutex);
				Secrets.Add(Handle, Secret);
			}
			OutHandle = Handle;
			return true;
		}

		virtual bool ResolveSecretForTrustedChild(const FString& Handle, FString& OutSecret, FString& OutError) const override
		{
			OutError.Empty();
			OutSecret.Empty();
			FScopeLock Lock(&SecretsMutex);
			const FString* Value = Secrets.Find(Handle);
			if (!Value)
			{
				OutError = TEXT("The ephemeral secret handle is missing or has been revoked.");
				return false;
			}
			OutSecret = *Value;
			return true;
		}

		virtual void RevokeSecret(const FString& Handle) override
		{
			FScopeLock Lock(&SecretsMutex);
			Secrets.Remove(Handle);
		}

		virtual bool ComputeFileSha256(const FString& AbsolutePath, FString& OutSha256, FString& OutError) const override
		{
			OutSha256.Empty();
			OutError.Empty();
			if (AbsolutePath.IsEmpty() || FPaths::IsRelative(AbsolutePath) || !FPaths::FileExists(AbsolutePath))
			{
				OutError = TEXT("SHA-256 input must be an existing absolute file path.");
				return false;
			}
#if PLATFORM_WINDOWS
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *AbsolutePath) || Bytes.IsEmpty())
			{
				OutError = TEXT("Could not read the file for SHA-256 verification.");
				return false;
			}

			BCRYPT_ALG_HANDLE Algorithm = nullptr;
			BCRYPT_HASH_HANDLE Hash = nullptr;
			DWORD ObjectLength = 0;
			DWORD ResultLength = 0;
			DWORD HashLength = 0;
			if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
				|| BCryptGetProperty(Algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&ObjectLength), sizeof(ObjectLength), &ResultLength, 0) < 0
				|| BCryptGetProperty(Algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&HashLength), sizeof(HashLength), &ResultLength, 0) < 0)
			{
				if (Algorithm) BCryptCloseAlgorithmProvider(Algorithm, 0);
				OutError = TEXT("Windows SHA-256 provider initialization failed.");
				return false;
			}

			TArray<uint8> HashObject;
			TArray<uint8> Digest;
			HashObject.SetNumUninitialized(ObjectLength);
			Digest.SetNumUninitialized(HashLength);
			const bool bSuccess = BCryptCreateHash(Algorithm, &Hash, HashObject.GetData(), HashObject.Num(), nullptr, 0, 0) >= 0
				&& BCryptHashData(Hash, Bytes.GetData(), Bytes.Num(), 0) >= 0
				&& BCryptFinishHash(Hash, Digest.GetData(), Digest.Num(), 0) >= 0;
			if (Hash) BCryptDestroyHash(Hash);
			BCryptCloseAlgorithmProvider(Algorithm, 0);
			if (!bSuccess)
			{
				OutError = TEXT("SHA-256 computation failed.");
				return false;
			}
			OutSha256 = BytesToHex(Digest.GetData(), Digest.Num()).ToLower();
			return true;
#else
			OutError = TEXT("SHA-256 verification is not implemented for this platform.");
			return false;
#endif
		}

		virtual bool VerifyPinnedFile(const FString& AbsolutePath, const FString& ExpectedSha256, FString& OutError) const override
		{
			FString Actual;
			if (!ComputeFileSha256(AbsolutePath, Actual, OutError))
			{
				return false;
			}
			if (ExpectedSha256.Len() != 64 || !Actual.Equals(ExpectedSha256, ESearchCase::IgnoreCase))
			{
				OutError = TEXT("The runtime SHA-256 does not match the pinned manifest.");
				return false;
			}
			return true;
		}

		virtual FString GetManagedRuntimeRoot() const override
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("Runtime")));
		}

	private:
		mutable FCriticalSection SecretsMutex;
		TMap<FString, FString> Secrets;
	};
}

class FWorldDataAgentSecurityModule final : public IWorldDataAgentSecurityModule
{
public:
	virtual IWorldDataAgentSecurity& GetSecurity() override { return Security; }

private:
	FWorldDataAgentSecurity Security;
};

IMPLEMENT_MODULE(FWorldDataAgentSecurityModule, WorldDataAgentSecurity)
