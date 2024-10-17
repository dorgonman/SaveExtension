// Copyright 2015-2020 Piperift. All Rights Reserved.

#include "Serialization/SlotDataTask_Loader.h"

#include <Camera/PlayerCameraManager.h>
#include <Components/PrimitiveComponent.h>
#include <Engine/AssetManager.h>
#include <GameFramework/Character.h>
#include <GameFramework/GameModeBase.h>
#include <GameFramework/GameSession.h>
#include <GameFramework/GameStateBase.h>
#include <GameFramework/HUD.h>
#include <GameFramework/PlayerController.h>
#include <GameFramework/PlayerState.h>
#include <GameFramework/SpectatorPawn.h>
#include <GameFramework/WorldSettings.h>
#include <Kismet/GameplayStatics.h>
#include <Serialization/MemoryReader.h>
#include <UObject/UObjectGlobals.h>
#include <WorldPartition/DataLayer/WorldDataLayers.h>

// Core
#include <Misc/OutputDeviceNull.h>

// GameplayDebugger
#include <GameplayDebuggerCategoryReplicator.h>

#include "Misc/SlotHelpers.h"
#include "SavePreset.h"
#include "SaveManager.h"
#include "Serialization/SEArchive.h"






/////////////////////////////////////////////////////
// Helpers

namespace Loader
{
	static int32 RemoveSingleRecordPtrSwap(TArray<FActorRecord*>& Records, AActor* Actor, bool bAllowShrinking = true)
	{
		if(!Actor)
		{
			return 0;
		}

		const int32 I = Records.IndexOfByPredicate([Records, Actor](auto* Record)
		{
			return *Record == Actor;
		});
		if (I != INDEX_NONE)
		{
			Records.RemoveAtSwap(I, 1, bAllowShrinking);
			return 1;
		}
		return 0;
	}
}


/////////////////////////////////////////////////////
// USaveDataTask_Loader

void USlotDataTask_Loader::OnStart()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USlotDataTask_Loader::OnStart);
	USaveManager* Manager = GetManager();
	SELog(Preset, "Loading from Slot " + SlotName.ToString());

	NewSlotInfo = Manager->LoadInfo(SlotName);
	if (!NewSlotInfo)
	{
		SELog(Preset, "Slot Info not found! Can't load.", FColor::White, true, 1);
		Finish(false);
		return;
	}

	// We load data while the map opens or GC runs
	StartLoadingData();

	const UWorld* World = GetWorld();

	// Cross-Level loading
	// TODO: Handle empty Map as empty world
	FName CurrentMapName { FSlotHelpers::GetWorldName(World) };
	if (CurrentMapName != NewSlotInfo->Map)
	{
		LoadState = ELoadDataTaskState::LoadingMap;
		FString MapToOpen = NewSlotInfo->Map.ToString();
		if (!GEngine->MakeSureMapNameIsValid(MapToOpen))
		{
			UE_LOG(LogSaveExtension, Warning, TEXT("Slot '%s' was saved in map '%s' but it did not exist while loading. Corrupted save file?"), *NewSlotInfo->FileName.ToString(), *MapToOpen);
			Finish(false);
			return;
		}
		bool bIsHostingServer = false;
		auto pWorld = GetWorld();
		do
		{
			if (!pWorld) break;
			ENetMode netMode = pWorld->GetNetMode();
			bIsHostingServer = (netMode == NM_DedicatedServer || 
							    netMode == NM_ListenServer || 
								netMode == NM_Client);
		} while (0);


		if (Manager->OnOpenLevelBeforeLoadGame.IsBound() || Manager->OnOpenLevelBeforeLoadGameNative.IsBound()) 
		{
			Manager->OnOpenLevelBeforeLoadGame.Broadcast(MapToOpen, bIsHostingServer);
			Manager->OnOpenLevelBeforeLoadGameNative.Broadcast(MapToOpen, bIsHostingServer);
		}
		else
		{
			FString mapOption = FString::Printf(TEXT("FromLoadGame"));
			if (bIsHostingServer)
			{
				FString hostOption = TEXT("listen");
				if (!hostOption.IsEmpty())
				{
					mapOption = FString::Printf(TEXT("%s?%s"), *mapOption, *hostOption);
				}
				UGameplayStatics::OpenLevel(this, FName{MapToOpen}, true, mapOption);
			}
			else
			{
				UGameplayStatics::OpenLevel(this, FName{MapToOpen}, true, mapOption);
			}
		}

		SELog(Preset, "Slot '" + SlotName.ToString() + "' is recorded on another Map. Loading before charging slot.", FColor::White, false, 1);
		return;
	}
	else if (IsDataLoaded())
	{
		StartDeserialization();
	}
	else
	{
		LoadState = ELoadDataTaskState::WaitingForData;
	}
}

