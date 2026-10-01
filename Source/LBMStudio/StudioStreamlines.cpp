#include "StudioStreamlines.h"
#include "StudioModel.h"
#include "StudioVolume.h"

namespace
{
bool StreamRange(double V,double Low,double High){return FMath::IsFinite(V)&&V>=Low&&V<=High;}
bool StreamBounds(const FBox& B,int32 Dimensions)
{
    return B.IsValid&&!B.Min.ContainsNaN()&&!B.Max.ContainsNaN()&&B.Min.GetAbsMax()<=1.e8&&B.Max.GetAbsMax()<=1.e8&&
        B.Min.X<B.Max.X&&B.Min.Z<B.Max.Z&&(Dimensions==2?B.Min.Y<=B.Max.Y:B.Min.Y<B.Max.Y);
}
void SeedGrid(int32 Count,const FVector& Center,const FVector& U,const FVector& V,TArray<FVector>& Out)
{
    const double Aspect=FMath::Clamp(U.Size()/V.Size(),1.e-3,1.e3);
    const int32 Rows=FMath::Clamp(FMath::RoundToInt(FMath::Sqrt(Count/Aspect)),1,Count);
    for(int32 J=0;J<Rows;++J)
    {
        const int32 Columns=Count/Rows+(J<Count%Rows?1:0);
        for(int32 I=0;I<Columns;++I)Out.Add(Center+U*((I+.5)/Columns-.5)+V*((J+.5)/Rows-.5));
    }
}
}

FBox StudioStreamlines::DomainBounds(const IStudioField& Field,const FBox& RecordingBounds)
{
    if(const auto Grid=Field.VolumeReconstruction())
        return FBox(FVector(Grid->SourceBounds.Min.X,Grid->SourceBounds.Min.Z,Grid->SourceBounds.Min.Y),
            FVector(Grid->SourceBounds.Max.X,Grid->SourceBounds.Max.Z,Grid->SourceBounds.Max.Y));
    return RecordingBounds;
}

