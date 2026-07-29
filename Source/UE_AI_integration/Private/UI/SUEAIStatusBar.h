#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SUEAIStatusBarWidget final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUEAIStatusBarWidget)
	{
	}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<SWidget> MakeMenuContent();
	FText GetStatusCountText() const;
	FText GetStatusToolTip() const;
	const FSlateBrush* GetStatusIcon() const;
	EVisibility GetStatusCountVisibility() const;
};

TSharedRef<SWidget> CreateUEAIStatusBarWidget();
