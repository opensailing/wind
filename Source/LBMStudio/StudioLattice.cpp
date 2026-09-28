#include "StudioLattice.h"
#include "StudioColor.h"
#include "StudioMaterials.h"

bool FStudioLatticeLayout::Cell(const FIntVector& Index,FVector& Center,FBox& Box) const
{
    if(!Cells||!Bounds.IsValid||Spacing.ContainsNaN()||Spacing.GetMin()<=0)return false;
    for(int32 Axis=0;Axis<3;++Axis)if(Index[Axis]<0||Index[Axis]>=Resolution[Axis])return false;
    const FVector Low=Bounds.Min+FVector(Index)*Spacing;FVector High=Bounds.Min+(FVector(Index)+FVector::OneVector)*Spacing;
    for(int32 Axis=0;Axis<3;++Axis)if(Index[Axis]==Resolution[Axis]-1)High[Axis]=Bounds.Max[Axis];
    Center=(Low+High)*.5;Box=FBox(Low,High);return true;
}
bool StudioLattice::Layout(const FStudioDomain& Domain,const FIntVector& Resolution,FStudioLatticeLayout& Out,FString& Error)
{
    FStudioLatticeLayout Result;Result.Bounds=FBox(Domain.Min,Domain.Max);Result.Resolution=Resolution;Result.Cells=1;
    for(int32 Axis=0;Axis<3;++Axis)
    {
        if(!FMath::IsFinite(Domain.Min[Axis])||!FMath::IsFinite(Domain.Max[Axis])||Domain.Min[Axis]>=Domain.Max[Axis]||
            FMath::Abs(Domain.Min[Axis])>1.e8||FMath::Abs(Domain.Max[Axis])>1.e8)
        {Error=TEXT("Apply a finite domain with positive X, Y and Z dimensions before preparing the lattice.");return false;}
        if(Resolution[Axis]<1||Resolution[Axis]>MaximumResolution)
        {Error=FString::Printf(TEXT("%c resolution must be an integer from 1 to 1,048,576 cells."),TEXT("XYZ")[Axis]);return false;}
        Result.Spacing[Axis]=(Domain.Max[Axis]-Domain.Min[Axis])/Resolution[Axis];
        if(Domain.Min[Axis]+Result.Spacing[Axis]==Domain.Min[Axis]||Domain.Max[Axis]-Result.Spacing[Axis]==Domain.Max[Axis])
        {Error=TEXT("Cell spacing is below coordinate precision at this domain location. Move the geometry closer to the origin or reduce the resolution.");return false;}
        Result.Cells*=uint64(Resolution[Axis]); // At most 2^60, bounded by the per-axis limit.
    }
    Out=Result;Error.Empty();return true;
}
void FStudioLatticeEdit::Reset(const FIntVector& Resolution)
{
    Saved=Resolution;for(int32 Axis=0;Axis<3;++Axis)Counts[Axis]=FString::FromInt(Resolution[Axis]);RequestedSpacing.Empty();Error.Empty();ErrorField=INDEX_NONE;
}
bool FStudioLatticeEdit::IsDirty() const
{for(int32 Axis=0;Axis<3;++Axis)if(Counts[Axis]!=FString::FromInt(Saved[Axis]))return true;return false;}
bool FStudioLatticeEdit::Build(FIntVector& Out)
{
    Error.Empty();ErrorField=INDEX_NONE;FIntVector Value;
    for(int32 Axis=0;Axis<3;++Axis)
    {
        double Count=0;
        if(!StudioColor::ParseNumber(Counts[Axis],Count)||Count<1||Count>StudioLattice::MaximumResolution||Count!=FMath::FloorToDouble(Count))
        {ErrorField=Axis;Error=FString::Printf(TEXT("%c resolution: enter a whole number from 1 to 1,048,576."),TEXT("XYZ")[Axis]);return false;}
        Value[Axis]=int32(Count);
    }
    Out=Value;return true;
}
bool FStudioLatticeEdit::UseSpacing(const FStudioDomain& Domain)
{
    Error.Empty();ErrorField=3;double Spacing=0;
    if(!StudioColor::ParseNumber(RequestedSpacing,Spacing)||Spacing<=0||Spacing>2.e8)
    {Error=TEXT("Maximum cell spacing: enter a positive finite distance in meters, at most 2e8 m.");return false;}
    FIntVector Value;
    for(int32 Axis=0;Axis<3;++Axis)
    {
        const double Count=FMath::CeilToDouble((Domain.Max[Axis]-Domain.Min[Axis])/Spacing);
        if(!FMath::IsFinite(Count)||Count<1||Count>StudioLattice::MaximumResolution)
        {Error=TEXT("This spacing cannot cover the domain within the per-axis resolution limit. Increase the spacing or adjust the domain.");return false;}
        Value[Axis]=int32(Count);
    }
    FStudioLatticeLayout Check;if(!StudioLattice::Layout(Domain,Value,Check,Error))return false;
    for(int32 Axis=0;Axis<3;++Axis)Counts[Axis]=FString::FromInt(Value[Axis]);ErrorField=INDEX_NONE;return true;
}
bool StudioLattice::SamplePlan(const FStudioLatticeLayout& Layout,const FStudioLatticePreviewSettings& Settings,FStudioLatticeSamplePlan& Out,FString& Error)
{
    FStudioDomain Domain;Domain.Min=Layout.Bounds.Min;Domain.Max=Layout.Bounds.Max;FStudioLatticeLayout Checked;
    if(!Layout.Bounds.IsValid||!StudioLattice::Layout(Domain,Layout.Resolution,Checked,Error)||
        Checked.Cells!=Layout.Cells||Checked.Spacing!=Layout.Spacing)
    {Error=TEXT("Build a valid physical lattice layout before requesting preview cells.");return false;}
    if(!Layout.Cells||Settings.Axis<-1||Settings.Axis>2||Settings.MaximumSamples<1||Settings.MaximumSamples>MaximumPreviewSamples||
        (Settings.Axis>=0&&(Settings.Layer<0||Settings.Layer>=Layout.Resolution[Settings.Axis])))
    {Error=TEXT("Choose a valid lattice layer and a preview budget from 1 to 32,768 cells.");return false;}
    FStudioLatticeSamplePlan Plan;Plan.Layout=Layout;Plan.Settings=Settings;
    Plan.CandidateCells=Settings.Axis<0?Layout.Cells:Layout.Cells/uint64(Layout.Resolution[Settings.Axis]);
    Plan.Samples=int32(FMath::Min<uint64>(Plan.CandidateCells,uint64(Settings.MaximumSamples)));
    Plan.bSampled=uint64(Plan.Samples)<Plan.CandidateCells;Out=Plan;Error.Empty();return true;
}
FIntVector FStudioLatticeSamplePlan::Index(int32 Sample) const
{
    if(Sample<0||Sample>=Samples||!CandidateCells)return FIntVector(INDEX_NONE);
    uint64 Linear=uint64(Sample);
    if(bSampled)
    {
        if(Samples==1)Linear=CandidateCells/2;
        else
        {
            // Quotient/remainder prevents overflow even for a 2^60-cell domain.
            const uint64 Span=CandidateCells-1,Denominator=uint64(Samples-1),I=uint64(Sample);
            Linear=(Span/Denominator)*I+((Span%Denominator)*I)/Denominator;
        }
    }
    FIntVector Result=FIntVector::ZeroValue;
    if(Settings.Axis<0)
    {
        for(int32 Axis=0;Axis<3;++Axis){Result[Axis]=int32(Linear%uint64(Layout.Resolution[Axis]));Linear/=uint64(Layout.Resolution[Axis]);}
    }
    else
    {
        const int32 A=(Settings.Axis+1)%3,B=(Settings.Axis+2)%3;Result[Settings.Axis]=Settings.Layer;
        Result[A]=int32(Linear%uint64(Layout.Resolution[A]));Result[B]=int32(Linear/uint64(Layout.Resolution[A]));
    }
    return Result;
}
FStudioLatticeValidation StudioLattice::Validate(const FStudioCaseDraft& Case,const FStudioLatticeCapabilities* Backend)
{
    FStudioLatticeValidation Result;FStudioLatticeLayout Grid;FString Error;
    if(!Layout(Case.Domain,Case.Setup.LatticeResolution,Grid,Error)){Result.Issues.Add(Error);return Result;}
    Result.bBackendKnown=Backend&&Backend->bLayoutRulesSupplied&&!Backend->BackendId.IsEmpty()&&Backend->BackendId==Case.Setup.BackendId;
    if(!Result.bBackendKnown)return Result;
    if(Backend->MaximumCells.IsSet()&&Grid.Cells>Backend->MaximumCells.GetValue())Result.Issues.Add(TEXT("The requested lattice exceeds the selected backend's declared cell limit."));
    if(!FMath::IsFinite(Backend->SpacingRelativeTolerance)||Backend->SpacingRelativeTolerance<0||Backend->SpacingRelativeTolerance>.01)
        Result.Issues.Add(TEXT("The backend supplied an invalid spacing tolerance."));
    else if(Backend->bUniformSpacingRequired&&Grid.Spacing.GetMax()-Grid.Spacing.GetMin()>Grid.Spacing.GetMax()*Backend->SpacingRelativeTolerance)
        Result.Issues.Add(TEXT("The selected backend requires equal X, Y and Z cell spacing. Adjust the resolution or domain."));
    if(Backend->BytesPerCell.IsSet()&&Backend->FixedBytes.IsSet())
    {
        const uint64 PerCell=Backend->BytesPerCell.GetValue(),Fixed=Backend->FixedBytes.GetValue();
        if(!PerCell||Grid.Cells>(MAX_uint64-Fixed)/PerCell)Result.Issues.Add(TEXT("The backend memory estimate is invalid or exceeds the reportable range."));
        else Result.EstimatedBytes=Grid.Cells*PerCell+Fixed;
    }
    return Result;
}
FString StudioLattice::PreviewKey(const FStudioCaseDraft& Case,const FStudioLatticePreviewSettings& Settings)
{
    FString Key=TEXT("authoring-cells-v1|")+Case.Domain.Id.ToString();
    for(int32 Axis=0;Axis<3;++Axis)Key+=TEXT("|")+StudioMaterials::ExactNumber(Case.Domain.Min[Axis])+TEXT("|")+
        StudioMaterials::ExactNumber(Case.Domain.Max[Axis])+TEXT("|")+FString::FromInt(Case.Setup.LatticeResolution[Axis]);
    return Key+FString::Printf(TEXT("|%d|%d|%d|"),Settings.Axis,Settings.Layer,Settings.MaximumSamples)+StudioDomain::GeometryKey(Case);
}