bool FStudioStreamlineSettings::operator==(const FStudioStreamlineSettings& S) const
{
    return bDirectionMarkers==S.bDirectionMarkers&&bAutomaticSeeds==S.bAutomaticSeeds&&AutomaticSeedCount==S.AutomaticSeedCount&&Direction==S.Direction&&StepFraction==S.StepFraction&&MaximumLength==S.MaximumLength&&
        WidthFraction==S.WidthFraction&&MaximumSteps==S.MaximumSteps&&WorkBudget==S.WorkBudget&&VelocityField==S.VelocityField;
}
bool StudioStreamlines::IsValid(const FStudioStreamlineSettings& S)
{
    return S.AutomaticSeedCount>=1&&S.AutomaticSeedCount<=512&&uint8(S.Direction)<=uint8(EStudioStreamDirection::Both)&&StreamRange(S.StepFraction,1.e-5,.1)&&
        StreamRange(S.MaximumLength,.001,100)&&StreamRange(S.WidthFraction,1.e-6,.02)&&
        S.MaximumSteps>=1&&S.MaximumSteps<=4096&&S.WorkBudget>=1&&S.WorkBudget<=MaximumWork&&S.VelocityField==TEXT("velocity");
}
TSharedRef<FJsonObject> StudioStreamlines::ToJSON(const FStudioStreamlineSettings& S)
{
    auto O=MakeShared<FJsonObject>();O->SetNumberField(TEXT("direction"),uint8(S.Direction));
    O->SetBoolField(TEXT("directionMarkers"),S.bDirectionMarkers);
    O->SetBoolField(TEXT("automaticSeeds"),S.bAutomaticSeeds);O->SetNumberField(TEXT("automaticSeedCount"),S.AutomaticSeedCount);
    O->SetNumberField(TEXT("stepFraction"),S.StepFraction);O->SetNumberField(TEXT("maximumLength"),S.MaximumLength);
    O->SetNumberField(TEXT("widthFraction"),S.WidthFraction);O->SetNumberField(TEXT("maximumSteps"),S.MaximumSteps);
    O->SetNumberField(TEXT("workBudget"),S.WorkBudget);O->SetStringField(TEXT("velocityField"),S.VelocityField);return O;
}
bool StudioStreamlines::FromJSON(const TSharedPtr<FJsonObject>& O,FStudioStreamlineSettings& Out)
{
    FStudioStreamlineSettings S;double Direction,Steps,Budget,Count;
    if(!O||!O->TryGetNumberField(TEXT("direction"),Direction)||!O->TryGetNumberField(TEXT("stepFraction"),S.StepFraction)||
        !O->TryGetNumberField(TEXT("maximumLength"),S.MaximumLength)||!O->TryGetNumberField(TEXT("widthFraction"),S.WidthFraction)||
        !O->TryGetNumberField(TEXT("maximumSteps"),Steps)||!O->TryGetNumberField(TEXT("workBudget"),Budget)||
        !O->TryGetStringField(TEXT("velocityField"),S.VelocityField)||!O->TryGetBoolField(TEXT("automaticSeeds"),S.bAutomaticSeeds)||
        !O->TryGetNumberField(TEXT("automaticSeedCount"),Count)||!StreamRange(Count,1,512)||Count!=FMath::FloorToDouble(Count)||
        !StreamRange(Direction,0,2)||!StreamRange(Steps,1,4096)||!StreamRange(Budget,1,MaximumWork)||
        Direction!=FMath::FloorToDouble(Direction)||Steps!=FMath::FloorToDouble(Steps)||Budget!=FMath::FloorToDouble(Budget))return false;
    // Additive display preference: older documents retain unmarked streamlines.
    if(O->HasField(TEXT("directionMarkers"))&&(!O->HasTypedField<EJson::Boolean>(TEXT("directionMarkers"))||
        !O->TryGetBoolField(TEXT("directionMarkers"),S.bDirectionMarkers)))return false;
    S.Direction=EStudioStreamDirection(int32(Direction));S.MaximumSteps=int32(Steps);S.WorkBudget=int32(Budget);S.AutomaticSeedCount=int32(Count);
    if(!IsValid(S))return false;Out=MoveTemp(S);return true;
}
bool StudioStreamlines::Seeds(const FStudioSeedObject& S,const FBox& Bounds,int32 Dimensions,
    double PlaneY,TArray<FVector>& Out,FString& Error)
{
    FStudioInspectionObjects Objects;Objects.Seeds.Add(S);
    if(!StudioInspectionObjects::IsValid(Objects,Error))return false;
    Error=TEXT("Invalid seed domain.");
    if((Dimensions!=2&&Dimensions!=3)||!StreamBounds(Bounds,Dimensions)||!FMath::IsFinite(PlaneY))return false;
    TArray<FVector> Positions;Positions.Reserve(S.Kind==EStudioSeedKind::Points?S.Points.Num():S.Count);
    if(S.Kind==EStudioSeedKind::Points)Positions=S.Points;
    else if(S.Kind==EStudioSeedKind::Line)
        for(int32 I=0;I<S.Count;++I)Positions.Add(FMath::Lerp(S.A,S.B,S.Count==1?.5:double(I)/(S.Count-1)));
    else if(S.Kind==EStudioSeedKind::Plane)SeedGrid(S.Count,S.A,S.B,S.C,Positions);
    else
    {
        if(Dimensions==2&&S.InletAxis==1){Error=TEXT("A 2D inlet must cross the source X/Z plane.");return false;}
        const FVector Size=Bounds.GetSize();FVector Center=Bounds.GetCenter();
        Center[S.InletAxis]=FMath::Lerp(Bounds.Min[S.InletAxis],Bounds.Max[S.InletAxis],S.bUpperFace?.9977:.0023);
        if(Dimensions==2)
        {
            Center.Y=PlaneY;const int32 Axis=S.InletAxis==0?2:0;
            for(int32 I=0;I<S.Count;++I)
            {FVector P=Center;P[Axis]=Bounds.Min[Axis]+Size[Axis]*(.05+.9*(I+.5)/S.Count);Positions.Add(P);}
        }
        else
        {
            FVector U=FVector::ZeroVector,V=FVector::ZeroVector;
            const int32 A=(S.InletAxis+1)%3,B=(S.InletAxis+2)%3;U[A]=Size[A]*.9;V[B]=Size[B]*.9;
            SeedGrid(S.Count,Center,U,V,Positions);
        }
    }
    for(const auto& P:Positions)if(P.ContainsNaN()||P.GetAbsMax()>1.e8){Error=TEXT("Seed position exceeds supported coordinates.");return false;}
    Error.Empty();Out=MoveTemp(Positions);return true;
}