void USlotDataTask_Loader::Tick(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USlotDataTask_Loader::Tick);
	switch(LoadState)
	{
	case ELoadDataTaskState::Deserializing:
		if (CurrentLevel.IsValid())
		{
			DeserializeASyncLoop();
		}
		break;

	case ELoadDataTaskState::WaitingForData:
		if (IsDataLoaded())
		{
			StartDeserialization();
		}
	}
}

void USlotDataTask_Loader::OnFinish(bool bSuccess)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USlotDataTask_Loader::OnFinish);
	if (bSuccess)
	{
		SELog(Preset, "Finished Loading", FColor::Green);
	}
	for (auto& DeserializedObject : AllDeserializedObject)
	{
		Deserialize_RepNotify(DeserializedObject.Get());
	}
	AllDeserializedObject.Empty();


	// Execute delegates
	Delegate.ExecuteIfBound((bSuccess) ? NewSlotInfo : nullptr);

	GetManager()->OnLoadFinished(
		SlotData? GetGeneralFilter() : FSELevelFilter{},
		!bSuccess
	);
}

void USlotDataTask_Loader::BeginDestroy()
{
	if (LoadDataTask)
	{
		LoadDataTask->EnsureCompletion(false);
		delete LoadDataTask;
	}

	Super::BeginDestroy();
}

void USlotDataTask_Loader::OnMapLoaded()
{
	if(LoadState != ELoadDataTaskState::LoadingMap)
	{
		return;
	}

	const UWorld* World = GetWorld();
	if(!World)
	{
		UE_LOG(LogSaveExtension, Warning, TEXT("Failed loading map from saved slot."));
		Finish(false);
	}
	const FName NewMapName { FSlotHelpers::GetWorldName(World) };
	if (NewMapName == NewSlotInfo->Map)
	{
		if(IsDataLoaded())
		{
			StartDeserialization();
		}
		else
		{
			LoadState = ELoadDataTaskState::WaitingForData;
		}
	}
}

void USlotDataTask_Loader::StartDeserialization()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USlotDataTask_Loader::StartDeserialization);
	check(NewSlotInfo);

	LoadState = ELoadDataTaskState::Deserializing;

	SlotData = GetLoadedData();
	if (!SlotData)
	{
		// Failed to load data
		Finish(false);
		return;
	}

	NewSlotInfo->LoadDate = FDateTime::Now();

	GetManager()->OnLoadBegan(GetGeneralFilter());
	//Apply current Info if succeeded
	GetManager()->__SetCurrentInfo(NewSlotInfo);

	BakeAllFilters();

	BeforeDeserialize();

	if (Preset->IsFrameSplitLoad())
		DeserializeASync();
	else
		DeserializeSync();
}

void USlotDataTask_Loader::StartLoadingData()
{
	LoadDataTask = new FAsyncTask<FLoadFileTask>(GetManager(), SlotName.ToString());

	if (Preset->IsMTFilesLoad())
		LoadDataTask->StartBackgroundTask();
	else
		LoadDataTask->StartSynchronousTask();
}

USlotData* USlotDataTask_Loader::GetLoadedData() const
{
	if (IsDataLoaded())
	{
		return LoadDataTask->GetTask().GetData();
	}
	return nullptr;
}

const bool USlotDataTask_Loader::IsDataLoaded() const 
{
	bool bIsDone = false;
	do 
	{
		if (!LoadDataTask) break;

		if (!LoadDataTask->IsDone()) break;
		if (!UAssetManager::GetStreamableManager().AreAllAsyncLoadsComplete())
			break;
		auto& streamingLevels = GetWorld()->GetStreamingLevels();
		bool bAllLevelsLoaded = true;
		for (auto& streamingLevel : streamingLevels)
		{
			if (!streamingLevel->IsLevelLoaded())
			{
				bAllLevelsLoaded = false;
				break;
			}
		}
		if (!bAllLevelsLoaded) break;
		bIsDone = true;
	} while (0);

	return bIsDone;//LoadDataTask && LoadDataTask->IsDone();
}

