#include "StudioHome4Config.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    constexpr double Home4Cs = 0.57735026918962576451;
    constexpr int64 Home4MaxCount = 1000000000000LL;
    constexpr int32 Home4MaxLevels = 20;
    constexpr int32 Home4MaxText = 4096;
    using Home4FObject = TSharedPtr<FJsonObject>;
    using Home4FValues = TArray<TSharedPtr<FJsonValue>>;
    struct Home4FNumberField { FStudioHome4Field Field; TOptional<double>& (*Access)(FStudioHome4Spec&); };
    struct Home4FIntegerField { FStudioHome4Field Field; TOptional<int64>& (*Access)(FStudioHome4Spec&); };
    struct Home4FBoolField { FStudioHome4Field Field; TOptional<bool>& (*Access)(FStudioHome4Spec&); };
    struct Home4FStringField { FStudioHome4Field Field; FString& (*Access)(FStudioHome4Spec&); };
    FStudioHome4Field Home4Field(const TCHAR* Section, const TCHAR* Key, const TCHAR* Page, EStudioHome4FieldType Type,
        const TCHAR* Unit = TEXT(""), double Minimum = -1.e12, double Maximum = 1.e12, bool bOptional = true)
    {
        FStudioHome4Field F; F.Section=Section; F.Key=Key; F.Page=Page; F.Label=Key; F.Type=Type;
        F.Unit=Unit; F.Minimum=Minimum; F.Maximum=Maximum; F.bOptional=bOptional; return F;
    }
#define NUMBER(S,K,P,U,A,B) {Home4Field(TEXT(#S),TEXT(#K),TEXT(P),EStudioHome4FieldType::Number,TEXT(U),A,B), [](FStudioHome4Spec& V)->TOptional<double>& {return V.S.K;}}
    // Schema keys are lower camel case; storage fields retain Unreal naming conventions.
#define N(S,K,F,P,U,A,B) {Home4Field(TEXT(S),TEXT(K),TEXT(P),EStudioHome4FieldType::Number,TEXT(U),A,B), [](FStudioHome4Spec& V)->TOptional<double>& {return V.F;}}
    const TArray<Home4FNumberField>& Home4Numbers()
    {
        static const TArray<Home4FNumberField> V = {
            N("units","dxMeters",Units.DxMeters,"Lattice","m/cell",0,1.e12),
            N("units","dtSeconds",Units.DtSeconds,"Lattice","s/step",0,1.e12),
            N("units","densityReferenceKgM3",Units.DensityReferenceKgM3,"Lattice","kg/m3",0,1.e12),
            N("reference","lengthCells",Reference.LengthCells,"Lattice","cells",0,1.e12),
            N("reference","speedCellsPerStep",Reference.SpeedCellsPerStep,"Fluids & Interface","cells/step",0,1.e12),
            N("reference","timeSteps",Reference.TimeSteps,"Lattice","steps",0,1.e12),
            N("reference","mach",Reference.Mach,"Fluids & Interface","",0,1.e12),
            N("reference","reynolds",Reference.Reynolds,"Fluids & Interface","",0,1.e12),
            N("reference","froude",Reference.Froude,"Fluids & Interface","",0,1.e12),
            N("reference","bond",Reference.Bond,"Fluids & Interface","",0,1.e12),
            N("reference","weber",Reference.Weber,"Fluids & Interface","",0,1.e12),
            N("reference","capillary",Reference.Capillary,"Fluids & Interface","",0,1.e12),
            N("reference","peclet",Reference.Peclet,"Fluids & Interface","",0,1.e12),
            N("reference","cahn",Reference.Cahn,"Fluids & Interface","",0,1.e12),
            N("reference","atwood",Reference.Atwood,"Fluids & Interface","",-1,1),
            N("fluids","rhoHeavy",Fluids.RhoHeavy,"Fluids & Interface","lu",0,1.e12),
            N("fluids","rhoLight",Fluids.RhoLight,"Fluids & Interface","lu",0,1.e12),
            N("fluids","nuHeavy",Fluids.NuHeavy,"Fluids & Interface","cells2/step",0,1.e12),
            N("fluids","nuLight",Fluids.NuLight,"Fluids & Interface","cells2/step",0,1.e12),
            N("fluids","sigma",Fluids.Sigma,"Fluids & Interface","lu",0,1.e12),
            N("fluids","xi",Fluids.Xi,"Fluids & Interface","root cells",0,1.e12),
            N("fluids","mobility",Fluids.Mobility,"Fluids & Interface","cells2/step",0,1.e12),
            N("fluids","gravity",Fluids.Gravity,"Fluids & Interface","cells/step2",0,1.e12),
            N("fluids","phaseST",Fluids.PhaseST,"Fluids & Interface","",0,2),
            N("fluids","phaseSD1",Fluids.PhaseSD1,"Fluids & Interface","",0,2),
            N("fluids","phaseSD2",Fluids.PhaseSD2,"Fluids & Interface","",0,2),
            N("fluids","phaseSXY",Fluids.PhaseSXY,"Fluids & Interface","",0,2),
            N("fluids","gradientLimitFactor",Fluids.GradientLimitFactor,"Fluids & Interface","",0,1.e12),
            N("fluids","forceThresholdFactor",Fluids.ForceThresholdFactor,"Fluids & Interface","",0,1.e12),
            N("fluids","lightForceFactor",Fluids.LightForceFactor,"Fluids & Interface","",0,1.e12),
            N("fluids","lightPhaseCutoff",Fluids.LightPhaseCutoff,"Fluids & Interface","",0,1),
            N("geometry","heelDegrees",Geometry.HeelDegrees,"Geometry","degrees",-360,360),
            N("geometry","trimDegrees",Geometry.TrimDegrees,"Geometry","degrees",-360,360),
            N("geometry","yawDegrees",Geometry.YawDegrees,"Geometry","degrees",-360,360),
            N("geometry","sinkCells",Geometry.SinkCells,"Bodies","cells",-1.e12,1.e12),
            N("geometry","bandCells",Geometry.BandCells,"Geometry","cells",0,1.e12),
            N("geometry","refine",Geometry.Refine,"Geometry","",0,1.e12),
            N("geometry","bodyMass",Geometry.BodyMass,"Bodies","lu",0,1.e12),
            N("geometry","retabulateEvery",Geometry.RetabulateEvery,"Bodies","steps",0,1.e12),
            N("lattice","padUp",Lattice.PadUp,"Lattice","L",0,1.e12),
            N("lattice","padDown",Lattice.PadDown,"Lattice","L",0,1.e12),
            N("lattice","padSide",Lattice.PadSide,"Lattice","L",0,1.e12),
            N("lattice","depth",Lattice.Depth,"Lattice","L",0,1.e12),
            N("lattice","air",Lattice.Air,"Lattice","L",0,1.e12),
            N("zones","sponge",Zones.Sponge,"Boundaries & Zones","driver value",0,1.e12),
            N("zones","xBeach",Zones.XBeach,"Boundaries & Zones","driver value",0,1.e12),
            N("zones","xBeachStrength",Zones.XBeachStrength,"Boundaries & Zones","",0,1.e12),
            N("zones","beachY",Zones.BeachY,"Boundaries & Zones","driver value",0,1.e12),
            N("zones","beachGap",Zones.BeachGap,"Boundaries & Zones","driver value",0,1.e12),
            N("zones","zoneStrength",Zones.ZoneStrength,"Boundaries & Zones","",0,1.e12),
            N("zones","floorFriction",Zones.FloorFriction,"Boundaries & Zones","",0,1.e12),
            N("zones","phiTop",Zones.PhiTop,"Boundaries & Zones","",0,1),
            N("zones","phiBottom",Zones.PhiBottom,"Boundaries & Zones","",0,1),
            N("multidomain","z1",Multidomain.Z1,"Lattice","root cells",-1.e12,1.e12),
            N("multidomain","z2",Multidomain.Z2,"Lattice","root cells",-1.e12,1.e12),
            N("multidomain","margin",Multidomain.Margin,"Lattice","cells",0,1.e12),
            N("multidomain","bandDepth",Multidomain.BandDepth,"Lattice","cells",0,1.e12),
            N("multidomain","overlap",Multidomain.Overlap,"Lattice","cells",0,1.e12),
            N("multidomain","restrictionMargin",Multidomain.RestrictionMargin,"Lattice","cells",0,1.e12),
            N("multidomain","tauFloor",Multidomain.TauFloor,"Lattice","",0.5,1.e12),
            N("multidomain","finestMobility",Multidomain.FinestMobility,"Lattice","cells2/step",0,1.e12),
            N("multidomain","recipeCahn",Multidomain.RecipeCahn,"Lattice","",0,1.e12),
            N("run","travel",Run.Travel,"Run","L",0,1.e12),
            N("run","rampLength",Run.RampLength,"Run","L",0,1.e12),
            N("run","averageLength",Run.AverageLength,"Run","L",0,1.e12),
            N("performance","measuredMLUPS",Performance.MeasuredMLUPS,"Run","MLUPS",0,1.e12)
        }; return V;
    }
#undef N
#undef NUMBER
#define I(S,K,F,P,A,B) {Home4Field(TEXT(S),TEXT(K),TEXT(P),EStudioHome4FieldType::Integer,TEXT(""),A,B), [](FStudioHome4Spec& V)->TOptional<int64>& {return V.F;}}
    const TArray<Home4FIntegerField>& Home4Integers()
    {
        static const TArray<Home4FIntegerField> V = {
            I("multidomain","levels",Multidomain.Levels,"Lattice",1,Home4MaxLevels),
            I("multidomain","massFixEvery",Multidomain.MassFixEvery,"Lattice",1,Home4MaxCount),
            I("run","steps",Run.Steps,"Run",1,Home4MaxCount),
            I("run","measureEvery",Run.MeasureEvery,"Run",1,Home4MaxCount),
            I("run","printEvery",Run.PrintEvery,"Run",1,Home4MaxCount),
            I("run","saveEvery",Run.SaveEvery,"Run",1,Home4MaxCount),
            I("run","vizEvery",Run.VizEvery,"Run",1,Home4MaxCount),
            I("run","restartEvery",Run.RestartEvery,"Run",1,Home4MaxCount),
            I("performance","availableBytes",Performance.AvailableBytes,"Run",1,Home4MaxCount)
        }; return V;
    }
