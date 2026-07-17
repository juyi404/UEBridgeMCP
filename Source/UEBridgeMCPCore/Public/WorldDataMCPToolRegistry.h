#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace WorldDataMCP
{
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
		using FToolHandler = TFunction<FString(const TSharedPtr<FJsonObject>&)>;

		static FToolRegistry& Get();

		void RegisterTool(const FString& Name, FToolHandler Handler);
		void RegisterDefinitionSet(const FString& DefinitionsJson);
		bool Dispatch(const FString& Name, const TSharedPtr<FJsonObject>& Arguments, FString& OutResult) const;
		FString GetRegisteredDefinitionsJson() const;
		void Reset();

	private:
		mutable FRWLock Lock;
		TMap<FString, FToolHandler> Handlers;
		TArray<FString> DefinitionSets;
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
		FString CaptureWorldRevision() const;
		FString CaptureTargetRevision(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments) const;

	private:
		mutable FRWLock Lock;
		FWorldRevisionHandler WorldRevision;
		FTargetRevisionHandler TargetRevision;
	};
}