void USlotDataTask_Loader::BeforeDeserialize()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USlotDataTask_Loader::BeforeDeserialize);
	UWorld* World = GetWorld();

	// Set current game time to the saved value
	World->TimeSeconds = SlotData->TimeSeconds;

	if (SlotData->bStoreGameInstance)
	{
		DeserializeGameInstance();
	}
}

void USlotDataTask_Loader::DeserializeSync()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USlotDataTask_Loader::DeserializeSync);

	const UWorld* World = GetWorld();
	check(World);

	SELog(Preset, "World '" + World->GetName() + "'", FColor::Green, false, 1);

	PrepareAllLevels();

	// Deserialize world
	{
		DeserializeLevelSync(World->GetCurrentLevel());

		const TArray<ULevelStreaming*>& Levels = World->GetStreamingLevels();
		for (const ULevelStreaming* Level : Levels)
		{
			if (Level->IsLevelLoaded())
			{
				DeserializeLevelSync(Level->GetLoadedLevel(), Level);
			}
		}
	}

	FinishedDeserializing();
}

void USlotDataTask_Loader::DeserializeLevelSync(const ULevel* Level, const ULevelStreaming* StreamingLevel)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USlotDataTask_Loader::DeserializeLevelSync);

	if (!IsValid(Level))
		return;

	const FName LevelName = StreamingLevel ? StreamingLevel->GetWorldAssetPackageFName() : FPersistentLevelRecord::PersistentName;
	SELog(Preset, "Level '" + LevelName.ToString() + "'", FColor::Green, false, 1);

	if (FLevelRecord* LevelRecord = FindLevelRecord(StreamingLevel))
	{
		const auto& Filter = GetLevelFilter(*LevelRecord);

		for (auto ActorItr = Level->Actors.CreateConstIterator(); ActorItr; ++ActorItr)
		{
			TObjectPtr<AActor> Actor = *ActorItr;
			if (IsValid(Actor) && Filter.ShouldSave(Actor))
			{
				DeserializeLevel_Actor(Actor, *LevelRecord, Filter);
			}
		}
	}
}

void USlotDataTask_Loader::DeserializeASync()
{
	// Deserialize world
	{
		SELog(Preset, "World '" + GetWorld()->GetName() + "'", FColor::Green, false, 1);

		PrepareAllLevels();
		DeserializeLevelASync(GetWorld()->GetCurrentLevel());
	}
}

void USlotDataTask_Loader::DeserializeLevelASync(ULevel* Level, ULevelStreaming* StreamingLevel)
{
	check(IsValid(Level));

	const FName LevelName = StreamingLevel ? StreamingLevel->GetWorldAssetPackageFName() : FPersistentLevelRecord::PersistentName;
	SELog(Preset, "Level '" + LevelName.ToString() + "'", FColor::Green, false, 1);

	FLevelRecord* LevelRecord = FindLevelRecord(StreamingLevel);
	if (!LevelRecord) {
		Finish(false);
		return;
	}

	const float StartMS = GetTimeMilliseconds();

	CurrentLevel = Level;
	CurrentSLevel = StreamingLevel;
	CurrentActorIndex = 0;

	// Copy actors array. New actors won't be considered for deserialization
	CurrentLevelActors.Empty(Level->Actors.Num());
	for (TObjectPtr<AActor> Actor : Level->Actors)
	{
		if(IsValid(Actor))
		{
			CurrentLevelActors.Add(Actor);
		}
	}

	DeserializeASyncLoop(StartMS);
}