#undef I
#define B(S,K,F,P) {Home4Field(TEXT(S),TEXT(K),TEXT(P),EStudioHome4FieldType::Boolean), [](FStudioHome4Spec& V)->TOptional<bool>& {return V.F;}}
    const TArray<Home4FBoolField>& Home4Bools()
    {
        static const TArray<Home4FBoolField> V = {
            B("fluids","gradientLimiter",Fluids.GradientLimiter,"Fluids & Interface"),
            B("fluids","forceThresholding",Fluids.ForceThresholding,"Fluids & Interface"),
            B("geometry","float",Geometry.Float,"Geometry"), B("geometry","noEquilibrate",Geometry.NoEquilibrate,"Bodies"),
            B("zones","periodicX",Zones.PeriodicX,"Boundaries & Zones"), B("zones","periodicY",Zones.PeriodicY,"Boundaries & Zones"),
            B("zones","periodicZ",Zones.PeriodicZ,"Boundaries & Zones"), B("zones","pinPhaseWalls",Zones.PinPhaseWalls,"Boundaries & Zones"),
            B("multidomain","evenWrap",Multidomain.EvenWrap,"Lattice"), B("multidomain","rootPhaseFree",Multidomain.RootPhaseFree,"Lattice"),
            B("multidomain","noTauFloor",Multidomain.NoTauFloor,"Lattice"), B("multidomain","noFullF2C",Multidomain.NoFullF2C,"Lattice"),
            B("multidomain","fixedCahnRefinement",Multidomain.FixedCahnRefinement,"Lattice"),
            B("run","bodyOnCpu",Run.BodyOnCpu,"Run"), B("run","noGpuKernels",Run.NoGpuKernels,"Run"),
            B("run","smoke",Run.Smoke,"Run"), B("run","noFrameAcceleration",Run.NoFrameAcceleration,"Run"),
            B("run","extensionImported",Run.ExtensionImported,"Run"), B("run","fallbackConfirmed",Run.FallbackConfirmed,"Run")
        }; return V;
    }
#undef B
#define S(S,K,F,P) {Home4Field(TEXT(S),TEXT(K),TEXT(P),EStudioHome4FieldType::String,TEXT(""),0,Home4MaxText,false), [](FStudioHome4Spec& V)->FString& {return V.F;}}
    const TArray<Home4FStringField>& Home4Strings()
    {
        static const TArray<Home4FStringField> V = {
            S("","recipeId",RecipeId,"Projects"), S("","lineageId",LineageId,"Projects"),
            S("fluids","tauMethod",Fluids.TauMethod,"Fluids & Interface"), S("fluids","surfaceTensionForm",Fluids.SurfaceTensionForm,"Fluids & Interface"),
            S("geometry","sourcePath",Geometry.SourcePath,"Geometry"), S("geometry","patchClassification",Geometry.PatchClassification,"Geometry"),
            S("geometry","sdfBackend",Geometry.SdfBackend,"Geometry"), S("geometry","cptPath",Geometry.CptPath,"Geometry"),
            S("geometry","bodyMotion",Geometry.BodyMotion,"Bodies"), S("geometry","retabulationPolicy",Geometry.RetabulationPolicy,"Bodies"),
            S("zones","massCorrection",Zones.MassCorrection,"Boundaries & Zones"), S("zones","pierceBoundary",Zones.PierceBoundary,"Boundaries & Zones"),
            S("zones","walls",Zones.Walls,"Boundaries & Zones"), S("zones","inlet",Zones.Inlet,"Boundaries & Zones"),
            S("zones","outlet",Zones.Outlet,"Boundaries & Zones"), S("zones","waveAbsorption",Zones.WaveAbsorption,"Boundaries & Zones"),
            S("multidomain","sneqMode",Multidomain.SneqMode,"Lattice"),
            S("run","device",Run.Device,"Run"), S("run","queueTarget",Run.QueueTarget,"Run"), S("run","tag",Run.Tag,"Run"),
            S("run","outDirectory",Run.OutDirectory,"Run"), S("run","initState",Run.InitState,"Run"), S("run","saveState",Run.SaveState,"Run"),
            S("run","vizDirectory",Run.VizDirectory,"Run"), S("performance","measurementSource",Performance.MeasurementSource,"Run")
        }; return V;
    }
#undef S
    const TCHAR* Home4Sections[] = {TEXT("units"),TEXT("reference"),TEXT("fluids"),TEXT("geometry"),TEXT("lattice"),TEXT("zones"),TEXT("multidomain"),TEXT("run"),TEXT("performance")};
    Home4FObject Home4Section(const Home4FObject& O, const FString& Name)
    {
        if(Name.IsEmpty()) return O;
        const Home4FObject* V=nullptr; return O && O->TryGetObjectField(Name,V) ? *V : nullptr;
    }
    bool Home4TextValid(const FString& S)
    {
        if(S.Len()>Home4MaxText) return false;
        for(TCHAR C:S) if(C<32 || C==127) return false;
        return true;
    }
    bool Home4Integer(double D, double Min, double Max)
    {return FMath::IsFinite(D)&&D>=Min&&D<=Max&&D==FMath::FloorToDouble(D);}
    template<typename T> void Home4Optional(const Home4FObject& O,const FString& Key,const TOptional<T>& V)
    {
        if(!V.IsSet()) O->SetField(Key,MakeShared<FJsonValueNull>());
        else O->SetNumberField(Key,double(V.GetValue()));
    }
    void Home4Optional(const Home4FObject& O,const FString& Key,const TOptional<bool>& V)
    {
        if(!V.IsSet()) O->SetField(Key,MakeShared<FJsonValueNull>());
        else O->SetBoolField(Key,V.GetValue());
    }
    template<typename T> Home4FValues Home4Components(const T& V)
    {return {MakeShared<FJsonValueNumber>(double(V.X)),MakeShared<FJsonValueNumber>(double(V.Y)),MakeShared<FJsonValueNumber>(double(V.Z))};}
    template<typename T> void Home4Vector(const Home4FObject& O,const TCHAR* Key,const TOptional<T>& V)
    {if(V.IsSet())O->SetArrayField(Key,Home4Components(V.GetValue()));else O->SetField(Key,MakeShared<FJsonValueNull>());}
    template<typename T> void Home4Array(const Home4FObject& O,const TCHAR* Key,const TArray<T>& V)
    {Home4FValues A;for(T N:V)A.Add(MakeShared<FJsonValueNumber>(double(N)));O->SetArrayField(Key,A);}
    const TCHAR* Home4DisplayName(EStudioHome4UnitDisplay V)
    {switch(V){case EStudioHome4UnitDisplay::Lattice:return TEXT("lattice");case EStudioHome4UnitDisplay::Physical:return TEXT("physical");case EStudioHome4UnitDisplay::Nondimensional:return TEXT("nondimensional");default:return TEXT("invalid");}}
    const TCHAR* Home4BackendName(EStudioHome4Backend V)
    {switch(V){case EStudioHome4Backend::Unknown:return TEXT("unknown");case EStudioHome4Backend::Metal:return TEXT("metal");case EStudioHome4Backend::CUDA:return TEXT("cuda");case EStudioHome4Backend::PyTorch:return TEXT("pytorch");default:return TEXT("invalid");}}
    bool Home4ReadVector(const Home4FObject& O,const TCHAR* Key,double* Out,bool& bSet)
    {
        const auto V=O->TryGetField(Key); bSet=false;
        if(!V||V->Type==EJson::Null) return true;
        const Home4FValues* A=nullptr; if(!V->TryGetArray(A)||A->Num()!=3) return false;
        for(int32 I=0;I<3;++I)if(!(*A)[I]||(*A)[I]->Type!=EJson::Number||!(*A)[I]->TryGetNumber(Out[I])||!FMath::IsFinite(Out[I]))return false;
        bSet=true;return true;
    }
    template<typename T> bool Home4ReadArray(const Home4FObject& O,const TCHAR* Key,TArray<T>& Out,int32 Limit,bool bIntegral)
    {
        const Home4FValues* A=nullptr;if(!O->TryGetArrayField(Key,A)||A->Num()>Limit)return false;
        for(const auto& V:*A){double D;if(!V||V->Type!=EJson::Number||!V->TryGetNumber(D)||!FMath::IsFinite(D)||FMath::Abs(D)>1.e12||(bIntegral&&!Home4Integer(D,0,Home4MaxCount)))return false;Out.Add(T(D));}
        return true;
    }
    bool Home4IsTrue(const TOptional<bool>& V){return V.IsSet()&&V.GetValue();}
    bool Home4Positive(const TOptional<double>& V){return V.IsSet()&&FMath::IsFinite(V.GetValue())&&V.GetValue()>0;}
    TOptional<double> Home4FiniteResult(double V){return FMath::IsFinite(V)?TOptional<double>(V):TOptional<double>();}
    void Home4Issue(FStudioHome4Derived& D,EStudioHome4IssueSeverity S,const TCHAR* Field,const TCHAR* Text)
    {FStudioHome4Issue I;I.Severity=S;I.Field=Field;I.Message=Text;D.Issues.Add(MoveTemp(I));}
    bool Home4Multiply(uint64 A,uint64 B,uint64& Out)
    {if(B&&A>MAX_uint64/B)return false;Out=A*B;return true;}
    FString Home4Number(double V){return FString::Printf(TEXT("%.17g"),V);}
    FString Home4ReadableLabel(const FString& Key)
    {
        FString Label;
        for(int32 I=0;I<Key.Len();++I)
        {
            const TCHAR C=Key[I];
            if(I>0&&FChar::IsUpper(C)&&(!FChar::IsUpper(Key[I-1])||(I+1<Key.Len()&&FChar::IsLower(Key[I+1]))))Label.AppendChar(TEXT(' '));
            Label.AppendChar(I==0?FChar::ToUpper(C):C);
        }
        return Label;
    }
}

