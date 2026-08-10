// Copyright FlowViz contributors. All Rights Reserved.

#include "FlowVizGameModeBase.h"

#include "FlowVizCameraPawn.h"

#include "Engine/Engine.h"
#include "UI/FlowVizConsoleCommands.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "TimerManager.h"

AFlowVizGameModeBase::AFlowVizGameModeBase()
{
	// No tick logic anywhere in this class; the one-shot load is a timer.
	PrimaryActorTick.bCanEverTick = false;

	DefaultPawnClass = AFlowVizCameraPawn::StaticClass();
}

void AFlowVizGameModeBase::BeginPlay()
{
	Super::BeginPlay();

	FString CasePath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("case="), CasePath) || CasePath.IsEmpty())
	{
		return;
	}

	// A relative path means "from where the user launched", not from inside
	// the .app bundle -- resolve it before the working directory can lie.
	if (FPaths::IsRelative(CasePath))
	{
		CasePath = FPaths::Combine(FPaths::LaunchDir(), CasePath);
	}

	FString FieldId;
	FParse::Value(FCommandLine::Get(), TEXT("field="), FieldId);

	const FString Command = FlowVizConsoleCommands::MakeLoadCaseCommand(CasePath, FieldId);

	GetWorldTimerManager().SetTimerForNextTick(
		FTimerDelegate::CreateWeakLambda(this, [this, Command]()
		{
			if (GEngine != nullptr)
			{
				GEngine->Exec(GetWorld(), *Command);
			}
		}));
}