void USlotDataTask_Loader::DeserializeASyncLoop(float StartMS)
{
	FLevelRecord * LevelRecord = FindLevelRecord(CurrentSLevel.Get());
	if (!LevelRecord)
	{
		return;
	}

	const auto& Filter = GetLevelFilter(*LevelRecord);

	if(StartMS <= 0)
	{
		StartMS = GetTimeMilliseconds();
	}

	// Continue Iterating actors every tick
	for (; CurrentActorIndex < CurrentLevelActors.Num(); ++CurrentActorIndex)
	{
		AActor* const Actor{ CurrentLevelActors[CurrentActorIndex].Get() };
		if (IsValid(Actor) && Filter.ShouldSave(Actor))
		{
			DeserializeLevel_Actor(Actor, *LevelRecord, Filter);

			const float CurrentMS = GetTimeMilliseconds();
			// If x milliseconds passed, stop and continue on next frame
			if (CurrentMS - StartMS >= MaxFrameMs)
			{
				return;
			}
		}
	}

	ULevelStreaming* CurrentLevelStreaming = CurrentSLevel.Get();
	FindNextAsyncLevel(CurrentLevelStreaming);
	if (CurrentLevelStreaming)
	{
		// Iteration has ended. Deserialize next level
		CurrentLevel = CurrentLevelStreaming->GetLoadedLevel();
		if (CurrentLevel.IsValid())
		{
			DeserializeLevelASync(CurrentLevel.Get(), CurrentLevelStreaming);
			return;
		}
	}

	// All levels deserialized
	FinishedDeserializing();
}