bool StudioStreamlines::Build(const IStudioField& Field,const FBox& Bounds,const TArray<FStudioSeedObject>& SeedObjects,
    const FStudioStreamlineSettings& S,const FString& Scalar,FStudioStreamlineOutput& Out,FString& Error,
    const FStudioLoadCancellation& Cancellation)
{
    const auto Cancelled=[&]{return Cancellation&&Cancellation->load(std::memory_order_relaxed);};
    FStudioInspectionObjects Objects;Objects.Seeds=SeedObjects;
    if(!StudioInspectionObjects::IsValid(Objects,Error))return false;
    Error=TEXT("Invalid streamline settings, frame or domain.");
    const auto Identity=Field.Identity();
    if(!IsValid(S)||!Field.IsValid()||!Identity.IsSet()||(Identity->SpatialDimensions!=2&&Identity->SpatialDimensions!=3)||
        !StreamBounds(Bounds,Identity->SpatialDimensions))return false;
    if(Cancelled()){Error=TEXT("Streamline tracing cancelled.");return false;}
    FStudioStreamlineOutput Result;Result.Identity=Identity;Result.Scalar=Scalar;
    const double Scale=Bounds.GetSize().GetMax(),Step=S.StepFraction*Scale,LengthLimit=S.MaximumLength*Scale;
    Result.WidthMeters=S.WidthFraction*Scale;
    const FStudioInspectionSource Source{Identity->Dataset,Identity->MetadataSHA256,Identity->PayloadSHA256};
    TBitArray<> Active;
    const auto At=[&](const FVector& P,FVector& Direction,double* Value,EStudioStreamEnd& End)
    {
        if(!Bounds.IsInsideOrOn(P)||(Identity->SpatialDimensions==2&&P.Y!=Identity->SourceOffset.Y))
        {End=EStudioStreamEnd::OutsideDomain;return false;}
        if(Field.IsSolid(P)){End=EStudioStreamEnd::CoverageUnavailable;return false;}
        FVector Velocity;
        if(!Field.SampleVelocity(P,Velocity)||Velocity.ContainsNaN()||!FMath::IsFinite(Velocity.Size()))
        {End=EStudioStreamEnd::MissingVelocity;return false;}
        const double Speed=Velocity.Size();
        if(Speed<=0){End=EStudioStreamEnd::Stagnation;return false;}
        Direction=Velocity/Speed;
        if(Value&&(!Field.SampleScalar(P,Scalar,*Value)||!FMath::IsFinite(*Value)))
        {End=EStudioStreamEnd::MissingScalar;return false;}
        return true;
    };
    for(const auto& Seed:SeedObjects)
    {
        if(Cancelled()){Error=TEXT("Streamline tracing cancelled.");return false;}
        if(!Seed.bVisible)continue;
        if(!(Seed.Source==Source)){Result.Notices.Add(Seed.Id,TEXT("Seed set belongs to a different recording."));continue;}
        if(Identity->Interpolation==EStudioFieldInterpolation::None)
        {Result.Notices.Add(Seed.Id,TEXT("Streamlines require verified interpolation topology and recorded velocity components."));continue;}
        TArray<FVector> Positions;FString Notice;
        if(!Seeds(Seed,Bounds,Identity->SpatialDimensions,Identity->SourceOffset.Y,Positions,Notice))
        {Result.Notices.Add(Seed.Id,Notice);continue;}
        Result.SeedCount+=Positions.Num();
        for(int32 I=0;I<Positions.Num();++I)
        {
            FVector Direction;double Value=0;EStudioStreamEnd End=EStudioStreamEnd::WorkLimit;
            const bool bValid=At(Positions[I],Direction,&Value,End);
            for(int32 Branch=0;Branch<(S.Direction==EStudioStreamDirection::Both?2:1);++Branch)
            {
                FStudioStreamlinePath Path;Path.SeedId=Seed.Id;Path.SeedIndex=I;
                Path.bBackward=S.Direction==EStudioStreamDirection::Backward||(S.Direction==EStudioStreamDirection::Both&&Branch==1);
                Path.End=End;
                if(bValid){Path.PositionsMeters.Add(Positions[I]);Path.Scalars.Add(Value);}
                Result.Paths.Add(MoveTemp(Path));Active.Add(bValid);
            }
        }
    }
    bool bMore=true;
    while(bMore&&Result.Attempts<S.WorkBudget)
    {
        bMore=false;
        for(int32 I=0;I<Result.Paths.Num()&&Result.Attempts<S.WorkBudget;++I)
        {
            if(Cancelled()){Error=TEXT("Streamline tracing cancelled.");return false;}
            if(!Active[I])continue;
            auto& Path=Result.Paths[I];auto Stop=[&](EStudioStreamEnd End){Path.End=End;Active[I]=false;};
            if(Path.PositionsMeters.Num()-1>=S.MaximumSteps){Stop(EStudioStreamEnd::StepLimit);continue;}
            const double Remaining=LengthLimit-Path.LengthMeters;
            if(Remaining<=LengthLimit*1.e-12){Stop(EStudioStreamEnd::LengthLimit);continue;}
            ++Result.Attempts;
            const FVector P=Path.PositionsMeters.Last();FVector D1,D2,D3;EStudioStreamEnd End;
            if(!At(P,D1,nullptr,End)){Stop(End);continue;}
            const double Sign=Path.bBackward?-1.:1.,Distance=FMath::Min(Step,Remaining);
            const FVector Mid=P+D1*(Sign*Distance*.5);
            if(!At(Mid,D2,nullptr,End)){Stop(End);continue;}
            const FVector Q=P+D2*(Sign*Distance);double Value;
            if(!At(Q,D3,&Value,End)){Stop(End);continue;}
            if(!Field.SupportsSegment(P,Mid,Cancellation)||!Field.SupportsSegment(P,Q,Cancellation))
            {Stop(EStudioStreamEnd::CoverageUnavailable);continue;}
            const double Length=(Q-P).Size();
            if(!FMath::IsFinite(Length)||Length<=0){Stop(EStudioStreamEnd::Stagnation);continue;}
            Path.PositionsMeters.Add(Q);Path.Scalars.Add(Value);Path.LengthMeters+=Length;++Result.Segments;bMore=true;
            if(Path.LengthMeters>=LengthLimit*(1.-1.e-12))Stop(EStudioStreamEnd::LengthLimit);
            else if(Path.PositionsMeters.Num()-1>=S.MaximumSteps)Stop(EStudioStreamEnd::StepLimit);
        }
    }
    if(Cancelled()){Error=TEXT("Streamline tracing cancelled.");return false;}
    for(int32 I=0;I<Active.Num();++I)if(Active[I]){Result.bBudgetExhausted=true;Result.Paths[I].End=EStudioStreamEnd::WorkLimit;}
    Error.Empty();Out=MoveTemp(Result);return true;
}
