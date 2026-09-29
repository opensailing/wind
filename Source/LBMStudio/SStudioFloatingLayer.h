#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioFloatingPanes.h"

class FStudioModel;
class SConstraintCanvas;
class SStudioFloatingPane;
/** Bounded, independently movable viewport chrome; never changes scene/case state. */
class SStudioFloatingLayer : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioFloatingLayer){}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void AddPane(FName Id,const FString& Title,TSharedRef<SWidget> Content,FVector2D Anchor,FVector2D Offset,bool Stretch=false);
    FVector2D ViewSize() const;
    void Raise(SStudioFloatingPane* Pane);
    void Save();
    TSharedPtr<FStudioModel> Model;
private:
    TSharedPtr<SConstraintCanvas> Canvas;
    TArray<TSharedPtr<SStudioFloatingPane>> Panes;
    int32 Front=0;
};
