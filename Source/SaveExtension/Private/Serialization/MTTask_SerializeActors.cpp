// Copyright 2015-2020 Piperift. All Rights Reserved.

#include "Serialization/MTTask_SerializeActors.h"
#include <Serialization/MemoryWriter.h>
#include <Components/PrimitiveComponent.h>
#include <GameFramework/PlayerState.h>
#include <GameFramework/GameStateBase.h>
#include <GameFramework/PlayerController.h>
#include <GameFramework/Pawn.h>
#include <GameFramework/GameSession.h>
#include <GameFramework/GameModeBase.h>

#include "SaveManager.h"
#include "SlotInfo.h"
#include "SlotData.h"
#include "SavePreset.h"
#include "Serialization/SEArchive.h"





	/////////////////////////////////////////////////////
// FMTTask_SerializeActors
void FMTTask_SerializeActors::DoWork()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FMTTask_SerializeActors::DoWork);
	if (bStoreGameInstance)
	{
		SerializeGameInstance();
	}
	for (int32 I = 0; I < Num; ++I)
	{
		const AActor* const Actor = (*LevelActors)[StartIndex + I];

		if (Cast<ALevelScriptActor>(Actor))
		{
			SerializeActor(Actor, LevelScriptRecord);
		}
		else if (Cast<AGameStateBase>(Actor)) 
		{
			SerializeActor(Actor, GameStateRecord);
		}
		else if (Cast<APlayerState>(Actor)) 
		{
			auto PlayerState = Cast<APlayerState>(Actor);
			FPlayerStateRecord& PlayerStateRecord = PlayerStateRecords.AddDefaulted_GetRef();
			// AI PlayerStates don't have a unique id
			if (PlayerState->GetUniqueId().IsValid())
			{
				PlayerStateRecord.UniqueId = PlayerState->GetUniqueId().ToString();
			}
			else 
			{
				PlayerStateRecord.UniqueId = PlayerState->GetPlayerName();
			}
			SerializeActor(Actor, PlayerStateRecord);

			auto pOwningController = PlayerState->GetOwningController();
			if (pOwningController)
			{
				FPlayerControllerRecord& ControllerRecord = PlayerControllerRecords.AddDefaulted_GetRef();
				ControllerRecord.UniqueId = PlayerStateRecord.UniqueId;
				SerializeActor(pOwningController, ControllerRecord);
			}
			auto Pawn = PlayerState->GetPawn();
			if (Pawn)
			{
				FPlayerControlleredPawnRecord& PawnRecord = PlayerControlleredPawnRecords.AddDefaulted_GetRef();
				PawnRecord.UniqueId = PlayerStateRecord.UniqueId;
				SerializeActor(Pawn, PawnRecord);
			}
		}
		else if (Actor && Filter.ShouldSave(Actor))
		{
			bool bShouldSave = true;

			// Skip Controller and Pawn if they have a PlayerState, should be handled by PlayerState
			if (Cast<AController>(Actor)) 
			{
				auto pController = Cast<AController>(Actor);
				if (pController->PlayerState)
				{
					bShouldSave = false;
				}
			}
			else if (Cast<APawn>(Actor))
			{
				auto pPawn = Cast<APawn>(Actor);
				if (pPawn->GetPlayerState())
				{
					bShouldSave = false;
				}
			}

			
			if (bShouldSave) 
			{
				FActorRecord& Record = ActorRecords.AddDefaulted_GetRef();
				SerializeActor(Actor, Record);
			}

		}
	}
}

void FMTTask_SerializeActors::DumpData() 
{
	if (LevelScriptRecord.IsValid())
	{
		LevelRecord->LevelScript = LevelScriptRecord;
	}

	// Shrink not needed. Move wont keep reserved space
	LevelRecord->Actors.Append(MoveTemp(ActorRecords));
	if (GameStateRecord.IsValid())
	{
		SlotData->GameStateRecord = MoveTemp(GameStateRecord);
	}
	SlotData->PlayerControllerRecords.Append(MoveTemp(PlayerControllerRecords));
	SlotData->PlayerStateRecords.Append(MoveTemp(PlayerStateRecords));
	SlotData->PlayerControlleredPawnRecords.Append(MoveTemp(PlayerControlleredPawnRecords));
}

