#include "StudioFieldDisplay.h"
#include "StudioModel.h"

TArray<int32> StudioFieldDisplay::VectorRows(int32 Total,int32 Count)
{
    TArray<int32> Rows;
    if(Total<=0||Count<1||Count>MaximumVectorCount)return Rows;
    Count=FMath::Min(Total,Count);Rows.Reserve(Count);
    for(int32 I=0;I<Count;++I)Rows.Add(int32(int64(I)*Total/Count));
    return Rows;
}

TArray<FVector> StudioFieldDisplay::VectorGrid(const FBox& Bounds,int32 Count)
{
    TArray<FVector> Positions;
    const FVector Size=Bounds.GetSize();
    if(!Bounds.IsValid||Size.ContainsNaN()||Size.X<=0||Size.Z<=0||Count<1||Count>MaximumVectorCount)return Positions;
    const double Aspect=FMath::Clamp(Size.X/Size.Z,1.e-3,1.e3);
    const int32 Rows=FMath::Clamp(FMath::RoundToInt(FMath::Sqrt(Count/Aspect)),1,Count);
    Positions.Reserve(Count);
    for(int32 J=0;J<Rows;++J)
    {
        const int32 Columns=Count/Rows+(J<Count%Rows?1:0);
        for(int32 I=0;I<Columns;++I)
            Positions.Add(FVector(Bounds.Min.X+Size.X*(.025+.95*(I+.5)/Columns),
                Bounds.Min.Y+Size.Y*.84375,Bounds.Min.Z+Size.Z*(.05+.9*(J+.5)/Rows)));
    }
    return Positions;
}

bool StudioFieldDisplay::VectorGlyphs(const TArray<FStudioVectorSample>& Samples,double Length,
    bool bUniform,TArray<FStudioVectorGlyph>& Out,FStudioVectorSummary& Summary,
    const FStudioLoadCancellation& Cancellation)
{
    if(Samples.Num()>16384||!FMath::IsFinite(Length)||Length<=0||Length>1.e9)return false;
    FStudioVectorSummary Result;Result.SampleCount=Samples.Num();Result.ReferenceLengthMeters=Length;Result.bUniformLength=bUniform;
    const auto Valid=[](const FStudioVectorSample& S)
    {return !S.PositionMeters.ContainsNaN()&&!S.Velocity.ContainsNaN()&&FMath::IsFinite(S.Velocity.Size());};
    for(const auto& Sample:Samples)
    {
        if(Cancellation&&Cancellation->load())return false;
        if(Valid(Sample))Result.MaximumSpeed=FMath::Max(Result.MaximumSpeed,Sample.Velocity.Size());
    }
    TArray<FStudioVectorGlyph> Glyphs;Glyphs.Reserve(Samples.Num());
    for(const auto& Sample:Samples)
    {
        if(Cancellation&&Cancellation->load())return false;
        if(!Valid(Sample)||!Sample.bHasColor||!FMath::IsFinite(Sample.Color.R)||!FMath::IsFinite(Sample.Color.G)||
            !FMath::IsFinite(Sample.Color.B)||!FMath::IsFinite(Sample.Color.A))continue;
        const double Speed=Sample.Velocity.Size();
        if(Speed<=0||Result.MaximumSpeed<=0)continue;
        const FVector Direction=Sample.Velocity/Speed;
        const double GlyphLength=Length*(bUniform?1.:Speed/Result.MaximumSpeed);
        const FVector Tip=Sample.PositionMeters+Direction*GlyphLength;
        if(Tip.ContainsNaN()||Tip==Sample.PositionMeters)continue;
        Glyphs.Add({Sample.PositionMeters,Tip,Direction,Sample.Color});
    }
    Result.GlyphCount=Glyphs.Num();Out=MoveTemp(Glyphs);Summary=Result;return true;
}

bool StudioFieldDisplay::SampleColor(const IStudioField& Field,const FVector& P,const FString& ScalarId,
    const FStudioColorMapping& Mapping,FLinearColor& Out)
{
    Out=FLinearColor::Transparent;
    double Value;
    if(!Field.IsValid()||!Field.SampleScalar(P,ScalarId,Value)||!FMath::IsFinite(Value))return false;
    Out=StudioColor::Map(Value,Mapping);return true;
}

bool StudioFieldDisplay::VectorGlyphs(const IStudioField& Field,const TArray<FVector>& Positions,const FString& ScalarId,
    const FStudioColorMapping& Mapping,double ReferenceSize,double RelativeScale,TArray<FStudioVectorGlyph>& Out,
    const FStudioLoadCancellation& Cancellation,bool bUniformLength,FStudioVectorSummary* Summary)
{
    if(!Field.IsValid()||Positions.Num()>16384||!FMath::IsFinite(ReferenceSize)||ReferenceSize<=0||
        !FMath::IsFinite(RelativeScale)||RelativeScale<.2||RelativeScale>3)return false;
    TArray<FStudioVectorSample> Samples;Samples.Reserve(Positions.Num());
    for(const auto& P:Positions)
    {
        if(Cancellation&&Cancellation->load())return false;
        FVector Velocity;
        if(P.ContainsNaN()||Field.IsSolid(P)||!Field.SampleVelocity(P,Velocity)||Velocity.ContainsNaN())continue;
        FStudioVectorSample Sample;Sample.PositionMeters=P;Sample.Velocity=Velocity;
        Sample.bHasColor=SampleColor(Field,P,ScalarId,Mapping,Sample.Color);Samples.Add(Sample);
    }
    FStudioVectorSummary Result;
    if(!VectorGlyphs(Samples,ReferenceSize*.035*RelativeScale,bUniformLength,Out,Result,Cancellation))return false;
    Result.SampleCount=Positions.Num();
    if(Summary)*Summary=Result;
    return true;
}
