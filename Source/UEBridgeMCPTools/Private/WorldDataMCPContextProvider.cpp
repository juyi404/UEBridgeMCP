#include "WorldDataMCPContextProvider.h"

#include "WorldDataMCPToolRegistry.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/Crc.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	FString MakeRevisionHash(const FString& Value)
	{
		const FTCHARToUTF8 Utf8(*Value);
		// Revisions are optimistic-concurrency tokens, not security primitives.
		// Use the portable CRC implementation instead of a platform hash hook.
		return FString::Printf(TEXT("crc32-%08x-%d"), FCrc::MemCrc32(Utf8.Get(), Utf8.Length()), Utf8.Length());
	}

	FString MakeTargetSummary(const TSharedPtr<FJsonObject>& Arguments)
	{
		if (!Arguments.IsValid())
		{
			return TEXT("target not provided");
		}

		static const TCHAR* const TargetFields[] = {
			TEXT("name"), TEXT("label"), TEXT("assetPath"), TEXT("path"), TEXT("file_path"),
			TEXT("old_path"), TEXT("new_path"), TEXT("child_name"), TEXT("parent_name")
		};
		TArray<FString> Parts;
		for (const TCHAR* Field : TargetFields)
		{
			FString Value;
			if (Arguments->TryGetStringField(Field, Value) && !Value.IsEmpty())
			{
				Parts.Add(FString::Printf(TEXT("%s=%s"), Field, *Value.Left(160)));
			}
		}
		return Parts.IsEmpty() ? TEXT("target not provided") : FString::Join(Parts, TEXT(", "));
	}

	UWorld* GetContextProviderEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	FString CaptureWorldRevision()
	{
		UWorld* World = GetContextProviderEditorWorld();
		if (!World)
		{
			return TEXT("unavailable");
		}

		int32 ActorCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			++ActorCount;
		}
		const FString Snapshot = FString::Printf(
			TEXT("world=%s|packageDirty=%d|actorCount=%d"),
			*World->GetPathName(),
			World->GetOutermost() && World->GetOutermost()->IsDirty() ? 1 : 0,
			ActorCount);
		return MakeRevisionHash(Snapshot);
	}

	FString CaptureTargetRevision(const FString& /*ToolName*/, const TSharedPtr<FJsonObject>& Arguments)
	{
		TArray<FString> Snapshot;
		Snapshot.Add(MakeTargetSummary(Arguments));

		UWorld* World = GetContextProviderEditorWorld();
		if (!World)
		{
			Snapshot.Add(TEXT("world=unavailable"));
			return MakeRevisionHash(FString::Join(Snapshot, TEXT("|")));
		}

		Snapshot.Add(FString::Printf(TEXT("world=%s"), *World->GetPathName()));
		Snapshot.Add(FString::Printf(TEXT("packageDirty=%d"), World->GetOutermost() && World->GetOutermost()->IsDirty() ? 1 : 0));
		int32 ActorCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			++ActorCount;
		}
		Snapshot.Add(FString::Printf(TEXT("actorCount=%d"), ActorCount));

		if (Arguments.IsValid())
		{
			static const TCHAR* const ActorFields[] = { TEXT("name"), TEXT("child_name"), TEXT("parent_name") };
			for (const TCHAR* Field : ActorFields)
			{
				FString Selector;
				if (!Arguments->TryGetStringField(Field, Selector) || Selector.IsEmpty())
				{
					continue;
				}

				bool bFound = false;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (Actor && (Actor->GetName().Equals(Selector, ESearchCase::IgnoreCase)
						|| Actor->GetActorLabel().Equals(Selector, ESearchCase::IgnoreCase)))
					{
						Snapshot.Add(FString::Printf(TEXT("%s=%s:%s:%s"), Field, *Actor->GetPathName(), *Actor->GetClass()->GetPathName(), *Actor->GetActorTransform().ToHumanReadableString()));
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					Snapshot.Add(FString::Printf(TEXT("%s=missing:%s"), Field, *Selector));
				}
			}

			static const TCHAR* const AssetFields[] = { TEXT("assetPath"), TEXT("path"), TEXT("old_path"), TEXT("new_path") };
			for (const TCHAR* Field : AssetFields)
			{
				FString Selector;
				if (!Arguments->TryGetStringField(Field, Selector) || Selector.IsEmpty())
				{
					continue;
				}
				const FSoftObjectPath ObjectPath(Selector);
				if (UObject* Object = ObjectPath.ResolveObject())
				{
					UPackage* Package = Object->GetOutermost();
					Snapshot.Add(FString::Printf(TEXT("%s=%s:%s:packageDirty=%d"), Field, *Object->GetPathName(), *Object->GetClass()->GetPathName(), Package && Package->IsDirty() ? 1 : 0));
				}
				else
				{
					Snapshot.Add(FString::Printf(TEXT("%s=unloaded:%s"), Field, *Selector));
				}
			}
		}

		Snapshot.Sort();
		return MakeRevisionHash(FString::Join(Snapshot, TEXT("|")));
	}
}

void WorldDataMCP::Tools::RegisterContextProvider()
{
	FContextRegistry::Get().RegisterRevisionProvider(CaptureWorldRevision, CaptureTargetRevision);
}