const TArray<FStudioHome4Field>& StudioHome4Config::Fields()
{
    static const TArray<FStudioHome4Field> Result=[]
    {
        TArray<FStudioHome4Field> A;
        for(const auto& F:Home4Numbers())A.Add(F.Field);
        for(const auto& F:Home4Integers())A.Add(F.Field);
        for(const auto& F:Home4Bools())A.Add(F.Field);
        for(const auto& F:Home4Strings())A.Add(F.Field);
        auto Display=Home4Field(TEXT("units"),TEXT("display"),TEXT("Settings"),EStudioHome4FieldType::String,TEXT(""),0,0,false);
        Display.Choices={TEXT("lattice"),TEXT("physical"),TEXT("nondimensional")};A.Add(Display);
        auto Backend=Home4Field(TEXT("run"),TEXT("backend"),TEXT("Run"),EStudioHome4FieldType::String,TEXT(""),0,0,false);
        Backend.Choices={TEXT("unknown"),TEXT("metal"),TEXT("cuda"),TEXT("pytorch")};A.Add(Backend);
        A.Add(Home4Field(TEXT("lattice"),TEXT("extents"),TEXT("Lattice"),EStudioHome4FieldType::IntegerVector,TEXT("cells"),1,1048576));
        A.Add(Home4Field(TEXT("run"),TEXT("blockShape"),TEXT("Run"),EStudioHome4FieldType::IntegerVector,TEXT("threads"),1,1024));
        A.Add(Home4Field(TEXT("geometry"),TEXT("centerOfGravity"),TEXT("Bodies"),EStudioHome4FieldType::NumberVector,TEXT("cells")));
        A.Add(Home4Field(TEXT("geometry"),TEXT("stiffness"),TEXT("Bodies"),EStudioHome4FieldType::NumberArray,TEXT("lu"),-1.e12,1.e12,false));
        A.Add(Home4Field(TEXT("multidomain"),TEXT("levelCells"),TEXT("Lattice"),EStudioHome4FieldType::IntegerArray,TEXT("cells"),1,Home4MaxCount,false));
        for(auto& F:A)
        {
            F.Label=Home4ReadableLabel(F.Key);
            F.Help=F.Label+TEXT(" request. Unset stays unknown; the command preview reports any unverified driver encoding.");
            if(F.Key==TEXT("patchClassification"))F.Choices={TEXT(""),TEXT("CB"),TEXT("HKR"),TEXT("HKr"),TEXT("full")};
            if(F.Key==TEXT("sdfBackend"))F.Choices={TEXT(""),TEXT("libigl"),TEXT("CPT")};
            if(F.Key==TEXT("massCorrection"))F.Choices={TEXT(""),TEXT("beach"),TEXT("global"),TEXT("off")};
            if(F.Key==TEXT("sneqMode"))F.Choices={TEXT(""),TEXT("dorschner"),TEXT("derived")};
            if(F.Key==TEXT("bodyMotion"))F.Choices={TEXT(""),TEXT("fixed"),TEXT("forced-heave"),TEXT("forced-pitch"),TEXT("forced-spin"),TEXT("free")};
            if(F.Key==TEXT("surfaceTensionForm"))F.Help=TEXT("Force form request (the feedback identifies muphi). Exact driver support is unverified.");
            if(F.Key==TEXT("mobility"))F.Help=TEXT("Root-level mobility. For MD, finest mobility is root mobility times 2^(levels-1).");
            if(F.Key==TEXT("finestMobility"))F.Help=TEXT("Recipe mobility at the finest level; root mobility is this value divided by 2^(levels-1).");
            if(F.Key==TEXT("xi"))F.Help=TEXT("Interface width in root cells. Fixed-Cn refinement requires xi to scale with reference length.");
            if(F.Key==TEXT("saveState"))F.Help=TEXT("Driver checkpoint request. Argument arity is not specified in the appendix and requires verification.");
            if(F.Key==TEXT("dxMeters")){F.Label=TEXT("Cell spacing (Δx)");F.Help=TEXT("Physical metres per root lattice cell. Fields remain in lattice units; this map converts their display.");}
            if(F.Key==TEXT("dtSeconds")){F.Label=TEXT("Step duration (Δt)");F.Help=TEXT("Physical seconds per root time step. HOME4 advances explicitly once per lattice step.");}
            if(F.Key==TEXT("densityReferenceKgM3")){F.Label=TEXT("Density unit (ρ ref)");F.Help=TEXT("Physical density represented by one lattice density unit; required for pressure, force and energy conversions.");}
            if(F.Key==TEXT("lengthCells")){F.Label=TEXT("Reference length (L)");F.Help=TEXT("Body/reference length in root cells. Maps to --L for the hull driver and sets Re, Fr, Cn and force scales.");}
            if(F.Key==TEXT("speedCellsPerStep")){F.Label=TEXT("Reference speed (U)");F.Help=TEXT("Lattice speed in cells per step. Ma=U/(1/√3); above 0.1 warns and above 0.3 blocks.");}
            if(F.Key==TEXT("timeSteps")){F.Label=TEXT("Reference time");F.Help=TEXT("Benchmark time scale in root steps for t/t0 or t/T. When unset, nondimensional time uses L/U.");}
            if(F.Key==TEXT("mach")){F.Label=TEXT("Mach number (Ma)");F.Help=TEXT("Ma=U/cs, cs=1/√3. A target derives missing lattice speed; conflicting requests block.");}
            if(F.Key==TEXT("reynolds")){F.Label=TEXT("Reynolds number (Re)");F.Help=TEXT("Re=UL/νH. Maps to --Re_ref; resolution, Mach and tau jointly constrain attainable Re.");}
            if(F.Key==TEXT("froude")){F.Label=TEXT("Froude number (Fr)");F.Help=TEXT("Fr=U/√(gL). Hull --Fn; the wake wavelength is 2π Fr² L.");}
            if(F.Key==TEXT("bond")){F.Label=TEXT("Bond number (Bo)");F.Help=TEXT("Bo=ρH g L²/σ. Hull --Bo derives surface tension from this target.");}
            if(F.Key==TEXT("weber")){F.Label=TEXT("Weber number (We)");F.Help=TEXT("We=ρH U² L/σ; requires reference speed, length, heavy density and surface tension.");}
            if(F.Key==TEXT("capillary")){F.Label=TEXT("Capillary number (Ca)");F.Help=TEXT("Ca=ρH νH U/σ, using heavy-phase dynamic viscosity ρHνH.");}
            if(F.Key==TEXT("peclet")){F.Label=TEXT("Phase Peclet number (Peφ)");F.Help=TEXT("Peφ=UL/M. MD mobility is checked at the finest level.");}
            if(F.Key==TEXT("cahn")||F.Key==TEXT("recipeCahn")){F.Label=F.Key==TEXT("cahn")?TEXT("Cahn number (Cn)"):TEXT("Recipe Cahn number");F.Help=TEXT("Cn=ξ/L. A refinement ladder holding Cn fixed must scale root-cell interface width with reference resolution.");}
            if(F.Key==TEXT("atwood")){F.Label=TEXT("Atwood number (At)");F.Help=TEXT("At=(ρH−ρL)/(ρH+ρL); can derive a missing phase density from the other phase density.");}
            if(F.Key==TEXT("rhoHeavy")){F.Label=TEXT("Heavy-fluid density (ρH)");F.Help=TEXT("Heavy density in lattice units. The appendix supplies no --rho_H flag; explicit requests need a verified driver contract.");}
            if(F.Key==TEXT("rhoLight")){F.Label=TEXT("Light-fluid density (ρL)");F.Help=TEXT("Light density in lattice units; --rho_L. At 1000:1 both gradient limiter and force thresholding are required.");}
            if(F.Key==TEXT("nuHeavy")||F.Key==TEXT("nuLight")){F.Label=F.Key==TEXT("nuHeavy")?TEXT("Heavy-fluid viscosity (νH)"):TEXT("Light-fluid viscosity (νL)");F.Help=TEXT("Kinematic viscosity in cells²/step. τ=0.5+3ν; the light-phase margin below 0.01 warns.");}
            if(F.Key==TEXT("sigma")){F.Label=TEXT("Surface tension (σ)");F.Help=TEXT("Lattice surface tension. The hull appendix derives this through --Bo; no direct flag is asserted.");}
            if(F.Key==TEXT("xi"))F.Label=TEXT("Interface width (ξ)");
            if(F.Key==TEXT("mobility"))F.Label=TEXT("Root mobility (M0)");
            if(F.Key==TEXT("finestMobility"))F.Label=TEXT("Finest-level mobility");
            if(F.Key==TEXT("gravity")){F.Label=TEXT("Gravity magnitude (g)");F.Help=TEXT("Lattice acceleration magnitude, cells/step²; hull --g. Refined level d uses g0/2^d.");}
            if(F.Key.StartsWith(TEXT("phaseS"))){F.Label=TEXT("Phase relaxation ")+F.Key.Mid(5);F.Help=TEXT("Phase-moment relaxation rate (sT, sD1, sD2 or sxy), strictly between 0 and 2. Solver flag contract is unavailable.");}
            if(F.Key==TEXT("gradientLimiter"))F.Help=TEXT("Limit |∇φ| relative to the tanh slope; feedback specifies factor 1.6. Both safeguards are load bearing at high density ratio.");
            if(F.Key==TEXT("forceThresholding"))F.Help=TEXT("Threshold forcing relative to |Fb|; feedback specifies 60|Fb| and factor 0.6 where φ<0.1. Exact solver contract is unverified.");
            if(F.Key==TEXT("extents"))F.Help=TEXT("Root Cartesian lattice Nx, Ny, Nz. HOME4 has a lattice and cut links rather than an FV meshing stage.");
            if(F.Key.StartsWith(TEXT("pad"))||F.Key==TEXT("depth")||F.Key==TEXT("air"))F.Help=TEXT("Tank layout in body/reference lengths. Encoded by its corresponding appendix flag (--pad_up, --pad_down, --pad_side, --depth or --air).");
            if(F.Key==TEXT("sponge"))F.Help=TEXT("Solver sponge width; compare with 2π Fr²L after driver units are verified. Sponge relaxes dynamic pressure and velocity toward the stream.");
            if(F.Key.StartsWith(TEXT("xBeach"))||F.Key.StartsWith(TEXT("beach")))F.Help=TEXT("Driver beach region or strength request. Beach relaxes velocity to the stream and phase to flat while leaving pressure untouched; exact region units need the driver.");
            if(F.Key==TEXT("massCorrection"))F.Help=TEXT("Phase-mass correction mode: beach, global or off; hull --mass_correct.");
            if(F.Key==TEXT("pierceBoundary"))F.Help=TEXT("Hull --pierce_bc mode request. The appendix omits accepted values and argument arity; preserved but not encoded yet.");
            if(F.Key==TEXT("phiTop")||F.Key==TEXT("phiBottom")||F.Key==TEXT("pinPhaseWalls"))F.Help=TEXT("Phase-wall pinning request (φtop/φbot and pin_phase_walls). Not an appendix-supported hull flag.");
            if(F.Key==TEXT("levels"))F.Help=TEXT("Total MD levels including root; finest depth is levels−1 and each refinement factor is 2. Hull appendix has no MD flags.");
            if(F.Key==TEXT("tauFloor"))F.Help=TEXT("Explicit minimum τ for per-level physics. Raises viscosity where active and changes local Reynolds number; driver semantics need verification.");
            if(F.Key==TEXT("evenWrap"))F.Help=TEXT("Even-wrap request for periodic MD axes. Odd extents on selected periodic axes block.");
            if(F.Key==TEXT("rootPhaseFree"))F.Help=TEXT("Phase-free MD root request; must remain explicit because it changes per-level physics.");
            if(F.Key==TEXT("noFullF2C"))F.Label=TEXT("Disable full fine-to-coarse transfer");
            if(F.Key==TEXT("sneqMode")){F.Label=TEXT("Nonequilibrium strain factor mode");F.Help=TEXT("MD Sneq factor request: dorschner or derived. Requires the MD driver's own verified contract.");}
            if(F.Key==TEXT("travel"))F.Help=TEXT("Duration in body lengths; --travel. Steps remain a separate explicit override.");
            if(F.Key==TEXT("rampLength"))F.Help=TEXT("Cubic inlet speed ramp in body lengths; --ramp_L.");
            if(F.Key==TEXT("averageLength"))F.Help=TEXT("Steady-window averaging length in body lengths; --avg_L.");
            if(F.Key==TEXT("measureEvery"))F.Help=TEXT("Trace/telemetry cadence in root steps; --measure_every. Independent of viz and restart outputs.");
            if(F.Key==TEXT("printEvery"))F.Help=TEXT("Driver stdout cadence in root steps; --print_every.");
            if(F.Key==TEXT("saveEvery"))F.Help=TEXT("Slice cadence request in root steps. The note names --save_every but the hull appendix does not establish its contract.");
            if(F.Key==TEXT("vizEvery")||F.Key==TEXT("vizDirectory"))F.Help=TEXT("Full 3-D visualization snapshot request. Distinct from restart state; --viz-every/--viz-dir are not in the hull appendix.");
            if(F.Key==TEXT("backend"))F.Help=TEXT("Requested backend identity. Actual extension import must confirm Metal/CUDA/PyTorch; no backend flag is invented.");
            if(F.Key==TEXT("fallbackConfirmed"))F.Help=TEXT("Explicit acknowledgment permitting slow PyTorch fallback when the native extension is unavailable.");
            if(F.Key==TEXT("extensionImported"))F.Help=TEXT("Backend probe/import result supplied by the driver. Unknown stays unavailable.");
            if(F.Key==TEXT("tag"))F.Help=TEXT("Run tag; --tag. The _viz provenance naming rule must be applied explicitly for figure runs.");
            if(F.Key==TEXT("initState"))F.Help=TEXT("Warm-start state path; --init_state. Grid compatibility is enforced by the actual driver's load_state contract.");
            if(F.Key==TEXT("measuredMLUPS"))F.Help=TEXT("Measured million lattice updates per second supplied for this machine/workload. No benchmark speed is invented.");
            if(F.Key==TEXT("levelCells"))F.Help=TEXT("Explicit allocated cells at every MD depth, including root. Cost uses 2^d substeps per root step.");
            if(F.Key==TEXT("centerOfGravity"))F.Help=TEXT("Body CoG in root lattice cells for attitude and moment reporting. Solver body contract is unverified.");
            if(F.Key==TEXT("stiffness"))F.Help=TEXT("Empty or 36 row-major entries of a 6-DOF hydrostatic stiffness matrix; no hydrostatics are inferred.");
        }
        return A;
    }();
    return Result;
}

