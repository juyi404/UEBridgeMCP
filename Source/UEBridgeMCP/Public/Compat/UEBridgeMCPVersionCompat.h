// Copyright uuuuzz 2024-2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/UserDefinedEnum.h"
#include "Misc/EngineVersionComparison.h"

namespace UEBridgeMCPCompat
{
	template <typename JsonKeyType>
	inline FString JsonKeyToString(const JsonKeyType& Key)
	{
#if defined(UE_JSONOBJECT_LEGACY_STRING_KEYS) && !UE_JSONOBJECT_LEGACY_STRING_KEYS
		return FString(Key.ToView());
#else
		return FString(Key);
#endif
	}

	inline void SetJsonField(const TSharedPtr<FJsonObject>& Object, const FString& Key, const TSharedPtr<FJsonValue>& Value)
	{
		if (Object.IsValid())
		{
			Object->SetField(Key, Value);
		}
	}

	template <typename JsonKeyType>
	inline void SetJsonField(const TSharedPtr<FJsonObject>& Object, const JsonKeyType& Key, const TSharedPtr<FJsonValue>& Value)
	{
		SetJsonField(Object, JsonKeyToString(Key), Value);
	}

	inline bool SetUserDefinedEnumEntries(UUserDefinedEnum* UserEnum, TArray<TPair<FName, int64>>& EnumNames)
	{
		if (!UserEnum)
		{
			return false;
		}

#if UE_VERSION_NEWER_THAN(5, 7, 99)
		return UserEnum->SetEnums(
			EnumNames,
			UEnum::ECppForm::Namespaced,
			UEnum::EUnderlyingType::uint8,
			EEnumFlags::None,
			UEnum::EAddMaxKeyIfMissing::Yes);
#else
		return UserEnum->SetEnums(EnumNames, UEnum::ECppForm::Namespaced);
#endif
	}
}

namespace UEBridgeMCPJson
{
	template <typename JsonKeyType>
	inline FString KeyToString(const JsonKeyType& Key)
	{
		return UEBridgeMCPCompat::JsonKeyToString(Key);
	}

	template <typename JsonKeyType>
	inline void SetField(const TSharedPtr<FJsonObject>& Object, const JsonKeyType& Key, const TSharedPtr<FJsonValue>& Value)
	{
		UEBridgeMCPCompat::SetJsonField(Object, Key, Value);
	}
}
