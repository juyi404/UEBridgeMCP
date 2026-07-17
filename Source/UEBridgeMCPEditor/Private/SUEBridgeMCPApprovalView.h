#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SUEBridgeMCPApprovalView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUEBridgeMCPApprovalView) {}
		SLATE_ATTRIBUTE(FText, Summary)
		SLATE_ATTRIBUTE(FText, Details)
		SLATE_ATTRIBUTE(bool, CanApprove)
		SLATE_EVENT(FOnClicked, OnApprove)
		SLATE_EVENT(FOnClicked, OnDeny)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};
