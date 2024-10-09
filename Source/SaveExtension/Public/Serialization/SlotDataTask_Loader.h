// Copyright 2015-2020 Piperift. All Rights Reserved.

#pragma once

#include "Delegates.h"
#include "ISaveExtension.h"
#include "Multithreading/LoadFileTask.h"
#include "SavePreset.h"
#include "SlotInfo.h"
#include "SlotData.h"
#include "SlotDataTask.h"

#include <Engine/Level.h>
#include <Engine/LevelStreaming.h>
#include <GameFramework/Actor.h>
#include <Engine/LevelScriptActor.h>
#include <GameFramework/Controller.h>

#include "SlotDataTask_Loader.generated.h"


enum class ELoadDataTaskState : uint8
{
	NotStarted,

	// Once loading starts we either load the map
	LoadingMap,
	WaitingForData,

	RestoringActors,
	Deserializing
};

/**
* Manages the loading process of a SaveData file
*/
UCLASS()
class USlotDataTask_Loader : public USlotDataTask
{
	GENERATED_BODY()

	FName SlotName;

	UPROPERTY()
	USlotInfo* NewSlotInfo;

	FOnGameLoaded Delegate;

protected:

	// Async variables
	TWeakObjectPtr<ULevel> CurrentLevel;
	TWeakObjectPtr<ULevelStreaming> CurrentSLevel;

	int32 CurrentActorIndex = 0;
	TArray<TWeakObjectPtr<AActor>> CurrentLevelActors;

	/** Start AsyncTasks */
	FAsyncTask<FLoadFileTask>* LoadDataTask;
	/** End AsyncTasks */

	ELoadDataTaskState LoadState = ELoadDataTaskState::NotStarted;


public:

	USlotDataTask_Loader() : Super() {}

	auto Setup(FName InSlotName)
	{
		SlotName = InSlotName;
		return this;
	}

	auto Bind(const FOnGameLoaded& OnLoaded) { Delegate = OnLoaded; return this; }

	void OnMapLoaded();

private:

	virtual void OnStart() override;

	virtual void Tick(float DeltaTime) override;
	virtual void OnFinish(bool bSuccess) override;
	virtual void BeginDestroy() override;

	void StartDeserialization();

	/** Spawns Actors hat were saved but which actors are not in the world. */
	void RespawnActors(const TArray<FActorRecord*>& Records, const ULevel* Level);
	void RespawnAndDeserializeGameplayFramework() ;
protected:

	//~ Begin Files
	void StartLoadingData();

	USlotData* GetLoadedData() const;
	const bool IsDataLoaded() const;
	//~ End Files


	/** BEGIN Deserialization */
	void BeforeDeserialize();
	void DeserializeSync();
	void DeserializeLevelSync(const ULevel* Level, const ULevelStreaming* StreamingLevel = nullptr);

	void DeserializeASync();
	void DeserializeLevelASync(ULevel* Level, ULevelStreaming* StreamingLevel = nullptr);

	virtual void DeserializeASyncLoop(float StartMS = 0.0f);

	void FinishedDeserializing();

	void PrepareAllLevels(bool bSkipGameplayFramework, bool bRespawnActor);
	void PrepareLevel(const ULevel* Level, FLevelRecord& LevelRecord, bool bRespawnActor);

	/** Deserializes all Level actors. */
	void DeserializeLevel_Actor(AActor* const Actor, const FLevelRecord& LevelRecord, const FSELevelFilter& Filter);

	void FindNextAsyncLevel(ULevelStreaming*& OutLevelStreaming) const;

private:

	/** Deserializes Game Instance Object and its Properties.
	Requires 'SaveGameMode' flag to be used. */
	void DeserializeGameInstance();

	/** Serializes an actor into this Actor Record */
	bool DeserializeActor(AActor* Actor, const FActorRecord& Record, const FSELevelFilter& Filter);

	/** Deserializes the components of an actor from a provided Record */
	void DeserializeActorComponents(AActor* Actor, const FActorRecord& ActorRecord, const FSELevelFilter& Filter, int8 indent = 0);
	/** END Deserialization */

	void Deserialize_RepNotify(UObject* InObject);
	TArray<TWeakObjectPtr<UObject>> AllDeserializedObject;
	//TArray<TWeakObjectPtr<AActor>> AllDeferredRespawnedActors;
};