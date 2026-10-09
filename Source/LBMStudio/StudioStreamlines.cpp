#include "StudioStreamlines.h"
#include "StudioModel.h"
#include "StudioVolume.h"
#include <limits>

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
// Dormand-Prince 5(4), verified against scipy/integrate/_ivp/rk.py (v1.17.0).
// Ordered rational coefficients; no dense output or FSAL cache is used.
constexpr double StreamDPA[7][7]={
    {},{1./5},{3./40,9./40},{44./45,-56./15,32./9},
    {19372./6561,-25360./2187,64448./6561,-212./729},
    {9017./3168,-355./33,46732./5247,49./176,-5103./18656},
    {35./384,0,500./1113,125./192,-2187./6784,11./84}};
constexpr double StreamDPE[7]={-71./57600,0,71./16695,-71./1920,17253./339200,-22./525,1./40};
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
        WidthFraction==S.WidthFraction&&MaximumSteps==S.MaximumSteps&&WorkBudget==S.WorkBudget&&VelocityField==S.VelocityField&&
        Method==S.Method&&AbsoluteToleranceFraction==S.AbsoluteToleranceFraction&&RelativeTolerance==S.RelativeTolerance&&MinimumStepFraction==S.MinimumStepFraction;
}
bool StudioStreamlines::IsValid(const FStudioStreamlineSettings& S)
{
    return S.AutomaticSeedCount>=1&&S.AutomaticSeedCount<=512&&uint8(S.Direction)<=uint8(EStudioStreamDirection::Both)&&StreamRange(S.StepFraction,1.e-5,.1)&&
        StreamRange(S.MaximumLength,.001,100)&&StreamRange(S.WidthFraction,1.e-6,.02)&&
        S.MaximumSteps>=1&&S.MaximumSteps<=4096&&S.WorkBudget>=1&&S.WorkBudget<=MaximumWork&&S.VelocityField==TEXT("velocity")&&
        uint8(S.Method)<=uint8(EStudioStreamMethod::DormandPrince45)&&StreamRange(S.AbsoluteToleranceFraction,1.e-12,1.e-3)&&
        StreamRange(S.RelativeTolerance,1.e-12,1.e-2)&&StreamRange(S.MinimumStepFraction,1.e-12,1.e-3)&&S.MinimumStepFraction<=S.StepFraction;
}
TSharedRef<FJsonObject> StudioStreamlines::ToJSON(const FStudioStreamlineSettings& S)
{
    auto O=MakeShared<FJsonObject>();O->SetNumberField(TEXT("direction"),uint8(S.Direction));
    O->SetBoolField(TEXT("directionMarkers"),S.bDirectionMarkers);
    O->SetBoolField(TEXT("automaticSeeds"),S.bAutomaticSeeds);O->SetNumberField(TEXT("automaticSeedCount"),S.AutomaticSeedCount);
    O->SetNumberField(TEXT("stepFraction"),S.StepFraction);O->SetNumberField(TEXT("maximumLength"),S.MaximumLength);
    O->SetNumberField(TEXT("widthFraction"),S.WidthFraction);O->SetNumberField(TEXT("maximumSteps"),S.MaximumSteps);
    O->SetNumberField(TEXT("workBudget"),S.WorkBudget);O->SetStringField(TEXT("velocityField"),S.VelocityField);
    auto I=MakeShared<FJsonObject>();I->SetStringField(TEXT("method"),S.Method==EStudioStreamMethod::Midpoint?TEXT("midpoint"):TEXT("dormand_prince_45"));
    I->SetNumberField(TEXT("absoluteToleranceFraction"),S.AbsoluteToleranceFraction);I->SetNumberField(TEXT("relativeTolerance"),S.RelativeTolerance);
    I->SetNumberField(TEXT("minimumStepFraction"),S.MinimumStepFraction);O->SetObjectField(TEXT("integration"),I);return O;
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
    S.Method=EStudioStreamMethod::Midpoint;
    if(O->HasField(TEXT("integration")))
    {
        const TSharedPtr<FJsonObject>* I=nullptr;FString Method;
        if(!O->TryGetObjectField(TEXT("integration"),I)||!I||!I->IsValid()||!(*I)->HasTypedField<EJson::Number>(TEXT("absoluteToleranceFraction"))||
            !(*I)->HasTypedField<EJson::Number>(TEXT("relativeTolerance"))||!(*I)->HasTypedField<EJson::Number>(TEXT("minimumStepFraction"))||!(*I)->TryGetStringField(TEXT("method"),Method)||
            !(*I)->TryGetNumberField(TEXT("absoluteToleranceFraction"),S.AbsoluteToleranceFraction)||
            !(*I)->TryGetNumberField(TEXT("relativeTolerance"),S.RelativeTolerance)||
            !(*I)->TryGetNumberField(TEXT("minimumStepFraction"),S.MinimumStepFraction))return false;
        if(Method==TEXT("dormand_prince_45"))S.Method=EStudioStreamMethod::DormandPrince45;
        else if(Method!=TEXT("midpoint"))return false;
    }
    if(!IsValid(S))return false;Out=MoveTemp(S);return true;
}
bool StudioStreamlines::AutomaticSeeds(const IStudioField& Field,const FBox& Bounds,int32 Count,
    EStudioStreamDirection Direction,TArray<FVector>& Out,FString& Error,const FStudioLoadCancellation& Cancellation)
{
    const auto Identity=Field.Identity();
    Error=TEXT("Automatic seeds require a supported domain and recorded velocity interpolation.");
    if(!Field.IsValid()||!Identity||Identity->Interpolation==EStudioFieldInterpolation::None||
        (Identity->SpatialDimensions!=2&&Identity->SpatialDimensions!=3)||!StreamBounds(Bounds,Identity->SpatialDimensions)||
        Count<1||Count>512||uint8(Direction)>uint8(EStudioStreamDirection::Both))return false;
    const bool Planar=Identity->SpatialDimensions==2;
    if(Planar&&(!FMath::IsFinite(Identity->SourceOffset.Y)||Identity->SourceOffset.Y<Bounds.Min.Y||Identity->SourceOffset.Y>Bounds.Max.Y))return false;
    const auto Cancelled=[&]{return Cancellation&&Cancellation->load(std::memory_order_relaxed);};
    if(const auto Grid=Field.VolumeReconstruction();Grid&&Grid->OriginalGrid)
    {
        if(Planar||Grid->Dimensions.GetMin()<2){Error=TEXT("Original liquid seeding requires a three-dimensional source grid.");return false;}
        const FIntVector Cells=Grid->Dimensions-FIntVector(1);
        const FIntVector Strata(FMath::Min(16,Cells.X),FMath::Min(16,Cells.Y),FMath::Min(16,Cells.Z));
        TArray<FVector> Supported;Supported.Reserve(Strata.X*Strata.Y*Strata.Z);
        const FVector Spacing=Grid->SourceBounds.GetSize()/FVector(Cells);
        for(int32 Z=0;Z<Strata.Z;++Z)for(int32 Y=0;Y<Strata.Y;++Y)for(int32 X=0;X<Strata.X;++X)
        {
            if(Cancelled()){Error=TEXT("Automatic liquid seeding cancelled.");return false;}
            const FIntVector Cell(int32((int64(2*X+1)*Cells.X)/(2*Strata.X)),
                int32((int64(2*Y+1)*Cells.Y)/(2*Strata.Y)),int32((int64(2*Z+1)*Cells.Z)/(2*Strata.Z)));
            const FVector SourceP=Grid->SourceBounds.Min+(FVector(Cell)+FVector(.5))*Spacing;
            const FVector P=FVector(SourceP.X,SourceP.Z,SourceP.Y)+Identity->SourceOffset;
            if(!Bounds.IsInsideOrOn(P)||Field.IsSolid(P))continue;
            if(Cancelled()){Error=TEXT("Automatic liquid seeding cancelled.");return false;}
            FVector Velocity;if(!Field.SampleVelocity(P,Velocity)||Velocity.ContainsNaN()||!FMath::IsFinite(Velocity.Size())||Velocity.Size()<=0)continue;
            if(Cancelled()){Error=TEXT("Automatic liquid seeding cancelled.");return false;}
            if(Field.SupportsSegment(P,P,Cancellation))Supported.Add(P);
        }
        if(Cancelled()){Error=TEXT("Automatic liquid seeding cancelled.");return false;}
        TArray<FVector> Positions;const int32 Available=FMath::Min(Count,Supported.Num());Positions.Reserve(Available);
        for(int32 I=0;I<Available;++I)Positions.Add(Supported[int32((int64(2*I+1)*Supported.Num())/(2*Available))]);
        Error=Available<Count?FString::Printf(TEXT("%d of %d requested liquid seeds have supported original cells in the bounded scan."),Available,Count):FString();
        Out=MoveTemp(Positions);return true;
    }
    const double Sign=Direction==EStudioStreamDirection::Backward?-1.:1.;
    const FVector Size=Bounds.GetSize();
    struct FCell { FVector Center,U,V,Inward;double Start,Weight; };
    TArray<FCell> Cells;double Total=0;
    // At most 4,096 planar or 6,144 volume samples, plus the requested seeds.
    const int32 N=Planar?FMath::Clamp(Count*2,64,1024):FMath::Clamp(FMath::CeilToInt(FMath::Sqrt(double(Count)*4)),8,32);
    auto WeightAt=[&](const FVector& P,const FVector& Inward)
    {
        FVector Velocity;
        if(!Bounds.IsInsideOrOn(P)||Field.IsSolid(P)||!Field.SampleVelocity(P,Velocity)||Velocity.ContainsNaN())return 0.;
        const double Speed=Velocity.Size();
        if(!FMath::IsFinite(Speed)||Speed<=0)return 0.;
        const double Weight=Sign*FVector::DotProduct(Velocity/Speed,Inward);
        return Weight>1.e-8?Weight:0.;
    };
    for(int32 Axis=0;Axis<3;++Axis)if(!Planar||Axis!=1)for(int32 Side=0;Side<2;++Side)
    {
        const int32 A=Planar?(Axis==0?2:0):(Axis+1)%3,B=(Axis+2)%3;
        FVector Center=Bounds.GetCenter(),U=FVector::ZeroVector,V=FVector::ZeroVector,Inward=FVector::ZeroVector;
        Center[Axis]=FMath::Lerp(Bounds.Min[Axis],Bounds.Max[Axis],Side?.9977:.0023);Inward[Axis]=Side?-1.:1.;
        if(Planar)Center.Y=Identity->SourceOffset.Y;
        U[A]=Size[A]/N;if(!Planar)V[B]=Size[B]/N;
        const double Measure=Planar?U.Size():U.Size()*V.Size();
        for(int32 J=0;J<(Planar?1:N);++J)for(int32 I=0;I<N;++I)
        {
            if(Cancelled()){Error=TEXT("Automatic seeding cancelled.");return false;}
            const FVector P=Center+U*(I+.5-N*.5)+(Planar?FVector::ZeroVector:V*(J+.5-N*.5));
            const double Weight=WeightAt(P,Inward)*Measure;
            if(Weight>0){Cells.Add({P,U,V,Inward,Total,Weight});Total+=Weight;}
        }
    }
    TArray<FVector> Positions;Positions.Reserve(Count);TSet<FVector> Seen;int32 Cell=0;
    if(Total>0)for(int32 I=0;I<Count;++I)
    {
        if(Cancelled()){Error=TEXT("Automatic seeding cancelled.");return false;}
        const double Target=Total*(I+.5)/Count;
        while(Cell+1<Cells.Num()&&Target>=Cells[Cell].Start+Cells[Cell].Weight)++Cell;
        const auto& C=Cells[Cell];
        const double T=FMath::Clamp((Target-C.Start)/C.Weight,0.,1.);
        // Recheck the final point: a valid cell midpoint cannot authorize a
        // seed across a missing-data edge or a local reversal within the cell.
        const FVector P=C.Center+C.U*(T-.5);
        if(WeightAt(P,C.Inward)>0&&!Seen.Contains(P)){Positions.Add(P);Seen.Add(P);}
    }
    if(Cancelled()){Error=TEXT("Automatic seeding cancelled.");return false;}
    Error.Empty();Out=MoveTemp(Positions);return true;
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
    FStudioStreamlineOutput Result;Result.Identity=Identity;Result.Scalar=Scalar;Result.Method=S.Method;
    const double Scale=Bounds.GetSize().GetMax(),Step=S.StepFraction*Scale,LengthLimit=S.MaximumLength*Scale;
    Result.WidthMeters=S.WidthFraction*Scale;
    const FStudioInspectionSource Source{Identity->Dataset,Identity->MetadataSHA256,Identity->PayloadSHA256};
    TBitArray<> Active;
    const auto Position=[&](const FVector& P,EStudioStreamEnd& End)
    {
        if(Cancelled())return false;
        if(!Bounds.IsInsideOrOn(P)||(Identity->SpatialDimensions==2&&P.Y!=Identity->SourceOffset.Y))
        {End=EStudioStreamEnd::OutsideDomain;return false;}
        if(Field.IsSolid(P)){End=EStudioStreamEnd::CoverageUnavailable;return false;}
        return true;
    };
    const auto ScalarAt=[&](const FVector& P,double& Value,EStudioStreamEnd& End)
    {
        if(Cancelled())return false;++Result.ScalarEvaluations;
        if(!Field.SampleScalar(P,Scalar,Value)||!FMath::IsFinite(Value))
        {End=EStudioStreamEnd::MissingScalar;return false;}return !Cancelled();
    };
    const auto Support=[&](const FVector& P,const FVector& Q)
    {if(Cancelled())return false;++Result.SupportEvaluations;return Field.SupportsSegment(P,Q,Cancellation)&&!Cancelled();};
    const auto At=[&](const FVector& P,FVector& Direction,double* Value,EStudioStreamEnd& End)
    {
        if(!Position(P,End)||Cancelled())return false;
        ++Result.VelocityEvaluations;
        FVector Velocity;
        if(!Field.SampleVelocity(P,Velocity)||Velocity.ContainsNaN()||!FMath::IsFinite(Velocity.Size()))
        {End=EStudioStreamEnd::MissingVelocity;return false;}
        const double Speed=Velocity.Size();
        if(Speed<=0){End=EStudioStreamEnd::Stagnation;return false;}
        Direction=Velocity/Speed;
        return !Cancelled()&&(!Value||ScalarAt(P,*Value,End));
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
    double AdaptiveMaximum=Step;FVector GridSpacing=FVector::ZeroVector;
    const auto Grid=Field.VolumeReconstruction();
    if(S.Method==EStudioStreamMethod::DormandPrince45&&Grid)
    {
        if(Grid->Dimensions.GetMin()<2){Error=TEXT("Streamline grid dimensions are invalid.");return false;}
        const FVector SourceSpacing=Grid->SourceBounds.GetSize()/FVector(Grid->Dimensions-FIntVector(1));
        GridSpacing=FVector(SourceSpacing.X,SourceSpacing.Z,SourceSpacing.Y);
        if(GridSpacing.ContainsNaN()||GridSpacing.GetMin()<=0){Error=TEXT("Streamline grid spacing is invalid.");return false;}
        AdaptiveMaximum=FMath::Min(Step,.25*GridSpacing.GetMin());
    }
    TArray<double> NextStep;NextStep.Init(AdaptiveMaximum,Result.Paths.Num());
    TBitArray<> PreviouslyRejected(false,Result.Paths.Num());
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
            const bool RK45=S.Method==EStudioStreamMethod::DormandPrince45;
            const double Remaining=LengthLimit-(RK45?Path.IntegrationLengthMeters:Path.LengthMeters);
            if(Remaining<=LengthLimit*1.e-12){Stop(EStudioStreamEnd::LengthLimit);continue;}
            ++Result.Attempts;++Path.Attempts;
            if(RK45)
            {
                const FVector P=Path.PositionsMeters.Last();const double Sign=Path.bBackward?-1.:1.;
                const double Floor=FMath::Max(S.MinimumStepFraction*Scale,32*std::numeric_limits<double>::epsilon()*FMath::Max(Scale,P.GetAbsMax()));
                const double H=FMath::Min(NextStep[I],Remaining);
                const double RepresentableFloor=32*std::numeric_limits<double>::epsilon()*FMath::Max(Scale,P.GetAbsMax());
                const bool TerminalRemainder=Remaining<Floor&&Remaining>=RepresentableFloor&&Remaining<=NextStep[I];
                if(!FMath::IsFinite(H)||H<=0||(H<Floor&&!TerminalRemainder))
                {Stop(EStudioStreamEnd::AccuracyLimit);continue;}
                auto Reject=[&](double Factor)
                {
                    ++Result.RejectedAttempts;++Path.RejectedAttempts;PreviouslyRejected[I]=true;
                    NextStep[I]=H*Factor;
                    if(TerminalRemainder||NextStep[I]<Floor||!FMath::IsFinite(NextStep[I]))Stop(EStudioStreamEnd::AccuracyLimit);
                    else bMore=true;
                };
                FVector K[7],Q=FVector::ZeroVector;bool Failed=false;EStudioStreamEnd End=EStudioStreamEnd::WorkLimit;
                for(int32 Stage=0;Stage<7;++Stage)
                {
                    if(Cancelled()){Error=TEXT("Streamline tracing cancelled.");return false;}
                    FVector Increment=FVector::ZeroVector;
                    for(int32 J=0;J<Stage;++J)Increment+=StreamDPA[Stage][J]*K[J];
                    const FVector AtPosition=P+H*Increment;
                    if(AtPosition.ContainsNaN()){Stop(EStudioStreamEnd::AccuracyLimit);Failed=true;break;}
                    const FVector Displacement=(AtPosition-P).GetAbs();
                    if(Grid&&Grid->OriginalGrid&&
                        (Displacement.X>GridSpacing.X||Displacement.Y>GridSpacing.Y||Displacement.Z>GridSpacing.Z))
                    {Reject(.5);Failed=true;break;}
                    if(!Position(AtPosition,End)){Stop(End);Failed=true;break;}
                    if(!Support(P,AtPosition)){Stop(EStudioStreamEnd::CoverageUnavailable);Failed=true;break;}
                    FVector Direction;if(!At(AtPosition,Direction,nullptr,End)){Stop(End);Failed=true;break;}
                    K[Stage]=Sign*Direction;if(Stage==6)Q=AtPosition;
                }
                if(Failed)continue;
                // Stage seven is the fifth-order endpoint and its full P->Q chord has already passed Support.
                FVector Embedded=FVector::ZeroVector;for(int32 Stage=0;Stage<7;++Stage)Embedded+=StreamDPE[Stage]*K[Stage];Embedded*=H;
                double Norm=0;
                for(int32 Axis=0;Axis<3;++Axis)if(Identity->SpatialDimensions!=2||Axis!=1)
                {
                    const double Tolerance=S.AbsoluteToleranceFraction*Scale+S.RelativeTolerance*
                        FMath::Max(FMath::Abs(P[Axis]-Bounds.Min[Axis]),FMath::Abs(Q[Axis]-Bounds.Min[Axis]));
                    Norm=FMath::Max(Norm,FMath::Abs(Embedded[Axis])/Tolerance);
                }
                if(!FMath::IsFinite(Norm)||Embedded.ContainsNaN()){Stop(EStudioStreamEnd::AccuracyLimit);continue;}
                if(Norm>1){Reject(FMath::Clamp(.9*FMath::Pow(Norm,-.2),.2,.9));continue;}
                const double Length=(Q-P).Size();
                if(!FMath::IsFinite(Length)||Length<=0){Stop(EStudioStreamEnd::Stagnation);continue;}
                if(Path.LengthMeters+Length>LengthLimit+32*std::numeric_limits<double>::epsilon()*FMath::Max(Scale,P.GetAbsMax()))
                {Reject(.5);continue;}
                double Value;if(!ScalarAt(Q,Value,End)){Stop(End);continue;}
                Path.PositionsMeters.Add(Q);Path.Scalars.Add(Value);Path.LengthMeters+=Length;Path.IntegrationLengthMeters+=H;
                ++Result.Segments;bMore=true;
                double Factor=Norm==0?5.:FMath::Clamp(.9*FMath::Pow(Norm,-.2),.2,5.);
                if(PreviouslyRejected[I])Factor=FMath::Min(1.,Factor);PreviouslyRejected[I]=false;
                NextStep[I]=FMath::Min(AdaptiveMaximum,H*Factor);
                if(Path.IntegrationLengthMeters>=LengthLimit*(1.-1.e-12))Stop(EStudioStreamEnd::LengthLimit);
                else if(Path.PositionsMeters.Num()-1>=S.MaximumSteps)Stop(EStudioStreamEnd::StepLimit);
                continue;
            }
            const FVector P=Path.PositionsMeters.Last();FVector D1,D2,D3;EStudioStreamEnd End;
            if(!At(P,D1,nullptr,End)){Stop(End);continue;}
            const double Sign=Path.bBackward?-1.:1.,Distance=FMath::Min(Step,Remaining);
            const FVector Mid=P+D1*(Sign*Distance*.5);
            if(!At(Mid,D2,nullptr,End)){Stop(End);continue;}
            const FVector Q=P+D2*(Sign*Distance);double Value;
            if(!At(Q,D3,&Value,End)){Stop(End);continue;}
            if(!Support(P,Mid)||!Support(P,Q))
            {Stop(EStudioStreamEnd::CoverageUnavailable);continue;}
            const double Length=(Q-P).Size();
            if(!FMath::IsFinite(Length)||Length<=0){Stop(EStudioStreamEnd::Stagnation);continue;}
            Path.PositionsMeters.Add(Q);Path.Scalars.Add(Value);Path.LengthMeters+=Length;Path.IntegrationLengthMeters=Path.LengthMeters;++Result.Segments;bMore=true;
            if(Path.LengthMeters>=LengthLimit*(1.-1.e-12))Stop(EStudioStreamEnd::LengthLimit);
            else if(Path.PositionsMeters.Num()-1>=S.MaximumSteps)Stop(EStudioStreamEnd::StepLimit);
        }
    }
    if(Cancelled()){Error=TEXT("Streamline tracing cancelled.");return false;}
    for(int32 I=0;I<Active.Num();++I)if(Active[I]){Result.bBudgetExhausted=true;Result.Paths[I].End=EStudioStreamEnd::WorkLimit;}
    Error.Empty();Out=MoveTemp(Result);return true;
}