void USlotDataTask_Loader::PrepareLevel(const ULevel* Level, FLevelRecord& LevelRecord)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USlotDataTask_Loader::PrepareLevel);

	const auto& Filter = GetLevelFilter(LevelRecord);


	// Records not contained in Scene Actors		 => Actors to be Respawned
	// Scene Actors not contained in loaded records  => Actors to be Destroyed
	// The rest									     => Just deserialize

	TArray<FActorRecord*> ActorsToSpawn;
	ActorsToSpawn.Reserve(LevelRecord.Actors.Num());
	for(FActorRecord& Record : LevelRecord.Actors)
	{
		ActorsToSpawn.Add(&Record);
	}
	AGameStateBase* ExistingGameState = nullptr;

	TArray<APlayerState*> PlayerStates;
	{
		// O(M*Log(N))
		for (AActor* const Actor : Level->Actors)
		{
			// skip GameStateBase, PlayeState and Controller
			if (Actor && (Cast<AGameStateBase>(Actor) ||
						  Cast<APlayerState>(Actor) ||
						  Cast<ALevelScriptActor>(Actor) ) ||
						  Cast<AController>(Actor))
			{
				if (auto pGameState = Cast<AGameStateBase>(Actor))
				{
					ExistingGameState = pGameState;
				}
				else if (auto pPlayerState = Cast<APlayerState>(Actor))
				{
					PlayerStates.Add(pPlayerState);
				}
				continue;
			}

			if (Cast<APawn>(Actor))
			{
				auto Pawn = Cast<APawn>(Actor);
				if (auto PlayerState = Pawn->GetPlayerState())
				{
					// Skip Player controlled Pawns
					continue;
				}
			}
			// Remove records which actors do exist
			const bool bFoundActorRecord = Loader::RemoveSingleRecordPtrSwap(ActorsToSpawn, Actor, false) > 0;

			if (Actor && Filter.ShouldSave(Actor))
			{
				if (!bFoundActorRecord) // Don't destroy level actors
				{
					// If the actor wasn't found, mark it for destruction
					Actor->Destroy();
				}
			}
		}
		ActorsToSpawn.Shrink();
	}

	// Create Actors that doesn't exist now but were saved
	RespawnActors(ActorsToSpawn, Level);


	if (GetWorld()->GetCurrentLevel() == Level)
	{

		FActorSpawnParameters SpawnInfo{};
		SpawnInfo.OverrideLevel = const_cast<ULevel*>(Level);
		SpawnInfo.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
		// Respawn PlayerState and PlayerController
		for (auto& PlayerStateRecord : SlotData->PlayerStateRecords)
		{
			bool bActivePlayerState = false;
			APlayerState* DeserializedPlayerState = nullptr;
			for (APlayerState* PlayerState : PlayerStates)
			{
				if (PlayerStateRecord.UniqueId == PlayerState->GetUniqueId().ToString())
				{
					DeserializedPlayerState = PlayerState;
					bActivePlayerState = true;
					// DeserializeActor(PlayerState, PlayerStateRecord, Filter);
					ensure(DeserializedPlayerState->Rename(
						*PlayerStateRecord.Name.ToString(), DeserializedPlayerState->GetOuter()));
					break;
				}
			}
			if (!DeserializedPlayerState)
			{
				SpawnInfo.Name = PlayerStateRecord.Name;
				DeserializedPlayerState = Cast<APlayerState>(
					GetWorld()->SpawnActor(PlayerStateRecord.SoftClassPath.TryLoadClass<APlayerState>(),
						&PlayerStateRecord.Transform, SpawnInfo));
				ensure(DeserializedPlayerState);
			}
			AController* DeserializedController = nullptr;

			if (DeserializedPlayerState)
			{
				DeserializedController = DeserializedPlayerState->GetOwningController();
			}


			FPlayerControllerRecord* PlayerControllerRecord =
				SlotData->PlayerControllerRecords.FindByKey(PlayerStateRecord.UniqueId);
			ensure(PlayerControllerRecord);
			if (PlayerControllerRecord)
			{
				if (DeserializedController)
				{
					ensure(DeserializedController->Rename(
						*PlayerControllerRecord->Name.ToString(), DeserializedController->GetOuter()));
				}
				else
				{
					SpawnInfo.Name = PlayerControllerRecord->Name;
					DeserializedController = Cast<AController>(GetWorld()->SpawnActor(
						PlayerControllerRecord->SoftClassPath.TryLoadClass<AController>(),
						&PlayerControllerRecord->Transform, SpawnInfo));
					ensure(DeserializedController);
				}
			}

			APawn* DeserializedPawn = nullptr;
			if (DeserializedController)
			{
				DeserializedPawn = DeserializedController->GetPawn();
			}
			FPlayerControlleredPawnRecord* PlayerControlleredPawnRecord =
				SlotData->PlayerControlleredPawnRecords.FindByKey(PlayerStateRecord.UniqueId);
			ensure(PlayerControlleredPawnRecord);
			if (PlayerControlleredPawnRecord)
			{
				if (DeserializedPawn)
				{
					if (DeserializedPawn->GetFName() != PlayerControlleredPawnRecord->Name) 
					{
						ensure(DeserializedPawn->Rename(
							*PlayerControlleredPawnRecord->Name.ToString(), DeserializedPawn->GetOuter()));
					}
				}
				else
				{
					SpawnInfo.Name = PlayerControlleredPawnRecord->Name;
					DeserializedPawn = Cast<APawn>(GetWorld()->SpawnActor(
						PlayerControlleredPawnRecord->SoftClassPath.TryLoadClass<APawn>(),
						&PlayerControlleredPawnRecord->Transform, SpawnInfo));
					ensure(DeserializedPawn);
				}
			}


			if (DeserializedPlayerState)
			{
				DeserializeActor(DeserializedPlayerState, PlayerStateRecord, Filter);
			}

			if (DeserializedController && PlayerControllerRecord)
			{
				DeserializeActor(DeserializedController, *PlayerControllerRecord, Filter);
			}

			if (DeserializedPawn && PlayerControlleredPawnRecord)
			{
				DeserializeActor(DeserializedPawn, *PlayerControlleredPawnRecord, Filter);
			}

			if (DeserializedPawn && DeserializedController)
			{
				// Hotfix Serialization Children
				auto OldControlleredPawn = DeserializedController->GetPawn();
				if (OldControlleredPawn) 
				{
					DeserializedController->Children.AddUnique(OldControlleredPawn);
				}
				//if (!IsValid(DeserializedController->GetPawn()) ||
				//	(DeserializedController->GetPawn()->GetName() != DeserializedPawn->GetName()))
				//{
					DeserializedController->Possess(DeserializedPawn);
				//}	
			}
			if (!bActivePlayerState)
			{
				// Mark controller and Associaed PlayerState for destruction,
				// it will call AGameMode::AddInactivePlayer eventually
				// so rejoined players can be repossessed
				DeserializedController->Destroy();
			}


		}

		ensure(ExistingGameState);
		if (ExistingGameState)
		{
			ensure(ExistingGameState->Rename(
				*SlotData->GameStateRecord.Name.ToString(), ExistingGameState->GetOuter()));
			DeserializeActor(ExistingGameState, SlotData->GameStateRecord, Filter);
		}
		auto pLevelScriptActor = GetWorld()->GetCurrentLevel()->GetLevelScriptActor();
		DeserializeActor(pLevelScriptActor, LevelRecord.LevelScript, Filter);
	}
	
}

void USlotDataTask_Loader::FinishedDeserializing()
{
	// Clean serialization data
	SlotData->CleanRecords(false);
	GetManager()->__SetCurrentData(SlotData);

	Finish(true);
}