bool StudioHome4Config::Validate(const FStudioHome4Spec& Spec,FString& Error)
{
    auto Fail=[&](const FString& Message){Error=Message;return false;};
    if(Spec.Version!=1)return Fail(TEXT("Unsupported HOME4 run-spec version."));
    // Accessors do not mutate their arguments; the copy also keeps constness explicit.
    FStudioHome4Spec V=Spec;
    for(const auto& F:Home4Numbers())
    {
        const auto& N=F.Access(V);
        if(N.IsSet()&&(!FMath::IsFinite(*N)||*N<F.Field.Minimum||*N>F.Field.Maximum))
            return Fail(F.Field.Section+TEXT(".")+F.Field.Key+TEXT(" must be finite and within its supported bounds."));
    }
    for(const auto& F:Home4Integers())
    {const auto& N=F.Access(V);if(N.IsSet()&&(*N<F.Field.Minimum||*N>F.Field.Maximum))return Fail(F.Field.Section+TEXT(".")+F.Field.Key+TEXT(" must be a bounded whole count."));}
    for(const auto& F:Home4Strings())if(!Home4TextValid(F.Access(V)))return Fail(F.Field.Section+TEXT(".")+F.Field.Key+TEXT(" contains a control character or exceeds 4096 characters."));
    for(const auto& F:Fields())
    {
        if(F.Choices.IsEmpty()||F.Key==TEXT("display")||F.Key==TEXT("backend"))continue;
        const auto* Binding=Home4Strings().FindByPredicate([&](const Home4FStringField& Item){return Item.Field.Section==F.Section&&Item.Field.Key==F.Key;});
        if(Binding&&!F.Choices.Contains(Binding->Access(V)))return Fail(F.Section+TEXT(".")+F.Key+TEXT(" is not a supported choice."));
    }
    if(int32(Spec.Units.Display)>int32(EStudioHome4UnitDisplay::Nondimensional)||int32(Spec.Run.Backend)>int32(EStudioHome4Backend::PyTorch))
        return Fail(TEXT("Invalid HOME4 units or backend choice."));
    for(const auto& N:{Spec.Units.DxMeters,Spec.Units.DtSeconds,Spec.Units.DensityReferenceKgM3,Spec.Reference.LengthCells,Spec.Reference.TimeSteps,Spec.Fluids.RhoHeavy,Spec.Fluids.RhoLight})
        if(N.IsSet()&&*N<=0)return Fail(TEXT("Unit-map scales, reference length/time and densities must be positive when supplied."));
    if(Spec.Lattice.Extents.IsSet()&&(Spec.Lattice.Extents->GetMin()<1||Spec.Lattice.Extents->GetMax()>1048576))return Fail(TEXT("Lattice extents must be integer counts from 1 to 1048576."));
    if(Spec.Run.BlockShape.IsSet()&&(Spec.Run.BlockShape->GetMin()<1||Spec.Run.BlockShape->GetMax()>1024))return Fail(TEXT("Block shape components must be integer counts from 1 to 1024."));
    if(Spec.Geometry.CenterOfGravity.IsSet())
    {const auto& C=*Spec.Geometry.CenterOfGravity;if(!FMath::IsFinite(C.X)||!FMath::IsFinite(C.Y)||!FMath::IsFinite(C.Z)||C.GetAbsMax()>1.e12)return Fail(TEXT("Center of gravity must have bounded finite components."));}
    if(!Spec.Geometry.Stiffness.IsEmpty()&&Spec.Geometry.Stiffness.Num()!=36)return Fail(TEXT("Stiffness must be empty or contain 36 row-major matrix entries."));
    if(Spec.Geometry.RetabulateEvery.IsSet()&&!Home4Integer(*Spec.Geometry.RetabulateEvery,1,Home4MaxCount))return Fail(TEXT("Retabulation cadence must be a positive whole step count."));
    for(double N:Spec.Geometry.Stiffness)if(!FMath::IsFinite(N)||FMath::Abs(N)>1.e12)return Fail(TEXT("Stiffness contains a non-finite or unbounded value."));
    if(Spec.Multidomain.LevelCells.Num()>Home4MaxLevels)return Fail(TEXT("Too many MD level cell counts."));
    for(int64 N:Spec.Multidomain.LevelCells)if(N<1||N>Home4MaxCount)return Fail(TEXT("MD level cell counts must be positive bounded integers."));
    if(Spec.Performance.Allocations.Num()>256)return Fail(TEXT("Allocation list exceeds 256 entries."));
    for(const auto& A:Spec.Performance.Allocations)
        if(A.Name.IsEmpty()||!Home4TextValid(A.Name)||A.Name.Len()>120||
           (A.Nodes.IsSet()&&(*A.Nodes<1||*A.Nodes>Home4MaxCount))||A.Components<1||A.Components>1024||
           A.BytesPerComponent<1||A.BytesPerComponent>16||A.Buffers<1||A.Buffers>16)
            return Fail(TEXT("Allocation entries need a bounded name, optional positive node count, and bounded component/byte/buffer counts."));
    Error.Empty();return true;
}

TSharedRef<FJsonObject> StudioHome4Config::ToJSON(const FStudioHome4Spec& Spec)
{
    auto O=MakeShared<FJsonObject>();O->SetNumberField(TEXT("version"),Spec.Version);
    for(const TCHAR* Name:Home4Sections)O->SetObjectField(Name,MakeShared<FJsonObject>());
    FStudioHome4Spec V=Spec;
    for(const auto& F:Home4Numbers())Home4Optional(Home4Section(O,F.Field.Section),F.Field.Key,F.Access(V));
    for(const auto& F:Home4Integers())Home4Optional(Home4Section(O,F.Field.Section),F.Field.Key,F.Access(V));
    for(const auto& F:Home4Bools())Home4Optional(Home4Section(O,F.Field.Section),F.Field.Key,F.Access(V));
    for(const auto& F:Home4Strings())Home4Section(O,F.Field.Section)->SetStringField(F.Field.Key,F.Access(V));
    Home4Section(O,TEXT("units"))->SetStringField(TEXT("display"),Home4DisplayName(V.Units.Display));
    Home4Section(O,TEXT("run"))->SetStringField(TEXT("backend"),Home4BackendName(V.Run.Backend));
    Home4Vector(Home4Section(O,TEXT("lattice")),TEXT("extents"),V.Lattice.Extents);
    Home4Vector(Home4Section(O,TEXT("run")),TEXT("blockShape"),V.Run.BlockShape);
    Home4Vector(Home4Section(O,TEXT("geometry")),TEXT("centerOfGravity"),V.Geometry.CenterOfGravity);
    Home4Array(Home4Section(O,TEXT("geometry")),TEXT("stiffness"),V.Geometry.Stiffness);
    Home4Array(Home4Section(O,TEXT("multidomain")),TEXT("levelCells"),V.Multidomain.LevelCells);
    Home4FValues Allocations;
    for(const auto& A:V.Performance.Allocations)
    {
        auto Item=MakeShared<FJsonObject>();Item->SetStringField(TEXT("name"),A.Name);Home4Optional(Item,TEXT("nodes"),A.Nodes);
        Item->SetNumberField(TEXT("components"),double(A.Components));Item->SetNumberField(TEXT("bytesPerComponent"),double(A.BytesPerComponent));Item->SetNumberField(TEXT("buffers"),double(A.Buffers));
        Allocations.Add(MakeShared<FJsonValueObject>(Item));
    }
    Home4Section(O,TEXT("performance"))->SetArrayField(TEXT("allocations"),Allocations);
    return O;
}

