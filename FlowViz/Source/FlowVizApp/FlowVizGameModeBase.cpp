// Copyright FlowViz contributors. All Rights Reserved.

#include "FlowVizGameModeBase.h"

#include "FlowVizCameraPawn.h"

#include "Engine/Engine.h"
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

	// QUOTED: a case path containing a space -- ordinary on macOS -- would
	// otherwise split at the console tokenizer, and the tail would arrive as a
	// bogus field id. FParse::Token (which IConsoleManager uses to build the
	// command's Args) treats a double-quoted run as one token and strips the
	// quotes, so the command body receives the full path unchanged.
	FString Command = FString::Printf(TEXT("FlowViz.LoadCase \"%s\""), *CasePath);
	if (!FieldId.IsEmpty())
	{
		Command += TEXT(" ") + FieldId;
	}

	GetWorldTimerManager().SetTimerForNextTick(
		FTimerDelegate::CreateWeakLambda(this, [this, Command]()
		{
			if (GEngine != nullptr)
			{
				GEngine->Exec(GetWorld(), *Command);
			}
		}));
}
