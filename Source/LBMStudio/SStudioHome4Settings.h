#pragma once
#include "Widgets/SCompoundWidget.h"
class FStudioModel;
class SStudioHome4Settings final:public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Settings){} SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model) SLATE_ARGUMENT(FString,DefaultsPath) SLATE_END_ARGS()
    void Construct(const FArguments&);
private:TSharedPtr<FStudioModel> Model;FString DefaultsPath;
};