bool StudioHome4Config::FromJSON(const Home4FObject& O,FStudioHome4Spec& Out,FString& Error)
{
    Error=TEXT("Invalid HOME4 run spec. The existing request was kept.");
    if(!O)return false;
    FStudioHome4Spec V;double Version;
    if(!O->TryGetNumberField(TEXT("version"),Version)||Version!=1)return false;
    if(O->TryGetField(TEXT("version"))->Type!=EJson::Number)return false;
    TMap<FString,TSet<FString>> Allowed;
    Allowed.FindOrAdd(TEXT("")).Add(TEXT("version"));
    for(const TCHAR* Name:Home4Sections){Allowed.FindOrAdd(TEXT("")).Add(Name);if(!Home4Section(O,Name))return false;}
    for(const auto& F:Fields())Allowed.FindOrAdd(F.Section).Add(F.Key);
    Allowed.FindOrAdd(TEXT("performance")).Add(TEXT("allocations"));
    for(const auto& Pair:Allowed)
    {
        const auto Obj=Home4Section(O,Pair.Key);
        for(const auto& Entry:Obj->Values)if(!Pair.Value.Contains(FString(*Entry.Key))){Error=TEXT("Unknown HOME4 field: ")+Pair.Key+TEXT(".")+FString(*Entry.Key);return false;}
    }
    for(const auto& F:Home4Numbers())
    {
        const auto J=Home4Section(O,F.Field.Section)->TryGetField(F.Field.Key);
        if(!J||J->Type==EJson::Null)continue;
        double N;if(J->Type!=EJson::Number||!J->TryGetNumber(N)||!FMath::IsFinite(N))return false;F.Access(V)=N;
    }
    for(const auto& F:Home4Integers())
    {
        const auto J=Home4Section(O,F.Field.Section)->TryGetField(F.Field.Key);
        if(!J||J->Type==EJson::Null)continue;
        double N;if(J->Type!=EJson::Number||!J->TryGetNumber(N)||!Home4Integer(N,F.Field.Minimum,F.Field.Maximum))return false;F.Access(V)=int64(N);
    }
    for(const auto& F:Home4Bools())
    {
        const auto J=Home4Section(O,F.Field.Section)->TryGetField(F.Field.Key);
        if(!J||J->Type==EJson::Null)continue;
        bool B;if(J->Type!=EJson::Boolean||!J->TryGetBool(B))return false;F.Access(V)=B;
    }
    for(const auto& F:Home4Strings())
    {const auto J=Home4Section(O,F.Field.Section)->TryGetField(F.Field.Key);if(!J||J->Type!=EJson::String||!J->TryGetString(F.Access(V)))return false;}
    FString Display,Backend;
    if(Home4Section(O,TEXT("units"))->TryGetField(TEXT("display"))==nullptr||Home4Section(O,TEXT("run"))->TryGetField(TEXT("backend"))==nullptr)return false;
    if(Home4Section(O,TEXT("units"))->TryGetField(TEXT("display"))->Type!=EJson::String||Home4Section(O,TEXT("run"))->TryGetField(TEXT("backend"))->Type!=EJson::String)return false;
    if(!Home4Section(O,TEXT("units"))->TryGetStringField(TEXT("display"),Display)||!Home4Section(O,TEXT("run"))->TryGetStringField(TEXT("backend"),Backend))return false;
    if(Display==TEXT("lattice"))V.Units.Display=EStudioHome4UnitDisplay::Lattice;
    else if(Display==TEXT("physical"))V.Units.Display=EStudioHome4UnitDisplay::Physical;
    else if(Display==TEXT("nondimensional"))V.Units.Display=EStudioHome4UnitDisplay::Nondimensional;
    else return false;
    if(Backend==TEXT("unknown"))V.Run.Backend=EStudioHome4Backend::Unknown;
    else if(Backend==TEXT("metal"))V.Run.Backend=EStudioHome4Backend::Metal;
    else if(Backend==TEXT("cuda"))V.Run.Backend=EStudioHome4Backend::CUDA;
    else if(Backend==TEXT("pytorch"))V.Run.Backend=EStudioHome4Backend::PyTorch;
    else return false;
    double C[3];bool bSet;
    if(!Home4ReadVector(Home4Section(O,TEXT("lattice")),TEXT("extents"),C,bSet))return false;
    if(bSet){for(double N:C)if(!Home4Integer(N,1,1048576))return false;V.Lattice.Extents=FIntVector(int32(C[0]),int32(C[1]),int32(C[2]));}
    if(!Home4ReadVector(Home4Section(O,TEXT("run")),TEXT("blockShape"),C,bSet))return false;
    if(bSet){for(double N:C)if(!Home4Integer(N,1,1024))return false;V.Run.BlockShape=FIntVector(int32(C[0]),int32(C[1]),int32(C[2]));}
    if(!Home4ReadVector(Home4Section(O,TEXT("geometry")),TEXT("centerOfGravity"),C,bSet))return false;
    if(bSet)V.Geometry.CenterOfGravity=FVector(C[0],C[1],C[2]);
    if(!Home4ReadArray(Home4Section(O,TEXT("geometry")),TEXT("stiffness"),V.Geometry.Stiffness,36,false)||
       !Home4ReadArray(Home4Section(O,TEXT("multidomain")),TEXT("levelCells"),V.Multidomain.LevelCells,Home4MaxLevels,true))return false;
    const Home4FValues* Allocations=nullptr;
    if(!Home4Section(O,TEXT("performance"))->TryGetArrayField(TEXT("allocations"),Allocations)||Allocations->Num()>256)return false;
    for(const auto& J:*Allocations)
    {
        const Home4FObject* Item=nullptr;FStudioHome4Allocation A;
        if(!J||!J->TryGetObject(Item)||!Item||!Item->IsValid())return false;
        const auto Name=(*Item)->TryGetField(TEXT("name"));if(!Name||Name->Type!=EJson::String||!Name->TryGetString(A.Name))return false;
        const TSet<FString> Keys={TEXT("name"),TEXT("nodes"),TEXT("components"),TEXT("bytesPerComponent"),TEXT("buffers")};
        for(const auto& Pair:(*Item)->Values)if(!Keys.Contains(FString(*Pair.Key)))return false;
        const auto Nodes=(*Item)->TryGetField(TEXT("nodes"));
        if(Nodes&&Nodes->Type!=EJson::Null){double N;if(Nodes->Type!=EJson::Number||!Nodes->TryGetNumber(N)||!Home4Integer(N,1,Home4MaxCount))return false;A.Nodes=int64(N);}
        for(const TCHAR* Key:{TEXT("components"),TEXT("bytesPerComponent"),TEXT("buffers")})
        {const auto Value=(*Item)->TryGetField(Key);if(!Value||Value->Type!=EJson::Number)return false;}
        double Count;
        if(!(*Item)->TryGetNumberField(TEXT("components"),Count)||!Home4Integer(Count,1,1024))return false;A.Components=int64(Count);
        if(!(*Item)->TryGetNumberField(TEXT("bytesPerComponent"),Count)||!Home4Integer(Count,1,16))return false;A.BytesPerComponent=int64(Count);
        if(!(*Item)->TryGetNumberField(TEXT("buffers"),Count)||!Home4Integer(Count,1,16))return false;A.Buffers=int64(Count);
        V.Performance.Allocations.Add(MoveTemp(A));
    }
    if(!Validate(V,Error))return false;
    Out=MoveTemp(V);Error.Empty();return true;
}

FString StudioHome4Config::Serialize(const FStudioHome4Spec& Spec)
{
    FString Text;FJsonSerializer::Serialize(ToJSON(Spec),TJsonWriterFactory<>::Create(&Text));return Text;
}
bool StudioHome4Config::Parse(const FString& Text,FStudioHome4Spec& Out,FString& Error)
{
    Error=TEXT("Invalid HOME4 JSON. The existing request was kept.");
    if(Text.Len()>2*1024*1024)return false;
    Home4FObject O;if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O))return false;
    return FromJSON(O,Out,Error);
}

bool FStudioHome4Derived::HasBlockingIssues() const
{return Issues.ContainsByPredicate([](const FStudioHome4Issue& I){return I.Severity==EStudioHome4IssueSeverity::Blocking;});}

