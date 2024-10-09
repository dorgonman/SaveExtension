// Copyright 2015-2020 Piperift. All Rights Reserved.

#include "LevelFilter.h"
#include <GameFramework/GameModeBase.h>
#include <GameFramework/GameSession.h>
#include <GameFramework/GameStateBase.h>
#include <GameFramework/Pawn.h>
#include <GameFramework/PlayerController.h>
#include <GameFramework/PlayerState.h>
#include <WorldPartition/DataLayer/WorldDataLayers.h>
#include <WorldPartition/WorldPartitionReplay.h>

// GameplayDebugger
#include <GameplayDebuggerCategoryReplicator.h>




/////////////////////////////////////////////////////
// USaveDataTask

const FName FSELevelFilter::TagNoTransform { "!SaveTransform"  };
const FName FSELevelFilter::TagNoPhysics   { "!SavePhysics"    };
const FName FSELevelFilter::TagNoTags      { "!SaveTags"       };
const FName FSELevelFilter::TagTransform   { "SaveTransform"   };

bool FSELevelFilter::ShouldSave(const AActor* Actor) const 
{
	bool bResult = IsValid(Actor) && (Actor->GetIsReplicated() || ActorFilter.IsClassAllowed(Actor->GetClass()));

	// Skip Controller and Pawn if they have a PlayerState, should be handled by PlayerState
	if (Cast<AController>(Actor))
	{
		auto pController = Cast<AController>(Actor);
		if (pController->PlayerState)
		{
			bResult = false;
		}
	}
	else if (Cast<APawn>(Actor))
	{
		auto pPawn = Cast<APawn>(Actor);
		if (pPawn->GetPlayerState())
		{
			bResult = false;
		}
	}
	else if (Cast<AWorldSettings>(Actor))
	{
		bResult = false;
	}
	else if (Cast<AGameplayDebuggerCategoryReplicator>(Actor))
	{
		bResult = false;
	}
	else if (Cast<AWorldPartitionReplay>(Actor)) 
	{
		bResult = false;
	}
	else if (Cast<AWorldDataLayers>(Actor)) 
	{
		bResult = false;
	}
	return bResult;
}

bool FSELevelFilter::ShouldLoad(const AActor* Actor) const 
{
	bool bResult =
		IsValid(Actor) &&
			   (Actor->GetIsReplicated() || LoadActorFilter.IsClassAllowed(Actor->GetClass()));

	// Skip Controller and Pawn if they have a PlayerState, should be handled by PlayerState
	if (Cast<AController>(Actor))
	{
		auto pController = Cast<AController>(Actor);
		if (pController->PlayerState)
		{
			bResult = false;
		}
	}
	else if (Cast<APawn>(Actor))
	{
		auto pPawn = Cast<APawn>(Actor);
		if (pPawn->GetPlayerState())
		{
			bResult = false;
		}
	}
	else if (Cast<AWorldSettings>(Actor))
	{
		bResult = false;
	}
	else if (Cast<AGameplayDebuggerCategoryReplicator>(Actor))
	{
		bResult = false;
	}
	else if (Cast<AWorldPartitionReplay>(Actor))
	{
		bResult = false;
	}
	else if (Cast<AWorldDataLayers>(Actor))
	{
		bResult = false;
	}
	
	return bResult;
}
