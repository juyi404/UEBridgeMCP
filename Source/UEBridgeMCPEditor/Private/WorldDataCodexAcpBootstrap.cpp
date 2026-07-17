#include "WorldDataCodexAcpBootstrap.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

namespace
{
	static const TCHAR* const GCodexAcpPackage = TEXT("@zed-industries/codex-acp@0.16.0");

	static bool IsExistingFile(const FString& Path)
	{
		return !Path.IsEmpty() && FPaths::FileExists(Path);
	}

	static FString NormalizePath(FString Path)
	{
		Path.TrimStartAndEndInline();
		if (!Path.IsEmpty())
		{
			Path = FPaths::ConvertRelativePathToFull(Path);
			FPaths::CollapseRelativeDirectories(Path);
			FPaths::MakePlatformFilename(Path);
		}
		return Path;
	}

	static FString GetManagedInstallRoot()
	{
		return NormalizePath(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("Adapters"), TEXT("codex-acp")));
	}

	static FString FindAdapterUnderNodeModules(const FString& NodeModulesRoot)
	{
		if (NodeModulesRoot.IsEmpty())
		{
			return FString();
		}

		// Test the exact optional platform packages published by the fixed ACP
		// package. This is not an arbitrary directory scan and never becomes a
		// runtime launch path until it is SHA-256 pinned.
		static const TCHAR* const PlatformPackages[] = {
			TEXT("codex-acp-win32-x64"),
			TEXT("codex-acp-win32-arm64")
		};
		for (const TCHAR* PlatformPackage : PlatformPackages)
		{
			const FString Candidate = NormalizePath(FPaths::Combine(
				NodeModulesRoot,
				TEXT("@zed-industries"),
				TEXT("codex-acp"),
				TEXT("node_modules"),
				TEXT("@zed-industries"),
				PlatformPackage,
				TEXT("bin"),
				TEXT("codex-acp.exe")));
			if (IsExistingFile(Candidate))
			{
				return Candidate;
			}
		}
		return FString();
	}

	static FString FindNpmExecutable()
	{
#if PLATFORM_WINDOWS
		TArray<FString> Candidates;
		const FString ProgramFiles = FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramFiles"));
		const FString ProgramW6432 = FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramW6432"));
		if (!ProgramFiles.IsEmpty())
		{
			Candidates.Add(FPaths::Combine(ProgramFiles, TEXT("nodejs"), TEXT("npm.cmd")));
		}
		if (!ProgramW6432.IsEmpty())
		{
			Candidates.Add(FPaths::Combine(ProgramW6432, TEXT("nodejs"), TEXT("npm.cmd")));
		}
		for (const FString& Candidate : Candidates)
		{
			const FString Normalized = NormalizePath(Candidate);
			if (IsExistingFile(Normalized))
			{
				return Normalized;
			}
		}

		// npm is used only as the installer following an explicit user click. The
		// downloaded adapter itself is still required to be a native .exe and is
		// verified by a local SHA-256 pin before any launch.
		int32 ReturnCode = 1;
		FString StandardOutput;
		FString StandardError;
		FPlatformProcess::ExecProcess(TEXT("where.exe"), TEXT("npm.cmd"), &ReturnCode, &StandardOutput, &StandardError);
		if (ReturnCode == 0)
		{
			TArray<FString> Lines;
			StandardOutput.ParseIntoArrayLines(Lines, true);
			for (const FString& Line : Lines)
			{
				const FString Candidate = NormalizePath(Line);
				if (FPaths::GetExtension(Candidate).Equals(TEXT("cmd"), ESearchCase::IgnoreCase) && IsExistingFile(Candidate))
				{
					return Candidate;
				}
			}
		}
#endif
		return FString();
	}

	static FString GetNpmGlobalNodeModulesRoot(const FString& NpmExecutable)
	{
		if (NpmExecutable.IsEmpty())
		{
			return FString();
		}

		int32 ReturnCode = 1;
		FString StandardOutput;
		FString StandardError;
		FPlatformProcess::ExecProcess(*NpmExecutable, TEXT("root --global"), &ReturnCode, &StandardOutput, &StandardError);
		if (ReturnCode != 0)
		{
			return FString();
		}

		TArray<FString> Lines;
		StandardOutput.ParseIntoArrayLines(Lines, true);
		for (const FString& Line : Lines)
		{
			const FString Candidate = NormalizePath(Line);
			if (!Candidate.IsEmpty() && !FPaths::IsRelative(Candidate) && IFileManager::Get().DirectoryExists(*Candidate))
			{
				return Candidate;
			}
		}
		return FString();
	}

	static FString GetShortOutput(FString Text)
	{
		Text.TrimStartAndEndInline();
		return Text.Len() > 1000 ? Text.Left(1000) + TEXT("…") : Text;
	}
}