FStudioHome4Derived StudioHome4Config::Derive(const FStudioHome4Spec& S)
{
    FStudioHome4Derived D;FString Error;
    if(!Validate(S,Error)){FStudioHome4Issue I;I.Severity=EStudioHome4IssueSeverity::Blocking;I.Field=TEXT("spec");I.Message=Error;D.Issues.Add(MoveTemp(I));return D;}
    const auto& R=S.Reference;const auto& F=S.Fluids;const auto& MD=S.Multidomain;
    const int32 Count=MD.Levels.IsSet()?int32(*MD.Levels):1;
    const double FinestScale=double(uint64(1)<<(Count-1));
    D.Speed=R.SpeedCellsPerStep;D.RhoHeavy=F.RhoHeavy;D.RhoLight=F.RhoLight;D.Gravity=F.Gravity;
    D.NuHeavy=F.NuHeavy;D.NuLight=F.NuLight;D.Sigma=F.Sigma;D.Mobility=F.Mobility;D.Xi=F.Xi;
    if(!D.RhoLight.IsSet()&&Home4Positive(D.RhoHeavy)&&R.Atwood.IsSet()&&*R.Atwood>-1&&*R.Atwood<1)
        D.RhoLight=Home4FiniteResult(*D.RhoHeavy*(1-*R.Atwood)/(1+*R.Atwood));
    if(!D.RhoHeavy.IsSet()&&Home4Positive(D.RhoLight)&&R.Atwood.IsSet()&&*R.Atwood>-1&&*R.Atwood<1)
        D.RhoHeavy=Home4FiniteResult(*D.RhoLight*(1+*R.Atwood)/(1-*R.Atwood));
    if(!D.Mobility.IsSet()&&MD.FinestMobility.IsSet()&&MD.Levels.IsSet())D.Mobility=Home4FiniteResult(*MD.FinestMobility/FinestScale);
    if(!D.Xi.IsSet()&&R.Cahn.IsSet()&&Home4Positive(R.LengthCells))D.Xi=Home4FiniteResult(*R.Cahn* *R.LengthCells);
    // Resolve only direct supplied constraints. Repeated passes allow a target to use another derived quantity.
    for(int32 Pass=0;Pass<4;++Pass)
    {
        if(!D.Speed.IsSet())
        {
            if(R.Mach.IsSet())D.Speed=Home4FiniteResult(*R.Mach*Home4Cs);
            else if(R.Froude.IsSet()&&Home4Positive(D.Gravity)&&Home4Positive(R.LengthCells))D.Speed=Home4FiniteResult(*R.Froude*FMath::Sqrt(*D.Gravity* *R.LengthCells));
            else if(R.Reynolds.IsSet()&&Home4Positive(D.NuHeavy)&&Home4Positive(R.LengthCells))D.Speed=Home4FiniteResult(*R.Reynolds* *D.NuHeavy / *R.LengthCells);
            else if(R.Weber.IsSet()&&Home4Positive(D.Sigma)&&Home4Positive(D.RhoHeavy)&&Home4Positive(R.LengthCells))D.Speed=Home4FiniteResult(FMath::Sqrt(*R.Weber* *D.Sigma / (*D.RhoHeavy* *R.LengthCells)));
            else if(R.Capillary.IsSet()&&Home4Positive(D.Sigma)&&Home4Positive(D.RhoHeavy)&&Home4Positive(D.NuHeavy))D.Speed=Home4FiniteResult(*R.Capillary* *D.Sigma / (*D.RhoHeavy* *D.NuHeavy));
        }
        if(!D.Gravity.IsSet()&&Home4Positive(R.Froude)&&D.Speed.IsSet()&&Home4Positive(R.LengthCells))D.Gravity=Home4FiniteResult(FMath::Square(*D.Speed / *R.Froude) / *R.LengthCells);
        if(!D.NuHeavy.IsSet()&&Home4Positive(R.Reynolds)&&D.Speed.IsSet()&&Home4Positive(R.LengthCells))D.NuHeavy=Home4FiniteResult(*D.Speed* *R.LengthCells / *R.Reynolds);
        if(!D.Sigma.IsSet())
        {
            if(Home4Positive(R.Bond)&&Home4Positive(D.RhoHeavy)&&D.Gravity.IsSet()&&Home4Positive(R.LengthCells))D.Sigma=Home4FiniteResult(*D.RhoHeavy* *D.Gravity*FMath::Square(*R.LengthCells) / *R.Bond);
            else if(Home4Positive(R.Weber)&&Home4Positive(D.RhoHeavy)&&D.Speed.IsSet()&&Home4Positive(R.LengthCells))D.Sigma=Home4FiniteResult(*D.RhoHeavy*FMath::Square(*D.Speed)* *R.LengthCells / *R.Weber);
            else if(Home4Positive(R.Capillary)&&Home4Positive(D.RhoHeavy)&&Home4Positive(D.NuHeavy)&&D.Speed.IsSet())D.Sigma=Home4FiniteResult(*D.RhoHeavy* *D.NuHeavy* *D.Speed / *R.Capillary);
        }
        if(!D.Mobility.IsSet()&&Home4Positive(R.Peclet)&&D.Speed.IsSet()&&Home4Positive(R.LengthCells))D.Mobility=Home4FiniteResult(*D.Speed* *R.LengthCells / *R.Peclet);
    }
    if(D.Speed.IsSet())D.Mach=Home4FiniteResult(*D.Speed/Home4Cs);
    if(D.Speed.IsSet()&&Home4Positive(R.LengthCells)&&Home4Positive(D.NuHeavy))D.Reynolds=Home4FiniteResult(*D.Speed* *R.LengthCells / *D.NuHeavy);
    if(D.Speed.IsSet()&&Home4Positive(R.LengthCells)&&Home4Positive(D.Gravity))D.Froude=Home4FiniteResult(*D.Speed/FMath::Sqrt(*D.Gravity* *R.LengthCells));
    if(Home4Positive(D.RhoHeavy)&&D.Gravity.IsSet()&&Home4Positive(R.LengthCells)&&Home4Positive(D.Sigma))D.Bond=Home4FiniteResult(*D.RhoHeavy* *D.Gravity*FMath::Square(*R.LengthCells) / *D.Sigma);
    if(Home4Positive(D.RhoHeavy)&&D.Speed.IsSet()&&Home4Positive(R.LengthCells)&&Home4Positive(D.Sigma))D.Weber=Home4FiniteResult(*D.RhoHeavy*FMath::Square(*D.Speed)* *R.LengthCells / *D.Sigma);
    if(Home4Positive(D.RhoHeavy)&&D.Speed.IsSet()&&D.NuHeavy.IsSet()&&Home4Positive(D.Sigma))D.Capillary=Home4FiniteResult(*D.RhoHeavy* *D.NuHeavy* *D.Speed / *D.Sigma);
    if(D.Speed.IsSet()&&Home4Positive(R.LengthCells)&&Home4Positive(D.Mobility))D.Peclet=Home4FiniteResult(*D.Speed* *R.LengthCells / *D.Mobility);
    if(D.Xi.IsSet()&&Home4Positive(R.LengthCells))D.Cahn=Home4FiniteResult(*D.Xi / *R.LengthCells);
    if(Home4Positive(D.RhoHeavy)&&Home4Positive(D.RhoLight))D.Atwood=Home4FiniteResult((*D.RhoHeavy-*D.RhoLight)/(*D.RhoHeavy+*D.RhoLight));
    if(D.NuHeavy.IsSet()){D.TauHeavyMargin=Home4FiniteResult(3* *D.NuHeavy);D.TauHeavy=Home4FiniteResult(.5+3* *D.NuHeavy);}
    if(D.NuLight.IsSet()){D.TauLightMargin=Home4FiniteResult(3* *D.NuLight);D.TauLight=Home4FiniteResult(.5+3* *D.NuLight);}
    if(D.TauHeavyMargin.IsSet()&&Home4Positive(R.LengthCells))D.Knudsen=Home4FiniteResult(Home4Cs* *D.TauHeavyMargin / *R.LengthCells);
    if(D.Froude.IsSet()&&Home4Positive(R.LengthCells))D.WakeWavelength=Home4FiniteResult(2*PI*FMath::Square(*D.Froude)* *R.LengthCells);
    auto CheckTarget=[&](const TOptional<double>& Target,const TOptional<double>& Actual,const TCHAR* Key)
    {
        if(!Target.IsSet())return;
        if(!Actual.IsSet()){Home4Issue(D,EStudioHome4IssueSeverity::Information,Key,TEXT("Requested dimensionless group needs more lattice inputs before it can be derived."));return;}
        if(FMath::Abs(*Target-*Actual)>1.e-8*FMath::Max(1.,FMath::Max(FMath::Abs(*Target),FMath::Abs(*Actual))))
            Home4Issue(D,EStudioHome4IssueSeverity::Blocking,Key,TEXT("The requested dimensionless group conflicts with the supplied lattice parameters."));
    };
    CheckTarget(R.Mach,D.Mach,TEXT("reference.mach"));CheckTarget(R.Reynolds,D.Reynolds,TEXT("reference.reynolds"));
    CheckTarget(R.Froude,D.Froude,TEXT("reference.froude"));CheckTarget(R.Bond,D.Bond,TEXT("reference.bond"));
    CheckTarget(R.Weber,D.Weber,TEXT("reference.weber"));CheckTarget(R.Capillary,D.Capillary,TEXT("reference.capillary"));
    CheckTarget(R.Peclet,D.Peclet,TEXT("reference.peclet"));CheckTarget(R.Cahn,D.Cahn,TEXT("reference.cahn"));CheckTarget(R.Atwood,D.Atwood,TEXT("reference.atwood"));
    if(D.Mach.IsSet()&&*D.Mach>0.3+1.e-12)Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("reference.speedCellsPerStep"),TEXT("Lattice Mach exceeds 0.3; increase resolution or reduce lattice speed."));
    else if(D.Mach.IsSet()&&*D.Mach>0.1+1.e-12)Home4Issue(D,EStudioHome4IssueSeverity::Warning,TEXT("reference.speedCellsPerStep"),TEXT("Lattice Mach exceeds 0.1; compressibility error scales as Mach squared."));
    for(const auto& Margin:{D.TauHeavyMargin,D.TauLightMargin})if(Margin.IsSet()&&*Margin<=0)
        Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("fluids.nu"),TEXT("Relaxation time must exceed 0.5; positive viscosity is required."));
    if(D.TauLightMargin.IsSet()&&*D.TauLightMargin>0&&*D.TauLightMargin<.01)
        Home4Issue(D,EStudioHome4IssueSeverity::Warning,TEXT("fluids.nuLight"),TEXT("Light-phase tau margin is below 0.01; consider viscosity, tau method or an explicit MD tau floor."));
    if(D.Xi.IsSet()&&*D.Xi<=0)Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("fluids.xi"),TEXT("Diffuse-interface width must be positive."));
    else if(D.Xi.IsSet()&&(*D.Xi<4||*D.Xi>5))Home4Issue(D,EStudioHome4IssueSeverity::Warning,TEXT("fluids.xi"),TEXT("Interface width is outside the recommended 4–5 root cells; assess spurious currents and Cahn number."));
    if(D.Mobility.IsSet()&&*D.Mobility<=0)Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("fluids.mobility"),TEXT("Phase mobility must be positive."));
    if(D.Sigma.IsSet()&&*D.Sigma<=0)Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("fluids.sigma"),TEXT("Two-fluid diffuse-interface surface tension must be positive."));
    for(const auto& Rate:{F.PhaseST,F.PhaseSD1,F.PhaseSD2,F.PhaseSXY})if(Rate.IsSet()&&(*Rate<=0||*Rate>=2))
        Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("fluids.phaseRates"),TEXT("Phase relaxation rates must lie strictly between 0 and 2."));
    if(Home4Positive(D.RhoHeavy)&&Home4Positive(D.RhoLight))
    {
        const double Ratio=*D.RhoHeavy / *D.RhoLight;
        if(Ratio<1)Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("fluids.rhoHeavy"),TEXT("Heavy-phase density is smaller than light-phase density."));
        if(Ratio>=100&&(!Home4IsTrue(F.GradientLimiter)||!Home4IsTrue(F.ForceThresholding)))
            Home4Issue(D,Ratio>=1000?EStudioHome4IssueSeverity::Blocking:EStudioHome4IssueSeverity::Warning,TEXT("fluids.safeguards"),
                TEXT("High density ratio needs both gradient limiter and force thresholding; both are required at 1000:1."));
    }
    if(MD.RecipeCahn.IsSet()&&D.Cahn.IsSet()&&FMath::Abs(*MD.RecipeCahn-*D.Cahn)>1.e-8)
        Home4Issue(D,EStudioHome4IssueSeverity::Warning,TEXT("multidomain.recipeCahn"),TEXT("Cahn number differs from the recipe; fixed-Cn refinement must scale xi with reference length."));
    if(Home4IsTrue(MD.FixedCahnRefinement)&&!MD.RecipeCahn.IsSet())Home4Issue(D,EStudioHome4IssueSeverity::Information,TEXT("multidomain.recipeCahn"),TEXT("Supply the recipe Cahn number to check fixed-Cn refinement."));
    if(MD.FinestMobility.IsSet()&&D.Mobility.IsSet()&&MD.Levels.IsSet()&&FMath::Abs(*D.Mobility*FinestScale-*MD.FinestMobility)>1.e-8*FMath::Max(1.,*MD.FinestMobility))
        Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("multidomain.finestMobility"),TEXT("Root mobility must equal recipe finest-level mobility divided by 2^(levels-1)."));
    if(MD.FinestMobility.IsSet()&&!MD.Levels.IsSet())Home4Issue(D,EStudioHome4IssueSeverity::Information,TEXT("multidomain.levels"),TEXT("Finest-level mobility needs an explicit MD level count."));
    if(MD.TauFloor.IsSet()&&*MD.TauFloor<=.5)Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("multidomain.tauFloor"),TEXT("MD tau floor must exceed 0.5."));
    if(MD.TauFloor.IsSet()&&!Home4IsTrue(MD.NoTauFloor))Home4Issue(D,EStudioHome4IssueSeverity::Warning,TEXT("multidomain.tauFloor"),TEXT("The tau floor changes per-level viscosity and Reynolds number; treat it as a physics choice."));
    if(MD.Z1.IsSet()&&MD.Z2.IsSet()&&*MD.Z2<=*MD.Z1)Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("multidomain.z2"),TEXT("Second MD boundary must exceed the first."));
    if(MD.Levels.IsSet()&&MD.LevelCells.Num()>0&&MD.LevelCells.Num()!=Count)Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("multidomain.levelCells"),TEXT("Explicit per-level cell counts must match the requested level count."));
    if(S.Lattice.Extents.IsSet())
    {
        const auto& E=*S.Lattice.Extents;uint64 XY,XYZ;
        if(Home4Multiply(uint64(E.X),uint64(E.Y),XY)&&Home4Multiply(XY,uint64(E.Z),XYZ))D.RootCells=XYZ;
        else Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("lattice.extents"),TEXT("Lattice cell count exceeds the supported integer range."));
        if(Home4IsTrue(MD.EvenWrap)&&((Home4IsTrue(S.Zones.PeriodicX)&&(E.X%2))||(Home4IsTrue(S.Zones.PeriodicY)&&(E.Y%2))||(Home4IsTrue(S.Zones.PeriodicZ)&&(E.Z%2))))
            Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("multidomain.evenWrap"),TEXT("Periodic axes require even lattice extents for even-wrap MD."));
    }
    if(MD.LevelCells.Num()>0)
    {
        uint64 Total=0;for(int64 N:MD.LevelCells)Total+=uint64(N);D.TotalCells=Total;
        if(D.RootCells.IsSet()&&uint64(MD.LevelCells[0])!=*D.RootCells)Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("multidomain.levelCells"),TEXT("Root MD cell count differs from lattice extents."));
    }
    else if(Count==1)D.TotalCells=D.RootCells;
    if(S.Run.Backend==EStudioHome4Backend::Metal)
    {
        // 27*N < 2^32; compare before multiplying to avoid overflow.
        constexpr uint64 LastMetalCell=(uint64(1)<<32)-1;
        const uint64 Limit=LastMetalCell/27;
        if(D.RootCells.IsSet()&&*D.RootCells>Limit)Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("lattice.extents"),TEXT("Metal D3Q27 indexing requires 27 times the cell count to be strictly below 2^32."));
        for(int64 N:MD.LevelCells)if(uint64(N)>Limit)Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("multidomain.levelCells"),TEXT("An MD level exceeds the Metal D3Q27 index limit."));
    }
    if(S.Run.Backend==EStudioHome4Backend::Unknown)Home4Issue(D,EStudioHome4IssueSeverity::Information,TEXT("run.backend"),TEXT("Backend has not been selected or verified."));
    if((S.Run.Backend==EStudioHome4Backend::PyTorch||(S.Run.ExtensionImported.IsSet()&&!Home4IsTrue(S.Run.ExtensionImported)))&&!Home4IsTrue(S.Run.FallbackConfirmed))
        Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("run.fallbackConfirmed"),TEXT("PyTorch fallback is slow and requires explicit confirmation before launch."));
    if(!S.Run.ExtensionImported.IsSet())Home4Issue(D,EStudioHome4IssueSeverity::Information,TEXT("run.extensionImported"),TEXT("Extension import status is unavailable until a driver/backend probe supplies it."));
    if(S.Performance.Allocations.IsEmpty())Home4Issue(D,EStudioHome4IssueSeverity::Information,TEXT("performance.allocations"),TEXT("Memory estimate is unknown until explicit solver allocations are supplied."));
    else
    {
        uint64 Total=0;bool bKnown=true;
        for(const auto& A:S.Performance.Allocations)
        {
            if(!A.Nodes.IsSet()){bKnown=false;continue;}
            uint64 Bytes=uint64(*A.Nodes);
            if(!Home4Multiply(Bytes,uint64(A.Components),Bytes)||!Home4Multiply(Bytes,uint64(A.BytesPerComponent),Bytes)||!Home4Multiply(Bytes,uint64(A.Buffers),Bytes)||Total>MAX_uint64-Bytes)
            {Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("performance.allocations"),TEXT("Explicit allocation byte count exceeds the supported integer range."));bKnown=false;break;}
            Total+=Bytes;
        }
        if(bKnown)D.AllocationBytes=Total;
        else Home4Issue(D,EStudioHome4IssueSeverity::Information,TEXT("performance.allocations"),TEXT("Memory estimate is unknown because an allocation count is missing or invalid."));
    }
    if(D.AllocationBytes.IsSet()&&S.Performance.AvailableBytes.IsSet()&&*D.AllocationBytes>uint64(*S.Performance.AvailableBytes))
        Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("performance.availableBytes"),TEXT("Explicit allocations exceed the supplied available device memory."));
    if(Home4Positive(S.Performance.MeasuredMLUPS)&&S.Performance.MeasurementSource.IsEmpty())
        Home4Issue(D,EStudioHome4IssueSeverity::Warning,TEXT("performance.measurementSource"),TEXT("Measured MLUPS needs its machine and workload provenance."));
    if(Home4Positive(S.Performance.MeasuredMLUPS)&&S.Run.Steps.IsSet())
    {
        TOptional<double> Work;
        if(MD.LevelCells.Num()==Count)
        {double NodesPerRootStep=0;for(int32 I=0;I<Count;++I)NodesPerRootStep+=double(MD.LevelCells[I])*double(uint64(1)<<I);Work=NodesPerRootStep;}
        else if(Count==1&&D.RootCells.IsSet())Work=double(*D.RootCells);
        if(Work.IsSet())D.EstimatedSeconds=Home4FiniteResult(*Work*double(*S.Run.Steps) / (*S.Performance.MeasuredMLUPS*1.e6));
    }
    if(!D.EstimatedSeconds.IsSet())Home4Issue(D,EStudioHome4IssueSeverity::Information,TEXT("performance.measuredMLUPS"),TEXT("Cost estimate needs supplied measured MLUPS, step count, and explicit cell counts at every MD level."));
    for(int32 I=0;I<Count;++I)
    {
        FStudioHome4LevelPhysics P;P.Depth=I;P.Scale=double(uint64(1)<<I);
        auto Scaled=[&](const TOptional<double>& V,double K){return V.IsSet()?Home4FiniteResult(*V*K):TOptional<double>();};
        P.NuHeavy=Scaled(D.NuHeavy,P.Scale);P.NuLight=Scaled(D.NuLight,P.Scale);P.Sigma=Scaled(D.Sigma,P.Scale);
        P.Mobility=Scaled(D.Mobility,P.Scale);P.Gravity=Scaled(D.Gravity,1/P.Scale);P.Xi=Scaled(D.Xi,P.Scale);
        if(MD.TauFloor.IsSet()&&!Home4IsTrue(MD.NoTauFloor))
        {const double NuFloor=(*MD.TauFloor-.5)/3;if(P.NuHeavy.IsSet())P.NuHeavy=FMath::Max(*P.NuHeavy,NuFloor);if(P.NuLight.IsSet())P.NuLight=FMath::Max(*P.NuLight,NuFloor);}
        if(P.NuHeavy.IsSet())P.TauHeavy=Home4FiniteResult(.5+3* *P.NuHeavy);
        if(P.NuLight.IsSet())P.TauLight=Home4FiniteResult(.5+3* *P.NuLight);
        D.Levels.Add(MoveTemp(P));
    }
    if(Count>1&&S.Zones.ZoneStrength.IsSet())Home4Issue(D,EStudioHome4IssueSeverity::Information,TEXT("zones.zoneStrength"),TEXT("Per-level sponge exponent rescaling is identified by the feedback, but its driver formula must be verified before reporting values."));
    if(D.WakeWavelength.IsSet()&&S.Zones.Sponge.IsSet()&&*S.Zones.Sponge<*D.WakeWavelength)
        Home4Issue(D,EStudioHome4IssueSeverity::Information,TEXT("zones.sponge"),TEXT("Expected wake wavelength is available; verify driver sponge units before comparing width against 2*pi*Fr^2*L."));
    if(S.Zones.BeachGap.IsSet())Home4Issue(D,EStudioHome4IssueSeverity::Information,TEXT("zones.beachGap"),TEXT("Compare beach gap against the derived wake wavelength after the driver's beach-gap units are verified."));
    auto Required=[&](bool bKnown,const TCHAR* FieldName)
    {if(!bKnown)Home4Issue(D,EStudioHome4IssueSeverity::Blocking,FieldName,TEXT("A core numerical input is unknown; supply it before numerical launch."));};
    Required(D.Speed.IsSet(),TEXT("reference.speedCellsPerStep"));Required(Home4Positive(R.LengthCells),TEXT("reference.lengthCells"));
    Required(D.RhoHeavy.IsSet(),TEXT("fluids.rhoHeavy"));Required(D.RhoLight.IsSet(),TEXT("fluids.rhoLight"));
    Required(D.NuHeavy.IsSet(),TEXT("fluids.nuHeavy"));Required(D.NuLight.IsSet(),TEXT("fluids.nuLight"));
    Required(D.Sigma.IsSet(),TEXT("fluids.sigma"));Required(D.Mobility.IsSet(),TEXT("fluids.mobility"));Required(D.Xi.IsSet(),TEXT("fluids.xi"));
    Required(D.Gravity.IsSet(),TEXT("fluids.gravity"));Required(D.RootCells.IsSet(),TEXT("lattice.extents"));
    Required(S.Run.ExtensionImported.IsSet(),TEXT("run.extensionImported"));
    Required(F.PhaseST.IsSet(),TEXT("fluids.phaseST"));Required(F.PhaseSD1.IsSet(),TEXT("fluids.phaseSD1"));
    Required(F.PhaseSD2.IsSet(),TEXT("fluids.phaseSD2"));Required(F.PhaseSXY.IsSet(),TEXT("fluids.phaseSXY"));
    if(S.Run.Backend==EStudioHome4Backend::Unknown)Home4Issue(D,EStudioHome4IssueSeverity::Blocking,TEXT("run.backend"),TEXT("Select and verify a numerical backend before launch."));
    return D;
}