void FMTTask_SerializeActors::SerializeGameInstance()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FMTTask_SerializeActors::SerializeGameInstance);
	if (UGameInstance* GameInstance = World->GetGameInstance())
	{
		FObjectRecord Record{ GameInstance };

		//Serialize into Record Data
		FMemoryWriter MemoryWriter(Record.Data, true);
		FSEArchive Archive(MemoryWriter, false);
		GameInstance->Serialize(Archive);

		SlotData->GameInstance = MoveTemp(Record);
	}
}

bool FMTTask_SerializeActors::SerializeActor(const AActor* Actor, FActorRecord& Record) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FMTTask_SerializeActors::SerializeActor);

	//Clean the record
	Record = { Actor };

	Record.bHiddenInGame = Actor->IsHidden();
	Record.bIsProcedural = Filter.IsProcedural(Actor);

	if (Filter.StoresTags(Actor))
	{
		Record.Tags = Actor->Tags;
	}
	else
	{
		// Only save save-tags
		for (const auto& Tag : Actor->Tags)
		{
			if (Filter.IsSaveTag(Tag))
			{
				Record.Tags.Add(Tag);
			}
		}
	}

	if (Filter.StoresTransform(Actor))
	{
		Record.Transform = Actor->GetTransform();

		if (Filter.StoresPhysics(Actor))
		{
			USceneComponent* const Root = Actor->GetRootComponent();
			if (Root && Root->Mobility == EComponentMobility::Movable)
			{
				if (auto* const Primitive = Cast<UPrimitiveComponent>(Root))
				{
					Record.LinearVelocity = Primitive->GetPhysicsLinearVelocity();
					Record.AngularVelocity = Primitive->GetPhysicsAngularVelocityInRadians();
				}
				else
				{
					Record.LinearVelocity = Root->GetComponentVelocity();
				}
			}
		}
	}

	if (Filter.bStoreComponents)
	{
		SerializeActorComponents(Actor, Record, 1);
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(Serialize);
	FMemoryWriter MemoryWriter(Record.Data, true);
	FSEArchive Archive(MemoryWriter, false);
	const_cast<AActor*>(Actor)->Serialize(Archive);

	return true;
}

void FMTTask_SerializeActors::SerializeActorComponents(const AActor* Actor, FActorRecord& ActorRecord, int8 Indent /*= 0*/) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FMTTask_SerializeActors::SerializeActorComponents);

	const TSet<UActorComponent*>& Components = Actor->GetComponents();
	for (auto* Component : Components)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FMTTask_SerializeActors::SerializeActorComponents|Component);
		if (Filter.ShouldSave(Component))
		{
			FComponentRecord ComponentRecord;
			ComponentRecord.Name = Component->GetFName();
			ComponentRecord.SoftClassPath = Component->GetClass();

			if (Filter.StoresTransform(Component))
			{
				const USceneComponent* Scene = CastChecked<USceneComponent>(Component);
				if (Scene->Mobility == EComponentMobility::Movable)
				{
					ComponentRecord.Transform = Scene->GetRelativeTransform();
				}
			}

			if (Filter.StoresTags(Component))
			{
				ComponentRecord.Tags = Component->ComponentTags;
			}

			if (!Component->GetClass()->IsChildOf<UPrimitiveComponent>())
			{
				FMemoryWriter MemoryWriter(ComponentRecord.Data, true);
				FSEArchive Archive(MemoryWriter, false);
				Component->Serialize(Archive);
			}
			ActorRecord.ComponentRecords.Add(ComponentRecord);
		}
	}
}
