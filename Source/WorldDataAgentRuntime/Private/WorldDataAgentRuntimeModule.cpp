#include "IWorldDataAgentRuntimeModule.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr int32 RuntimeManifestVersion = 1;

	FString NormalizePath(FString Path)
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

	FString ReadExecutableVersion(const FString& Executable, const FString& Arguments);

	uint64 SemanticVersionScore(FString VersionOutput)
	{
		VersionOutput.TrimStartAndEndInline();
		int32 Separator = INDEX_NONE;
		if (VersionOutput.FindLastChar(TEXT(' '), Separator))
		{
			VersionOutput.RightChopInline(Separator + 1);
		}
		TArray<FString> Parts;
		VersionOutput.ParseIntoArray(Parts, TEXT("."), true);
		if (Parts.Num() < 2) return 0;
		const uint64 Major = static_cast<uint64>(FMath::Max(0, FCString::Atoi(*Parts[0])));
		const uint64 Minor = static_cast<uint64>(FMath::Max(0, FCString::Atoi(*Parts[1])));
		const uint64 Patch = Parts.Num() > 2 ? static_cast<uint64>(FMath::Max(0, FCString::Atoi(*Parts[2]))) : 0;
		return Major * 1000000000000ULL + Minor * 1000000ULL + Patch;
	}

	FString FindBundledAgentHost()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEBridgeMCP"));
		if (Plugin.IsValid())
		{
			const FString Bundled = NormalizePath(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries"), TEXT("Win64"), TEXT("AgentHost"), TEXT("worlddata-agent-host.exe")));
			if (FPaths::FileExists(Bundled)) return Bundled;
		}

		// Developer fallback. It is never selected after a packaged single-file
		// host has been staged under the plugin's Binaries directory.
		const FString Development = NormalizePath(FPaths::Combine(
			FPaths::ProjectDir(),
			TEXT("Programs"), TEXT("WorldDataAgentHost"), TEXT("WorldData.AgentHost.App"),
			TEXT("bin"), TEXT("Release"), TEXT("net8.0"), TEXT("worlddata-agent-host.exe")));
		return FPaths::FileExists(Development) ? Development : FString();
	}

	FString FindInstalledCodex()
	{
		const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		const FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		const FString ProgramFiles = FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramFiles"));
		TArray<FString> ExactCandidates;
		if (!LocalAppData.IsEmpty())
		{
			ExactCandidates.Add(FPaths::Combine(LocalAppData, TEXT("Programs"), TEXT("OpenAI"), TEXT("Codex"), TEXT("bin"), TEXT("codex.exe")));
			ExactCandidates.Add(FPaths::Combine(LocalAppData, TEXT("OpenAI"), TEXT("Codex"), TEXT("bin"), TEXT("codex.exe")));
		}
		if (!ProgramFiles.IsEmpty())
		{
			ExactCandidates.Add(FPaths::Combine(ProgramFiles, TEXT("OpenAI"), TEXT("Codex"), TEXT("bin"), TEXT("codex.exe")));
		}
		if (!UserProfile.IsEmpty())
		{
			ExactCandidates.Add(FPaths::Combine(UserProfile, TEXT(".codex"), TEXT("plugins"), TEXT(".plugin-appserver"), TEXT("codex.exe")));
			ExactCandidates.Add(FPaths::Combine(UserProfile, TEXT(".codex"), TEXT(".sandbox-bin"), TEXT("codex.exe")));
		}
		if (!UserProfile.IsEmpty())
		{
			const FString ReleasesRoot = NormalizePath(FPaths::Combine(UserProfile, TEXT(".codex"), TEXT("packages"), TEXT("standalone"), TEXT("releases")));
			TArray<FString> ReleaseCandidates;
			IFileManager::Get().FindFilesRecursive(ReleaseCandidates, *ReleasesRoot, TEXT("codex.exe"), true, false, false);
			ReleaseCandidates.RemoveAll([](const FString& Candidate)
			{
				return !FPaths::GetPath(Candidate).EndsWith(TEXT("bin"), ESearchCase::IgnoreCase);
			});
			ExactCandidates.Append(ReleaseCandidates);
		}

		FString BestCandidate;
		uint64 BestVersion = 0;
		FDateTime BestTimestamp = FDateTime::MinValue();
		TSet<FString> Seen;
		for (const FString& Candidate : ExactCandidates)
		{
			const FString Normalized = NormalizePath(Candidate);
			if (Normalized.IsEmpty() || Seen.Contains(Normalized) || !FPaths::FileExists(Normalized)) continue;
			Seen.Add(Normalized);
			const uint64 Version = SemanticVersionScore(ReadExecutableVersion(Normalized, TEXT("--version")));
			const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*Normalized);
			if (BestCandidate.IsEmpty() || Version > BestVersion || (Version == BestVersion && Timestamp > BestTimestamp))
			{
				BestCandidate = Normalized;
				BestVersion = Version;
				BestTimestamp = Timestamp;
			}
		}
		return BestCandidate;
	}

	FString ReadExecutableVersion(const FString& Executable, const FString& Arguments)
	{
		int32 ReturnCode = 1;
		FString StandardOutput;
		FString StandardError;
		FPlatformProcess::ExecProcess(*Executable, *Arguments, &ReturnCode, &StandardOutput, &StandardError);
		StandardOutput.TrimStartAndEndInline();
		return ReturnCode == 0 ? StandardOutput.Left(256) : FString();
	}

	void Record(IWorldDataAgentDiagnostics& Diagnostics, const EWorldDataAgentLogLevel Level, const FString& Code, const FString& Message)
	{
		FWorldDataAgentDiagnosticEntry Entry;
		Entry.TimestampUtc = FDateTime::UtcNow();
		Entry.Level = Level;
		Entry.Component = TEXT("WorldDataAgentRuntime");
		Entry.Code = Code;
		Entry.Message = Message;
		Diagnostics.Record(Entry);
	}

	class FWorldDataAgentRuntime final : public IWorldDataAgentRuntime
	{
	public:
		FWorldDataAgentRuntime(IWorldDataAgentSecurity& InSecurity, IWorldDataAgentDiagnostics& InDiagnostics)
			: Security(InSecurity), Diagnostics(InDiagnostics)
		{
			Status.ManifestPath = GetManifestPath();
		}

		virtual bool ConfigureLocalRuntime(FString& OutError) override
		{
			OutError.Empty();
			const FString SourceHost = FindBundledAgentHost();
			if (SourceHost.IsEmpty())
			{
				return SetError(TEXT("runtime.agent_host_missing"), TEXT("The bundled WorldData Agent Host executable was not found. Publish the Agent Host before configuring the runtime."), OutError);
			}
			const FString SourceCodex = FindInstalledCodex();
			if (SourceCodex.IsEmpty())
			{
				return SetError(TEXT("runtime.codex_missing"), TEXT("No supported native Codex installation was found."), OutError);
			}

			const FString RuntimeRoot = NormalizePath(Security.GetManagedRuntimeRoot());
			FString HostHash;
			FString CodexHash;
			FString HashError;
			if (!Security.ComputeFileSha256(SourceHost, HostHash, HashError)
				|| !Security.ComputeFileSha256(SourceCodex, CodexHash, HashError))
			{
				return SetError(TEXT("runtime.source_hash_failed"), HashError, OutError);
			}

			// Executables are installed into immutable, content-addressed version
			// directories. A runtime repair can therefore complete while the previous
			// Host or Codex binary is still running; only the manifest is switched.
			const FString HostDirectory = FPaths::Combine(RuntimeRoot, TEXT("agent-host"), HostHash);
			const FString CodexDirectory = FPaths::Combine(RuntimeRoot, TEXT("codex"), CodexHash);
			const FString SchemaDirectory = FPaths::Combine(RuntimeRoot, TEXT("codex-schema"), CodexHash);
			if (!IFileManager::Get().MakeDirectory(*HostDirectory, true)
				|| !IFileManager::Get().MakeDirectory(*CodexDirectory, true)
				|| !IFileManager::Get().MakeDirectory(*SchemaDirectory, true))
			{
				return SetError(TEXT("runtime.directory_failed"), TEXT("Could not create the managed runtime directories."), OutError);
			}

			const FString ManagedHost = NormalizePath(FPaths::Combine(HostDirectory, TEXT("worlddata-agent-host.exe")));
			const FString ManagedCodex = NormalizePath(FPaths::Combine(CodexDirectory, TEXT("codex.exe")));
			FString VerifyError;
			if ((!FPaths::FileExists(ManagedHost) || !Security.VerifyPinnedFile(ManagedHost, HostHash, VerifyError))
				&& IFileManager::Get().Copy(*ManagedHost, *SourceHost, true, true) != COPY_OK)
			{
				return SetError(TEXT("runtime.host_copy_failed"), TEXT("Could not copy Agent Host into the managed runtime."), OutError);
			}
			VerifyError.Empty();
			if ((!FPaths::FileExists(ManagedCodex) || !Security.VerifyPinnedFile(ManagedCodex, CodexHash, VerifyError))
				&& IFileManager::Get().Copy(*ManagedCodex, *SourceCodex, true, true) != COPY_OK)
			{
				return SetError(TEXT("runtime.codex_copy_failed"), TEXT("Could not copy Codex into the managed runtime."), OutError);
			}

			int32 SchemaReturnCode = 1;
			FString SchemaOutput;
			FString SchemaError;
			const FString SchemaArguments = FString::Printf(TEXT("app-server generate-json-schema --out \"%s\""), *SchemaDirectory);
			FPlatformProcess::ExecProcess(*ManagedCodex, *SchemaArguments, &SchemaReturnCode, &SchemaOutput, &SchemaError);
			const FString CodexSchemaPath = NormalizePath(FPaths::Combine(SchemaDirectory, TEXT("codex_app_server_protocol.schemas.json")));
			if (SchemaReturnCode != 0 || !FPaths::FileExists(CodexSchemaPath))
			{
				return SetError(TEXT("runtime.schema_generation_failed"), TEXT("Codex app-server schema generation failed."), OutError);
			}

			FString SchemaHash;
			if (!Security.VerifyPinnedFile(ManagedHost, HostHash, HashError)
				|| !Security.VerifyPinnedFile(ManagedCodex, CodexHash, HashError)
				|| !Security.ComputeFileSha256(CodexSchemaPath, SchemaHash, HashError))
			{
				return SetError(TEXT("runtime.hash_failed"), HashError, OutError);
			}

			const FString HostVersion = ReadExecutableVersion(ManagedHost, TEXT("--version"));
			const FString CodexVersion = ReadExecutableVersion(ManagedCodex, TEXT("--version"));
			TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
			Manifest->SetNumberField(TEXT("manifestVersion"), RuntimeManifestVersion);
			Manifest->SetNumberField(TEXT("protocolVersion"), WorldDataAgentProtocol::CurrentVersion);
			Manifest->SetStringField(TEXT("agentHostExecutable"), ManagedHost);
			Manifest->SetStringField(TEXT("agentHostVersion"), HostVersion);
			Manifest->SetStringField(TEXT("agentHostSha256"), HostHash);
			Manifest->SetStringField(TEXT("codexExecutable"), ManagedCodex);
			Manifest->SetStringField(TEXT("codexVersion"), CodexVersion);
			Manifest->SetStringField(TEXT("codexSha256"), CodexHash);
			Manifest->SetStringField(TEXT("codexSchemaPath"), CodexSchemaPath);
			Manifest->SetStringField(TEXT("codexSchemaSha256"), SchemaHash);

			FString Json;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
			if (!FJsonSerializer::Serialize(Manifest, Writer))
			{
				return SetError(TEXT("runtime.manifest_serialize_failed"), TEXT("Could not serialize the runtime manifest."), OutError);
			}
			const FString ManifestPath = GetManifestPath();
			const FString TemporaryPath = ManifestPath + TEXT(".tmp");
			if (!FFileHelper::SaveStringToFile(Json, *TemporaryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
				|| !IFileManager::Get().Move(*ManifestPath, *TemporaryPath, true, true))
			{
				return SetError(TEXT("runtime.manifest_write_failed"), TEXT("Could not atomically write the runtime manifest."), OutError);
			}

			Record(Diagnostics, EWorldDataAgentLogLevel::Info, TEXT("runtime.configured"), FString::Printf(TEXT("Configured managed runtime: Host %s; Codex %s."), *HostVersion, *CodexVersion));
			return LoadAndVerify(OutError);
		}

		virtual bool LoadAndVerify(FString& OutError) override
		{
			OutError.Empty();
			FString Json;
			const FString ManifestPath = GetManifestPath();
			if (!FFileHelper::LoadFileToString(Json, *ManifestPath))
			{
				return SetError(TEXT("runtime.not_configured"), TEXT("The managed runtime has not been configured."), OutError, true);
			}
			TSharedPtr<FJsonObject> Manifest;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			if (!FJsonSerializer::Deserialize(Reader, Manifest) || !Manifest.IsValid())
			{
				return SetError(TEXT("runtime.manifest_invalid"), TEXT("The runtime manifest is not valid JSON."), OutError);
			}
			double ManifestVersion = 0;
			double ProtocolVersion = 0;
			if (!Manifest->TryGetNumberField(TEXT("manifestVersion"), ManifestVersion)
				|| !Manifest->TryGetNumberField(TEXT("protocolVersion"), ProtocolVersion)
				|| static_cast<int32>(ManifestVersion) != RuntimeManifestVersion
				|| static_cast<int32>(ProtocolVersion) != WorldDataAgentProtocol::CurrentVersion)
			{
				return SetError(TEXT("runtime.manifest_version_mismatch"), TEXT("The runtime manifest version is not supported."), OutError);
			}

			FWorldDataAgentRuntimeStatus Next;
			Next.ManifestPath = ManifestPath;
			Next.bConfigured = true;
			Manifest->TryGetStringField(TEXT("agentHostExecutable"), Next.AgentHostExecutable);
			Manifest->TryGetStringField(TEXT("agentHostVersion"), Next.AgentHostVersion);
			Manifest->TryGetStringField(TEXT("agentHostSha256"), Next.AgentHostSha256);
			Manifest->TryGetStringField(TEXT("codexExecutable"), Next.CodexExecutable);
			Manifest->TryGetStringField(TEXT("codexVersion"), Next.CodexVersion);
			Manifest->TryGetStringField(TEXT("codexSha256"), Next.CodexSha256);
			Manifest->TryGetStringField(TEXT("codexSchemaPath"), Next.CodexSchemaPath);
			Manifest->TryGetStringField(TEXT("codexSchemaSha256"), Next.CodexSchemaSha256);

			FString VerifyError;
			if (!Security.VerifyPinnedFile(Next.AgentHostExecutable, Next.AgentHostSha256, VerifyError)
				|| !Security.VerifyPinnedFile(Next.CodexExecutable, Next.CodexSha256, VerifyError)
				|| !Security.VerifyPinnedFile(Next.CodexSchemaPath, Next.CodexSchemaSha256, VerifyError))
			{
				return SetError(TEXT("runtime.pin_mismatch"), VerifyError, OutError);
			}
			Next.bVerified = true;
			{
				FScopeLock Lock(&StatusMutex);
				Status = MoveTemp(Next);
			}
			return true;
		}

		virtual FWorldDataAgentRuntimeStatus GetStatus() const override
		{
			FScopeLock Lock(&StatusMutex);
			return Status;
		}

	private:
		FString GetManifestPath() const
		{
			return NormalizePath(FPaths::Combine(Security.GetManagedRuntimeRoot(), TEXT("runtime-manifest.json")));
		}

		bool SetError(const FString& Code, const FString& Message, FString& OutError, const bool bRetryable = true)
		{
			OutError = Message;
			FWorldDataAgentRuntimeStatus Next;
			Next.ManifestPath = GetManifestPath();
			Next.Error = { Code, Message, TEXT("WorldDataAgentRuntime"), bRetryable };
			{
				FScopeLock Lock(&StatusMutex);
				Status = MoveTemp(Next);
			}
			Record(Diagnostics, EWorldDataAgentLogLevel::Error, Code, Message);
			return false;
		}

		IWorldDataAgentSecurity& Security;
		IWorldDataAgentDiagnostics& Diagnostics;
		mutable FCriticalSection StatusMutex;
		FWorldDataAgentRuntimeStatus Status;
	};
}

class FWorldDataAgentRuntimeModule final : public IWorldDataAgentRuntimeModule
{
public:
	virtual TSharedRef<IWorldDataAgentRuntime> CreateRuntime(
		IWorldDataAgentSecurity& Security,
		IWorldDataAgentDiagnostics& Diagnostics) override
	{
		return MakeShared<FWorldDataAgentRuntime>(Security, Diagnostics);
	}
};

IMPLEMENT_MODULE(FWorldDataAgentRuntimeModule, WorldDataAgentRuntime)