TOptional<double> StudioHome4Config::ConvertUnits(double Value,EStudioHome4Quantity Q,
    EStudioHome4UnitDisplay From,EStudioHome4UnitDisplay To,const FStudioHome4Spec& S)
{
    if(!FMath::IsFinite(Value)||int32(From)>2||int32(To)>2)return {};
    int32 Mass=0,Length=0,Time=0;
    switch(Q)
    {
        case EStudioHome4Quantity::Dimensionless:break;
        case EStudioHome4Quantity::Length:Length=1;break;
        case EStudioHome4Quantity::Time:Time=1;break;
        case EStudioHome4Quantity::Density:Mass=1;Length=-3;break;
        case EStudioHome4Quantity::Velocity:Length=1;Time=-1;break;
        case EStudioHome4Quantity::KinematicViscosity:
        case EStudioHome4Quantity::Mobility:Length=2;Time=-1;break;
        case EStudioHome4Quantity::Pressure:Mass=1;Length=-1;Time=-2;break;
        case EStudioHome4Quantity::Acceleration:Length=1;Time=-2;break;
        case EStudioHome4Quantity::SurfaceTension:Mass=1;Time=-2;break;
        case EStudioHome4Quantity::Force:Mass=1;Length=1;Time=-2;break;
        case EStudioHome4Quantity::Moment:
        case EStudioHome4Quantity::Energy:Mass=1;Length=2;Time=-2;break;
        case EStudioHome4Quantity::StrainRate:Time=-1;break;
        default:return {};
    }
    if(From==To)return Value;
    auto Scale=[&](EStudioHome4UnitDisplay Display)->TOptional<double>
    {
        if(Display==EStudioHome4UnitDisplay::Lattice)return 1.;
        TOptional<double> Rho,X,T;
        if(Display==EStudioHome4UnitDisplay::Physical)
        {Rho=S.Units.DensityReferenceKgM3;X=S.Units.DxMeters;T=S.Units.DtSeconds;}
        else
        {
            const auto D=Derive(S);Rho=D.RhoHeavy;X=S.Reference.LengthCells;T=S.Reference.TimeSteps;
            if(!T.IsSet()&&Home4Positive(X)&&Home4Positive(D.Speed))T=Home4FiniteResult(*X / *D.Speed);
        }
        const int32 XPower=Length+3*Mass;
        if((Mass&&!Home4Positive(Rho))||(XPower&&!Home4Positive(X))||(Time&&!Home4Positive(T)))return {};
        double Result=1.;
        if(Mass)Result*=FMath::Pow(*Rho,double(Mass));
        if(XPower)Result*=FMath::Pow(*X,double(XPower));
        if(Time)Result*=FMath::Pow(*T,double(Time));
        if(!FMath::IsFinite(Result)||Result<=0)return {};
        // physical values multiply by the SI scale; nondimensional values divide by reference scale.
        return Display==EStudioHome4UnitDisplay::Physical?Result:1/Result;
    };
    const auto A=Scale(From),B=Scale(To);if(!Home4Positive(A)||!Home4Positive(B))return {};
    return Home4FiniteResult(Value / *A * *B);
}