FWorldDataCodexAcpBootstrapResult FWorldDataCodexAcpBootstrap::FindOrInstall()
{
	FWorldDataCodexAcpBootstrapResult Result;
#if !PLATFORM_WINDOWS
	Result.Message = TEXT("Automatic Codex ACP setup currently supports Windows only.");
	return Result;
#else
	const FString ManagedInstallRoot = GetManagedInstallRoot();
	if (const FString ManagedAdapter = FindAdapterUnderNodeModules(FPaths::Combine(ManagedInstallRoot, TEXT("node_modules"))); !ManagedAdapter.IsEmpty())
	{
		Result.bSuccess = true;
		Result.ExecutablePath = ManagedAdapter;
		Result.Message = TEXT("Found the project-managed Codex ACP adapter.");
		return Result;
	}

	// Earlier project bootstrap flows placed the native adapter directly under
	// Saved/UEBridgeMCP.  Treat it as an explicit setup candidate only; the
	// caller still has to verify and SHA-256 pin it before it can be launched.
	const FString LegacyManagedAdapter = NormalizePath(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("codex-acp.exe")));
	if (IsExistingFile(LegacyManagedAdapter))
	{
		Result.bSuccess = true;
		Result.ExecutablePath = LegacyManagedAdapter;
		Result.Message = TEXT("Found the project-managed Codex ACP adapter.");
		return Result;
	}

	const FString NpmExecutable = FindNpmExecutable();
	if (NpmExecutable.IsEmpty())
	{
		Result.Message = TEXT("Node.js npm was not found. Install Node.js LTS, then run this setup action again.");
		return Result;
	}

	if (const FString GlobalAdapter = FindAdapterUnderNodeModules(GetNpmGlobalNodeModulesRoot(NpmExecutable)); !GlobalAdapter.IsEmpty())
	{
		Result.bSuccess = true;
		Result.ExecutablePath = GlobalAdapter;
		Result.Message = TEXT("Found the installed Codex ACP adapter.");
		return Result;
	}

	if (!IFileManager::Get().MakeDirectory(*ManagedInstallRoot, true))
	{
		Result.Message = FString::Printf(TEXT("Could not create the managed adapter directory: %s"), *ManagedInstallRoot);
		return Result;
	}

	int32 ReturnCode = 1;
	FString StandardOutput;
	FString StandardError;
	const FString Arguments = FString::Printf(
		TEXT("install --prefix \"%s\" --no-save --ignore-scripts --no-audit --no-fund %s"),
		*ManagedInstallRoot,
		GCodexAcpPackage);
	FPlatformProcess::ExecProcess(*NpmExecutable, *Arguments, &ReturnCode, &StandardOutput, &StandardError);
	if (ReturnCode != 0)
	{
		const FString Detail = GetShortOutput(StandardError.IsEmpty() ? StandardOutput : StandardError);
		Result.Message = Detail.IsEmpty()
			? TEXT("npm could not install the Codex ACP adapter.")
			: FString::Printf(TEXT("npm could not install the Codex ACP adapter: %s"), *Detail);
		return Result;
	}

	const FString InstalledAdapter = FindAdapterUnderNodeModules(FPaths::Combine(ManagedInstallRoot, TEXT("node_modules")));
	if (InstalledAdapter.IsEmpty())
	{
		Result.Message = TEXT("npm completed, but the expected native codex-acp.exe was not installed.");
		return Result;
	}

	Result.bSuccess = true;
	Result.bInstalled = true;
	Result.ExecutablePath = InstalledAdapter;
	Result.Message = TEXT("Downloaded the project-managed Codex ACP adapter.");
	return Result;
#endif
}