void USlotDataTask_Loader::PrepareAllLevels()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USlotDataTask_Loader::PrepareAllLevels);

	const UWorld* World = GetWorld();
	check(World);

	// Prepare Main level
	PrepareLevel(World->GetCurrentLevel(), SlotData->MainLevel);

	// Prepare other loaded sub-levels
	const TArray<ULevelStreaming*>& Levels = World->GetStreamingLevels();
	for (const ULevelStreaming* Level : Levels)
	{
		if (Level->IsLevelLoaded())
		{
			FLevelRecord* LevelRecord = FindLevelRecord(Level);
			if (LevelRecord)
			{
				PrepareLevel(Level->GetLoadedLevel(), *LevelRecord);
			}
		}
	}
}

void USlotDataTask_Loader::RespawnActors(const TArray<FActorRecord*>& Records, const ULevel* Level)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USlotDataTask_Loader::RespawnActors);

	FActorSpawnParameters SpawnInfo{};
	SpawnInfo.OverrideLevel = const_cast<ULevel*>(Level);
	SpawnInfo.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	UWorld* World = GetWorld();
	ULevel* MutableLevel = const_cast<ULevel*>(Level);

	// Respawn all procedural actors
	for (auto* Record : Records)
	{
		SpawnInfo.Name = Record->Name;
		UE_LOG(LogSaveExtension, Log, TEXT("RespawnActor: %s"), *SpawnInfo.Name.ToString());
		auto* NewActor =
			World->SpawnActor(Record->SoftClassPath.TryLoadClass<UObject>(), &Record->Transform, SpawnInfo);
		ensure(NewActor);
		if (NewActor) 
		{
			// We update the name on the record in case it changed
			Record->Name = NewActor->GetFName();
		}
		//if (MutableLevel && World->GetCurrentLevel() == MutableLevel) 
		{
			auto NewWorldSetting = Cast<AWorldSettings>(NewActor);
			if (NewWorldSetting) 
			{
				MutableLevel->SetWorldSettings(NewWorldSetting);
			}
			
			auto NewDataLayers = Cast<AWorldDataLayers>(NewActor);
			if (NewDataLayers) 
			{
				MutableLevel->SetWorldDataLayers(NewDataLayers);
			}
		}
	}
}

void USlotDataTask_Loader::DeserializeLevel_Actor(AActor* const Actor, const FLevelRecord& LevelRecord, const FSELevelFilter& Filter)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USlotDataTask_Loader::DeserializeLevel_Actor);

	// Find the record
	const FActorRecord* const Record = LevelRecord.Actors.FindByKey(Actor);
	if (Record && Record->IsValid() && Record->SoftClassPath.TryLoadClass<AActor>() == Actor->GetClass())
	{
		DeserializeActor(Actor, *Record, Filter);
	}
}

void USlotDataTask_Loader::DeserializeGameInstance()
{
	bool bSuccess = true;
	auto* GameInstance = GetWorld()->GetGameInstance();
	const FObjectRecord& Record = SlotData->GameInstance;

	if (!IsValid(GameInstance) || GameInstance->GetClass() != Record.SoftClassPath.TryLoadClass<UGameInstance>())
		bSuccess = false;

	if (bSuccess)
	{
		//Serialize from Record Data
		FMemoryReader MemoryReader(Record.Data, true);
		FSEArchive Archive(MemoryReader, false);
		GameInstance->Serialize(Archive);
	}

	SELog(Preset, "Game Instance '" + Record.Name.ToString() + "'", FColor::Green, !bSuccess, 1);
}

bool USlotDataTask_Loader::DeserializeActor(AActor* Actor, const FActorRecord& Record, const FSELevelFilter& Filter)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USlotDataTask_Loader::DeserializeActor);

	// Always load saved tags
	Actor->Tags = Record.Tags;

	const bool bSavesPhysics = FSELevelFilter::StoresPhysics(Actor);
	if (FSELevelFilter::StoresTransform(Actor))
	{
		Actor->SetActorTransform(Record.Transform);

		if (FSELevelFilter::StoresPhysics(Actor))
		{
			USceneComponent* Root = Actor->GetRootComponent();
			if (auto* Primitive = Cast<UPrimitiveComponent>(Root))
			{
				Primitive->SetPhysicsLinearVelocity(Record.LinearVelocity);
				Primitive->SetPhysicsAngularVelocityInRadians(Record.AngularVelocity);
			}
			else
			{
				Root->ComponentVelocity = Record.LinearVelocity;
			}
		}
	}

	Actor->SetActorHiddenInGame(Record.bHiddenInGame);

	DeserializeActorComponents(Actor, Record, Filter, 2);

	{
		//Serialize from Record Data
		FMemoryReader MemoryReader(Record.Data, true);
		FSEArchive Archive(MemoryReader, false);
		Actor->Serialize(Archive);
		UE_LOG(LogSaveExtension, Log, TEXT("DeserializeActor %s"), *Actor->GetName());
		AllDeserializedObject.AddUnique(Actor);
		//Deserialize_RepNotify(Actor);
	}

	return true;
}

