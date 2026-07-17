#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace WorldDataMCP
{
	enum class EToolRisk : uint8
	{
		ReadOnly,
		WorkspaceChange,
		Destructive,
		ArbitraryCode
	};

	// Mutating tools require a ContextEnvelope generated from a fresh editor
	// read. Keeping this policy next to the tool definition prevents the HTTP
	// router from having to infer behavior from a tool name.
	enum class EToolRevisionPolicy : uint8
	{
		None,
		RequireFreshContext
	};

	UEBRIDGEMCPCORE_API FString GetToolRiskName(EToolRisk Risk);
	UEBRIDGEMCPCORE_API FString GetToolRevisionPolicyName(EToolRevisionPolicy Policy);

	using FToolHandler = TFunction<FString(const TSharedPtr<FJsonObject>&)>;

	// The registry owns the canonical runtime description of a tool. Providers
	// may keep JSON schema text close to their implementation, but tools/list,
	// dispatch, approval, capability checks, audit, and policy snapshots all
	// read this one definition after registration.
	struct UEBRIDGEMCPCORE_API FToolDefinition
	{
		FString Name;
		FString ProviderName;
		FString DefinitionJson;
		FToolHandler Handler;
		EToolRisk Risk = EToolRisk::Destructive;
		TArray<FString> RequiredCapabilities;
		bool bRequiresInteractiveApproval = true;
		bool bAudited = true;
		EToolRevisionPolicy RevisionPolicy = EToolRevisionPolicy::RequireFreshContext;
	};

	// Safe-to-copy view for governance and diagnostics. It deliberately omits
	// the handler so callers cannot invoke a provider without going through the
	// registry dispatch path.
	struct UEBRIDGEMCPCORE_API FToolMetadata
	{
		FString Name;
		FString ProviderName;
		EToolRisk Risk = EToolRisk::Destructive;
		TArray<FString> RequiredCapabilities;
		bool bRequiresInteractiveApproval = true;
		bool bAudited = true;
		EToolRevisionPolicy RevisionPolicy = EToolRevisionPolicy::RequireFreshContext;
	};

	class UEBRIDGEMCPCORE_API IWorldDataMCPToolProvider
	{
	public:
		virtual ~IWorldDataMCPToolProvider() = default;
		virtual FString GetProviderName() const = 0;
		virtual void RegisterTools() = 0;
	};

	class UEBRIDGEMCPCORE_API IWorldDataMCPResourceProvider
	{
	public:
		virtual ~IWorldDataMCPResourceProvider() = default;
		virtual FString GetProviderName() const = 0;
		virtual void RegisterResources() = 0;
	};

	class UEBRIDGEMCPCORE_API FToolRegistry
	{
	public:
		static FToolRegistry& Get();

		// Re-registering a name replaces the previous definition. This makes
		// provider startup idempotent under Live Coding instead of duplicating
		// entries in tools/list.
		bool RegisterTool(FToolDefinition Definition);
		void UnregisterProvider(const FString& ProviderName);
		bool Dispatch(const FString& Name, const TSharedPtr<FJsonObject>& Arguments, FString& OutResult) const;
		bool FindToolMetadata(const FString& Name, FToolMetadata& OutMetadata) const;
		TArray<FToolMetadata> GetRegisteredToolMetadata() const;
		FString GetRegisteredDefinitionsJson() const;
		void Reset();

	private:
		mutable FRWLock Lock;
		TMap<FString, FToolDefinition> Tools;
	};

	class UEBRIDGEMCPCORE_API FResourceRegistry
	{
	public:
		using FResourceHandler = TFunction<FString()>;

		static FResourceRegistry& Get();
		void RegisterResource(const FString& Uri, const FString& Name, const FString& Description, FResourceHandler Handler);
		FString GetResourceListJson(const FString& RecommendedFirstRead) const;
		bool Read(const FString& Uri, FString& OutResult) const;

	private:
		struct FResource
		{
			FString Name;
			FString Description;
			FResourceHandler Handler;
		};

		mutable FRWLock Lock;
		TMap<FString, FResource> Resources;
	};

	// Revision capture is a Tool-side concern: Core only asks for opaque,
	// stable revision strings when it protects a queued change from becoming
	// stale. This keeps Core independent from UWorld/UObject editor APIs.
	class UEBRIDGEMCPCORE_API FContextRegistry
	{
	public:
		using FWorldRevisionHandler = TFunction<FString()>;
		using FTargetRevisionHandler = TFunction<FString(const FString&, const TSharedPtr<FJsonObject>&)>;

		static FContextRegistry& Get();
		void RegisterRevisionProvider(FWorldRevisionHandler InWorldRevision, FTargetRevisionHandler InTargetRevision);
		void ClearRevisionProvider();
		FString CaptureWorldRevision() const;
		FString CaptureTargetRevision(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments) const;

	private:
		mutable FRWLock Lock;
		FWorldRevisionHandler WorldRevision;
		FTargetRevisionHandler TargetRevision;
	};
}