FString StudioHome4Config::ShellDisplay(const TArray<FString>& Argv)
{
    TArray<FString> Quoted;
    for(const FString& Arg:Argv)
    {
        FString Escaped=Arg;Escaped.ReplaceInline(TEXT("'"),TEXT("'\"'\"'"));
        Quoted.Add(TEXT("'")+Escaped+TEXT("'"));
    }
    return FString::Join(Quoted,TEXT(" "));
}

bool StudioHome4Config::BuildHullDriverArgv(const FStudioHome4Spec& S,const FString& PythonExecutable,
    const FString& DriverPath,FStudioHome4DriverCommand& Out,FString& Error)
{
    if(!Validate(S,Error))return false;
    if(PythonExecutable.IsEmpty()||DriverPath.IsEmpty()||!Home4TextValid(PythonExecutable)||!Home4TextValid(DriverPath))
    {Error=TEXT("Python executable and driver path must be supplied as bounded arguments.");return false;}
    FStudioHome4DriverCommand C;C.Argv={PythonExecutable,DriverPath};
    const auto D=Derive(S);
    TSet<FString> Handled;
    auto NumberFlag=[&](const TCHAR* SectionName,const TCHAR* Key,const TCHAR* Flag,const TOptional<double>& Value)
    {Handled.Add(FString(SectionName)+TEXT(".")+Key);if(Value.IsSet()){C.Argv.Add(Flag);C.Argv.Add(Home4Number(*Value));}};
    auto IntegerFlag=[&](const TCHAR* Key,const TCHAR* Flag,const TOptional<int64>& Value)
    {Handled.Add(FString(TEXT("run."))+Key);if(Value.IsSet()){C.Argv.Add(Flag);C.Argv.Add(FString::Printf(TEXT("%lld"),*Value));}};
    auto StringFlag=[&](const TCHAR* SectionName,const TCHAR* Key,const TCHAR* Flag,const FString& Value)
    {Handled.Add(FString(SectionName)+TEXT(".")+Key);if(!Value.IsEmpty()){C.Argv.Add(Flag);C.Argv.Add(Value);}};
    auto Switch=[&](const TCHAR* SectionName,const TCHAR* Key,const TCHAR* Flag,const TOptional<bool>& Value)
    {Handled.Add(FString(SectionName)+TEXT(".")+Key);if(Home4IsTrue(Value))C.Argv.Add(Flag);};
    NumberFlag(TEXT("reference"),TEXT("lengthCells"),TEXT("--L"),S.Reference.LengthCells);
    StringFlag(TEXT("geometry"),TEXT("patchClassification"),TEXT("--config"),S.Geometry.PatchClassification);
    NumberFlag(TEXT("reference"),TEXT("froude"),TEXT("--Fn"),S.Reference.Froude.IsSet()?S.Reference.Froude:D.Froude);
    NumberFlag(TEXT("reference"),TEXT("reynolds"),TEXT("--Re_ref"),S.Reference.Reynolds.IsSet()?S.Reference.Reynolds:D.Reynolds);
    NumberFlag(TEXT("fluids"),TEXT("gravity"),TEXT("--g"),D.Gravity);
    NumberFlag(TEXT("fluids"),TEXT("xi"),TEXT("--xi"),D.Xi);
    NumberFlag(TEXT("reference"),TEXT("bond"),TEXT("--Bo"),S.Reference.Bond.IsSet()?S.Reference.Bond:D.Bond);
    NumberFlag(TEXT("fluids"),TEXT("mobility"),TEXT("--mobility"),D.Mobility);
    NumberFlag(TEXT("fluids"),TEXT("rhoLight"),TEXT("--rho_L"),D.RhoLight);
    NumberFlag(TEXT("geometry"),TEXT("sinkCells"),TEXT("--sink_cells"),S.Geometry.SinkCells);
    NumberFlag(TEXT("geometry"),TEXT("trimDegrees"),TEXT("--trim"),S.Geometry.TrimDegrees);
    Switch(TEXT("geometry"),TEXT("noEquilibrate"),TEXT("--no_equilibrate"),S.Geometry.NoEquilibrate);
    NumberFlag(TEXT("lattice"),TEXT("padUp"),TEXT("--pad_up"),S.Lattice.PadUp);
    NumberFlag(TEXT("lattice"),TEXT("padDown"),TEXT("--pad_down"),S.Lattice.PadDown);
    NumberFlag(TEXT("lattice"),TEXT("padSide"),TEXT("--pad_side"),S.Lattice.PadSide);
    NumberFlag(TEXT("lattice"),TEXT("depth"),TEXT("--depth"),S.Lattice.Depth);
    NumberFlag(TEXT("lattice"),TEXT("air"),TEXT("--air"),S.Lattice.Air);
    NumberFlag(TEXT("zones"),TEXT("sponge"),TEXT("--sponge"),S.Zones.Sponge);
    NumberFlag(TEXT("zones"),TEXT("xBeach"),TEXT("--xbeach"),S.Zones.XBeach);
    NumberFlag(TEXT("zones"),TEXT("xBeachStrength"),TEXT("--xbeach_strength"),S.Zones.XBeachStrength);
    NumberFlag(TEXT("zones"),TEXT("beachY"),TEXT("--beach_y"),S.Zones.BeachY);
    NumberFlag(TEXT("zones"),TEXT("beachGap"),TEXT("--beach_gap"),S.Zones.BeachGap);
    NumberFlag(TEXT("zones"),TEXT("zoneStrength"),TEXT("--zone_strength"),S.Zones.ZoneStrength);
    NumberFlag(TEXT("zones"),TEXT("floorFriction"),TEXT("--floor_fric"),S.Zones.FloorFriction);
    StringFlag(TEXT("zones"),TEXT("massCorrection"),TEXT("--mass_correct"),S.Zones.MassCorrection);
    // The appendix names pierce_bc but gives neither choices nor argument arity.
    if(!S.Zones.PierceBoundary.IsEmpty())C.MissingContracts.Add(TEXT("zones.pierceBoundary: --pierce_bc argument arity and mode values are unverified."));
    Handled.Add(TEXT("zones.pierceBoundary"));
    NumberFlag(TEXT("run"),TEXT("travel"),TEXT("--travel"),S.Run.Travel);
    NumberFlag(TEXT("run"),TEXT("rampLength"),TEXT("--ramp_L"),S.Run.RampLength);
    Switch(TEXT("run"),TEXT("noFrameAcceleration"),TEXT("--no_frame_accel"),S.Run.NoFrameAcceleration);
    IntegerFlag(TEXT("steps"),TEXT("--steps"),S.Run.Steps);
    NumberFlag(TEXT("run"),TEXT("averageLength"),TEXT("--avg_L"),S.Run.AverageLength);
    IntegerFlag(TEXT("measureEvery"),TEXT("--measure_every"),S.Run.MeasureEvery);
    IntegerFlag(TEXT("printEvery"),TEXT("--print_every"),S.Run.PrintEvery);
    NumberFlag(TEXT("geometry"),TEXT("bandCells"),TEXT("--band_cells"),S.Geometry.BandCells);
    NumberFlag(TEXT("geometry"),TEXT("refine"),TEXT("--refine"),S.Geometry.Refine);
    StringFlag(TEXT("geometry"),TEXT("sdfBackend"),TEXT("--sdf_backend"),S.Geometry.SdfBackend);
    StringFlag(TEXT("geometry"),TEXT("cptPath"),TEXT("--cpt_path"),S.Geometry.CptPath);
    Switch(TEXT("run"),TEXT("bodyOnCpu"),TEXT("--body_on_cpu"),S.Run.BodyOnCpu);
    Switch(TEXT("run"),TEXT("noGpuKernels"),TEXT("--no_gpu_kernels"),S.Run.NoGpuKernels);
    StringFlag(TEXT("run"),TEXT("device"),TEXT("--device"),S.Run.Device);
    StringFlag(TEXT("run"),TEXT("initState"),TEXT("--init_state"),S.Run.InitState);
    if(!S.Run.SaveState.IsEmpty())C.MissingContracts.Add(TEXT("run.saveState: --save_state is named but its path/cadence argument contract is unverified."));
    Handled.Add(TEXT("run.saveState"));
    StringFlag(TEXT("run"),TEXT("tag"),TEXT("--tag"),S.Run.Tag);
    StringFlag(TEXT("run"),TEXT("outDirectory"),TEXT("--outdir"),S.Run.OutDirectory);
    Switch(TEXT("run"),TEXT("smoke"),TEXT("--smoke"),S.Run.Smoke);
    // UI/provenance fields never become executable flags.
    const TSet<FString> Metadata={TEXT("units.display"),TEXT("run.backend"),TEXT("run.queueTarget"),TEXT("run.extensionImported"),TEXT("run.fallbackConfirmed"),TEXT(".recipeId"),TEXT(".lineageId")};
    const auto O=ToJSON(S);
    for(const auto& F:Fields())
    {
        const FString Path=F.Section+TEXT(".")+F.Key;
        if(Handled.Contains(Path)||Metadata.Contains(Path)||F.Section==TEXT("units")||F.Section==TEXT("performance"))continue;
        const auto V=Home4Section(O,F.Section)->TryGetField(F.Key);
        if(!V||V->Type==EJson::Null||(V->Type==EJson::String&&V->AsString().IsEmpty())||(V->Type==EJson::Array&&V->AsArray().IsEmpty()))continue;
        // Dimensionless requests already encoded by their derived lattice/driver counterparts.
        if(F.Section==TEXT("reference")&&(F.Key==TEXT("mach")||F.Key==TEXT("speedCellsPerStep")||F.Key==TEXT("weber")||F.Key==TEXT("capillary")||F.Key==TEXT("peclet")||F.Key==TEXT("cahn")||F.Key==TEXT("atwood")))
        {
            const bool bMapped=(F.Key==TEXT("cahn")&&D.Xi.IsSet())||(F.Key==TEXT("atwood")&&D.RhoLight.IsSet())||
                (F.Key==TEXT("peclet")&&D.Mobility.IsSet())||((F.Key==TEXT("weber")||F.Key==TEXT("capillary"))&&D.Bond.IsSet())||
                ((F.Key==TEXT("mach")||F.Key==TEXT("speedCellsPerStep"))&&D.Froude.IsSet());
            if(bMapped)continue;
        }
        if(F.Section==TEXT("fluids")&&((F.Key==TEXT("nuHeavy")&&D.Reynolds.IsSet())||(F.Key==TEXT("sigma")&&D.Bond.IsSet())))continue;
        C.MissingContracts.Add(Path+TEXT(": no encoding is established by the run_hull_speed.py appendix."));
    }
    C.MissingContracts.Add(TEXT("Driver parser, revision and default values must be verified against the actual HOME4 solver before launch; this is a command preview."));
    C.Display=ShellDisplay(C.Argv);Out=MoveTemp(C);Error.Empty();return true;
}