void USlotDataTask_Loader::DeserializeActorComponents(AActor* Actor, const FActorRecord& ActorRecord, const FSELevelFilter& Filter, int8 Indent)
{
	if (Filter.bStoreComponents)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UUSlotDataTask_Loader::DeserializeActorComponents);

		const TSet<UActorComponent*>& Components = Actor->GetComponents();
		for (auto* Component : Components)
		{
			if (!Filter.ShouldSave(Component))
			{
				continue;
			}

			// Find the record
			const FComponentRecord* Record = ActorRecord.ComponentRecords.FindByKey(Component);
			if (!Record)
			{
				SELog(Preset, "Component '" + Component->GetFName().ToString() + "' - Record not found", FColor::Red, false, Indent + 1);
				continue;
			}

			if (FSELevelFilter::StoresTransform(Component))
			{
				USceneComponent* Scene = CastChecked<USceneComponent>(Component);
				if (Scene->Mobility == EComponentMobility::Movable)
				{
					Scene->SetRelativeTransform(Record->Transform);
				}
			}

			if (FSELevelFilter::StoresTags(Component))
			{
				Component->ComponentTags = Record->Tags;
			}

			if (!Component->GetClass()->IsChildOf<UPrimitiveComponent>())
			{
				FMemoryReader MemoryReader(Record->Data, true);
				FSEArchive Archive(MemoryReader, false);
				Component->Serialize(Archive);
				UE_LOG(LogSaveExtension, Log, TEXT("DeserializeActorComponent %s.%s"), 
					*Component->GetOwner()->GetName(), *Component->GetName());
				AllDeserializedObject.AddUnique(Component);
			}
			
		}
	}
}

void USlotDataTask_Loader::Deserialize_RepNotify(UObject* InObject)
{
	ensure(InObject);
	if (InObject) 
	{
		auto MutableActor =Cast<AActor>(InObject);
		if (MutableActor)
		{
			MutableActor->GatherCurrentMovement();
		}
		for (TFieldIterator<FProperty> PropIt(InObject->GetClass(), EFieldIteratorFlags::IncludeSuper);
			 PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			if (Property->HasAnyPropertyFlags(CPF_RepNotify))
			{
				//if (!(Property->RepNotifyFunc == TEXT("OnRep_AttachmentReplication"))) 
				{
					FOutputDeviceNull ar;
					bool bSuccess = InObject->CallFunctionByNameWithArguments(
						*Property->RepNotifyFunc.ToString(), ar, NULL, true);
				}
			}
		}
	}
	
}

void USlotDataTask_Loader::FindNextAsyncLevel(ULevelStreaming*& OutLevelStreaming) const
{
	OutLevelStreaming = nullptr;

	const UWorld* World = GetWorld();
	const TArray<ULevelStreaming*>& Levels = World->GetStreamingLevels();
	if (CurrentLevel.IsValid() && Levels.Num() > 0)
	{
		if (!CurrentSLevel.IsValid())
		{
			//Current is persistent, get first streaming level
			OutLevelStreaming = Levels[0];
			return;
		}
		else
		{
			int32 CurrentIndex = Levels.IndexOfByKey(CurrentSLevel);
			if (CurrentIndex != INDEX_NONE && Levels.Num() > CurrentIndex + 1)
			{
				OutLevelStreaming = Levels[CurrentIndex + 1];
			}
		}
	}

	// If this level is unloaded, find next
	if (OutLevelStreaming && !OutLevelStreaming->IsLevelLoaded())
	{
		FindNextAsyncLevel(OutLevelStreaming);
	}
}
