// Copyright 2015-2024 Piperift. All Rights Reserved.

#pragma once

#include "SEDataTask.h"

#include <Engine/Level.h>
#include <Engine/LevelScriptActor.h>
#include <Engine/LevelStreaming.h>
#include <GameFramework/Actor.h>
#include <GameFramework/Controller.h>
#include <Tasks/Task.h>


class USaveManager;
class USaveSlot;
class USaveSlotData;


/** Called when game has been loaded
 * @param SaveSlot the loaded slot. Null if load failed
 */
DECLARE_DELEGATE_OneParam(FOnGameLoaded, USaveSlot*);


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
struct FSEDataTask_Load : public FSEDataTask
{
protected:
	FName SlotName;

	TObjectPtr<USaveSlot> Slot;
	TObjectPtr<USaveSlotData> SlotData;
	float MaxFrameMs = 0.f;
	FSEClassFilter SubsystemFilter;

	FOnGameLoaded Delegate;

protected:
	// Async variables
	TWeakObjectPtr<ULevel> CurrentLevel;
	TWeakObjectPtr<ULevelStreaming> CurrentSLevel;

	int32 CurrentActorIndex = 0;
	TArray<TWeakObjectPtr<AActor>> CurrentLevelActors;

	UE::Tasks::TTask<USaveSlot*> LoadFileTask;

	ELoadDataTaskState LoadState = ELoadDataTaskState::NotStarted;


public:
	FSEDataTask_Load(USaveManager* Manager, USaveSlot* Slot);
	~FSEDataTask_Load();

	auto& Setup(FName InSlotName)
	{
		SlotName = InSlotName;
		return *this;
	}

	auto& Bind(const FOnGameLoaded& OnLoaded)
	{
		Delegate = OnLoaded;
		return *this;
	}

	void OnMapLoaded();

private:
	virtual void OnStart() override;

	virtual void Tick(float DeltaTime) override;
	virtual void OnFinish(bool bSuccess) override;

	void StartDeserialization();

	/** Spawns Actors hat were saved but which actors are not in the world. */
	void RespawnActors(const TArray<FActorRecord*>& Records, const ULevel* Level, FLevelRecord& LevelRecord);

protected:
	void StartLoadingFile();
	bool CheckFileLoaded();

	/** BEGIN Deserialization */
	void BeforeDeserialize();
	void DeserializeSync();
	void DeserializeLevelSync(const ULevel* Level, const ULevelStreaming* StreamingLevel = nullptr);

	void DeserializeASync();
	void DeserializeLevelASync(ULevel* Level, ULevelStreaming* StreamingLevel = nullptr);

	virtual void DeserializeASyncLoop(float StartMS = 0.0f);

	void FinishedDeserializing();

	void PrepareAllLevels();
	void PrepareLevel(const ULevel* Level, FLevelRecord& LevelRecord);

	void FindNextAsyncLevel(ULevelStreaming*& OutLevelStreaming) const;
	/** END Deserialization */
};