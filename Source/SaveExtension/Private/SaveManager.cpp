// Copyright 2015-2024 Piperift. All Rights Reserved.

#include "SaveManager.h"

#include "SEFileHelpers.h"
#include "SaveExtension.h"
#include "SaveSettings.h"
#include "Serialization/SEDataTask_Load.h"
#include "Serialization/SEDataTask_LoadLevel.h"
#include "Serialization/SEDataTask_Save.h"
#include "Serialization/SEDataTask_SaveLevel.h"

#include <Engine/GameInstance.h>
#include <Engine/GameViewportClient.h>
#include <Engine/LatentActionManager.h>
#include <Engine/LevelStreaming.h>
#include <EngineUtils.h>
#include <GameDelegates.h>
#include <GameFramework/GameModeBase.h>
#include <HighResScreenshot.h>
#include <Kismet/GameplayStatics.h>
#include <LatentActions.h>
#include <Misc/CoreDelegates.h>
#include <Misc/Paths.h>
#include <Tasks/Pipe.h>


// From SaveGameSystem.cpp
void OnAsyncComplete(TFunction<void()> Callback)
{
	// NB. Using Ticker because AsyncTask may run during async package loading which may not be suitable for
	// save data
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Callback = MoveTemp(Callback)](float) -> bool {
			Callback();
			return false;
		}));
}

// BEGIN Async Actions

class FSELoadSlotDataAction : public FPendingLatentAction
{
public:
	ESEContinueOrFail& Result;
	FName ExecutionFunction;
	int32 OutputLink;
	FWeakObjectPtr CallbackTarget;

	FSELoadSlotDataAction(USaveManager* Manager, FName SlotName, ESEContinueOrFail& OutResult,
		const FLatentActionInfo& LatentInfo)
		: Result(OutResult)
		, ExecutionFunction(LatentInfo.ExecutionFunction)
		, OutputLink(LatentInfo.Linkage)
		, CallbackTarget(LatentInfo.CallbackTarget)
	{
		const bool bStarted = Manager->LoadSlot(
			SlotName, FOnGameLoaded::CreateRaw(this, &FSELoadSlotDataAction::OnLoadFinished));
		Result = bStarted ? ESEContinueOrFail::InProgress : ESEContinueOrFail::Failed;
	}
	void UpdateOperation(FLatentResponse& Response) override
	{
		Response.FinishAndTriggerIf(
			Result != ESEContinueOrFail::InProgress, ExecutionFunction, OutputLink, CallbackTarget);
	}
	void OnLoadFinished(USaveSlot* SavedSlot)
	{
		Result = SavedSlot ? ESEContinueOrFail::Continue : ESEContinueOrFail::Failed;
	}
#if WITH_EDITOR
	// Returns a human readable description of the latent operation's current state
	FString GetDescription() const override
	{
		return TEXT("Loading Game...");
	}
#endif
};


class FDeleteAllSlotsAction : public FPendingLatentAction
{
public:
	ESEContinue& Result;
	FName ExecutionFunction;
	int32 OutputLink;
	FWeakObjectPtr CallbackTarget;

	FDeleteAllSlotsAction(USaveManager* Manager, ESEContinue& OutResult, const FLatentActionInfo& LatentInfo)
		: Result(OutResult)
		, ExecutionFunction(LatentInfo.ExecutionFunction)
		, OutputLink(LatentInfo.Linkage)
		, CallbackTarget(LatentInfo.CallbackTarget)
	{
		Result = ESEContinue::InProgress;
		Manager->DeleteAllSlots([this](int32 Count) {
			Result = ESEContinue::Continue;
		});
	}
	void UpdateOperation(FLatentResponse& Response) override
	{
		Response.FinishAndTriggerIf(
			Result != ESEContinue::InProgress, ExecutionFunction, OutputLink, CallbackTarget);
	}
#if WITH_EDITOR
	FString GetDescription() const override
	{
		return TEXT("Deleting all slots...");
	}
#endif
};

class FSEPreloadSlotsAction : public FPendingLatentAction
{
public:
	TArray<USaveSlot*>& Slots;
	ESEContinue& Result;
	FName ExecutionFunction;
	int32 OutputLink;
	FWeakObjectPtr CallbackTarget;

	FSEPreloadSlotsAction(USaveManager* Manager, const bool bSortByRecent, TArray<USaveSlot*>& OutSlots,
		ESEContinue& OutResult, const FLatentActionInfo& LatentInfo)
		: Slots(OutSlots)
		, Result(OutResult)
		, ExecutionFunction(LatentInfo.ExecutionFunction)
		, OutputLink(LatentInfo.Linkage)
		, CallbackTarget(LatentInfo.CallbackTarget)
	{
		Result = ESEContinue::InProgress;
		Manager->PreloadAllSlots(
			[this](const TArray<USaveSlot*>& InSlots) {
				Slots = InSlots;
				Result = ESEContinue::Continue;
			},
			bSortByRecent);
	}
	virtual void UpdateOperation(FLatentResponse& Response) override
	{
		Response.FinishAndTriggerIf(
			Result != ESEContinue::InProgress, ExecutionFunction, OutputLink, CallbackTarget);
	}
#if WITH_EDITOR
	virtual FString GetDescription() const override
	{
		return TEXT("Loading all slots...");
	}
#endif
};


class FSaveGameAction : public FPendingLatentAction
{
public:
	ESEContinueOrFail& Result;
	FName ExecutionFunction;
	int32 OutputLink;
	FWeakObjectPtr CallbackTarget;

	FSaveGameAction(USaveManager* Manager, FName SlotName, bool bOverrideIfNeeded, bool bScreenshot,
		const FScreenshotSize Size, ESEContinueOrFail& OutResult, const FLatentActionInfo& LatentInfo)
		: Result(OutResult)
		, ExecutionFunction(LatentInfo.ExecutionFunction)
		, OutputLink(LatentInfo.Linkage)
		, CallbackTarget(LatentInfo.CallbackTarget)
	{
		const bool bStarted = Manager->SaveSlot(SlotName, bOverrideIfNeeded, bScreenshot, Size,
			FOnGameSaved::CreateRaw(this, &FSaveGameAction::OnSaveFinished));
		Result = bStarted ? ESEContinueOrFail::InProgress : ESEContinueOrFail::Failed;
	}

	virtual void UpdateOperation(FLatentResponse& Response) override
	{
		Response.FinishAndTriggerIf(
			Result != ESEContinueOrFail::InProgress, ExecutionFunction, OutputLink, CallbackTarget);
	}
	void OnSaveFinished(USaveSlot* SavedSlot)
	{
		Result = SavedSlot ? ESEContinueOrFail::Continue : ESEContinueOrFail::Failed;
	}
#if WITH_EDITOR
	// Returns a human readable description of the latent operation's current state
	virtual FString GetDescription() const override
	{
		return TEXT("Saving Game...");
	}
#endif
};

// END Async Actions


USaveManager::USaveManager() : Super() {}

void USaveManager::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	bTickWithGameWorld = GetDefault<USaveSettings>()->bTickWithGameWorld;

	FCoreUObjectDelegates::PreLoadMap.AddUObject(this, &USaveManager::OnMapLoadStarted);
	FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &USaveManager::OnMapLoadFinished);

	AssureActiveSlot();
	if (ActiveSlot && ActiveSlot->bLoadOnStart)
	{
		ReloadActiveSlot();
	}

	UpdateLevelStreamings();
}

void USaveManager::Deinitialize()
{
	Super::Deinitialize();

	FSEFileHelpers::GetPipe().WaitUntilEmpty();

	if (GetActiveSlot()->bSaveOnClose)
		SaveActiveSlot();

	FCoreUObjectDelegates::PreLoadMap.RemoveAll(this);
	FCoreUObjectDelegates::PostLoadMapWithWorld.RemoveAll(this);
	FGameDelegates::Get().GetEndPlayMapDelegate().RemoveAll(this);
}

bool USaveManager::SaveSlot(FName SlotName, bool bOverrideIfNeeded, bool bScreenshot,
	const FScreenshotSize Size, FOnGameSaved OnSaved)
{
	if (!CanLoadOrSave())
		return false;

	if (SlotName.IsNone())
	{
		SELog(ActiveSlot, "Can't use an empty slot name to save.", true);
		return false;
	}

	// Saving
	SELog(ActiveSlot, "Saving to Slot " + SlotName.ToString());

	UWorld* World = GetWorld();
	check(World);

	// Launch task, always fail if it didn't finish or wasn't scheduled
	auto& Task = CreateTask<FSEDataTask_Save>()
					 .Setup(SlotName, bOverrideIfNeeded, bScreenshot, Size.Width, Size.Height)
					 .Bind(OnSaved)
					 .Start();

	return Task.IsSucceeded() || Task.IsScheduled();
}

bool USaveManager::SaveSlot(const USaveSlot* Slot, bool bOverrideIfNeeded, bool bScreenshot,
	const FScreenshotSize Size, FOnGameSaved OnSaved)
{
	if (!Slot)
	{
		return false;
	}
	return SaveSlot(Slot->Name, bOverrideIfNeeded, bScreenshot, Size, OnSaved);
}

bool USaveManager::SaveActiveSlot(bool bScreenshot, const FScreenshotSize Size, FOnGameSaved OnSaved)
{
	return SaveSlot(ActiveSlot, true, bScreenshot, Size, OnSaved);
}

bool USaveManager::LoadSlot(FName SlotName, FOnGameLoaded OnLoaded)
{
	if (!CanLoadOrSave() || !IsSlotSaved(SlotName))
	{
		return false;
	}

	AssureActiveSlot();

	auto& Task = CreateTask<FSEDataTask_Load>().Setup(SlotName).Bind(OnLoaded).Start();
	return Task.IsSucceeded() || Task.IsScheduled();
}

bool USaveManager::LoadSlot(const USaveSlot* Slot, FOnGameLoaded OnLoaded)
{
	if (!Slot)
	{
		return false;
	}
	return LoadSlot(Slot->Name, OnLoaded);
}

void USaveManager::PreloadAllSlots(FSEOnAllSlotsPreloaded Callback, bool bSortByRecent)
{
	FSEFileHelpers::GetPipe().Launch(UE_SOURCE_LOCATION, [this, Callback, bSortByRecent]() {
		TArray<USaveSlot*> Slots;
		PreloadAllSlotsSync(Slots, bSortByRecent);

		if (Callback)
		{
			OnAsyncComplete([Slots = MoveTemp(Slots), Callback]() {
				for (auto* Slot : Slots)
				{
					Slot->ClearInternalFlags(EInternalObjectFlags::Async);
				}
				Callback(Slots);
			});
		}
	});
}

void USaveManager::PreloadAllSlotsSync(TArray<USaveSlot*>& Slots, bool bSortByRecent)
{
	TArray<FString> FileNames;
	FSEFileHelpers::FindAllFilesSync(FileNames);

	TArray<FSaveFile> LoadedFiles;
	LoadedFiles.Reserve(FileNames.Num());
	for (const FString& FileName : FileNames)
	{
		// Load all files
		FScopedFileReader Reader(FSEFileHelpers::GetSlotPath(FileName));
		if (Reader.IsValid())
		{
			LoadedFiles.AddDefaulted_GetRef().Read(Reader, true);
		}
	}

	Slots.Reserve(Slots.Num() + LoadedFiles.Num());
	for (const auto& File : LoadedFiles)
	{
		auto* Slot =
			Cast<USaveSlot>(FSEFileHelpers::DeserializeObject(nullptr, File.ClassName, this, File.Bytes));
		if (Slot)
		{
			Slots.Add(Slot);
		}
	}

	if (bSortByRecent)
	{
		Slots.Sort([](const USaveSlot& A, const USaveSlot& B) {
			return A.Stats.SaveDate > B.Stats.SaveDate;
		});
	}
}

bool USaveManager::DeleteSlotByNameSync(FName SlotName)
{
	const FString NameStr = SlotName.ToString();
	return FSEFileHelpers::DeleteFile(NameStr);
}

void USaveManager::DeleteSlotByName(FName SlotName)
{
	FSEFileHelpers::GetPipe().Launch(UE_SOURCE_LOCATION, [this, SlotName]() {
		DeleteSlotByNameSync(SlotName);
	});
}

int32 USaveManager::DeleteAllSlotsSync()
{
	TArray<FString> FoundSlots;
	FSEFileHelpers::FindAllFilesSync(FoundSlots);

	int32 Count = 0;
	for (const FString& SlotName : FoundSlots)
	{
		Count += FSEFileHelpers::DeleteFile(SlotName);
	}
	return Count;
}

void USaveManager::DeleteAllSlots(FSEOnAllSlotsDeleted Callback)
{
	FSEFileHelpers::GetPipe().Launch(UE_SOURCE_LOCATION, [this, Callback]() {
		const int32 Count = DeleteAllSlotsSync();
		if (Callback)
		{
			OnAsyncComplete([Count, Callback]() {
				Callback(Count);
			});
		}
	});
}

void USaveManager::BPSaveSlotByName(FName SlotName, bool bScreenshot, const FScreenshotSize Size,
	ESEContinueOrFail& Result, struct FLatentActionInfo LatentInfo, bool bOverrideIfNeeded /*= true*/)
{
	if (UWorld* World = GetWorld())
	{
		FLatentActionManager& LatentActionManager = World->GetLatentActionManager();
		if (LatentActionManager.FindExistingAction<FSaveGameAction>(
				LatentInfo.CallbackTarget, LatentInfo.UUID) == nullptr)
		{
			LatentActionManager.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID,
				new FSaveGameAction(
					this, SlotName, bOverrideIfNeeded, bScreenshot, Size, Result, LatentInfo));
		}
		return;
	}
	Result = ESEContinueOrFail::Failed;
}

void USaveManager::BPLoadSlotByName(
	FName SlotName, ESEContinueOrFail& Result, struct FLatentActionInfo LatentInfo)
{
	if (UWorld* World = GetWorld())
	{
		FLatentActionManager& LatentActionManager = World->GetLatentActionManager();
		if (LatentActionManager.FindExistingAction<FSELoadSlotDataAction>(
				LatentInfo.CallbackTarget, LatentInfo.UUID) == nullptr)
		{
			LatentActionManager.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID,
				new FSELoadSlotDataAction(this, SlotName, Result, LatentInfo));
		}
		return;
	}
	Result = ESEContinueOrFail::Failed;
}

void USaveManager::BPPreloadAllSlots(const bool bSortByRecent, TArray<USaveSlot*>& SaveInfos,
	ESEContinue& Result, struct FLatentActionInfo LatentInfo)
{
	if (UWorld* World = GetWorld())
	{
		FLatentActionManager& LatentActionManager = World->GetLatentActionManager();
		if (LatentActionManager.FindExistingAction<FSEPreloadSlotsAction>(
				LatentInfo.CallbackTarget, LatentInfo.UUID) == nullptr)
		{
			LatentActionManager.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID,
				new FSEPreloadSlotsAction(this, bSortByRecent, SaveInfos, Result, LatentInfo));
		}
	}
}

void USaveManager::BPDeleteAllSlots(ESEContinue& Result, struct FLatentActionInfo LatentInfo)
{
	if (UWorld* World = GetWorld())
	{
		FLatentActionManager& LatentActionManager = World->GetLatentActionManager();
		if (LatentActionManager.FindExistingAction<FDeleteAllSlotsAction>(
				LatentInfo.CallbackTarget, LatentInfo.UUID) == nullptr)
		{
			LatentActionManager.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID,
				new FDeleteAllSlotsAction(this, Result, LatentInfo));
		}
	}
}

USaveSlot* USaveManager::PreloadSlot(FName SlotName)
{
	USaveSlot* Slot = nullptr;
	const FString NameStr = SlotName.ToString();
	Slot = FSEFileHelpers::LoadFileSync(NameStr, nullptr, true, this);
	return Slot;
}

bool USaveManager::IsSlotSaved(FName SlotName) const
{
	return FSEFileHelpers::FileExists(SlotName.ToString());
}

bool USaveManager::CanLoadOrSave()
{
	const AGameModeBase* GameMode = UGameplayStatics::GetGameMode(this);
	if (GameMode && !GameMode->HasAuthority())
	{
		return false;
	}

	return IsValid(GetWorld());
}

void USaveManager::SetActiveSlot(USaveSlot* NewSlot)
{
	ActiveSlot = NewSlot;
	// TODO: Ensure data is not null here
}

void USaveManager::AssureActiveSlot(TSubclassOf<USaveSlot> ActiveSlotClass, bool bForced)
{
	if (HasActiveSlot() && !bForced)
		return;

	if (!ActiveSlotClass)
	{
		ActiveSlotClass = GetDefault<USaveSettings>()->ActiveSlot.Get();
		if (!ActiveSlotClass)
		{
			ActiveSlotClass = USaveSlot::StaticClass();
		}
	}
	SetActiveSlot(NewObject<USaveSlot>(this, ActiveSlotClass));
}

void USaveManager::UpdateLevelStreamings()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const TArray<ULevelStreaming*>& Levels = World->GetStreamingLevels();

	LevelStreamingNotifiers.Empty(Levels.Num());	  // Avoid memory deallocation
	LevelStreamingNotifiers.Reserve(Levels.Num());	  // Reserve extra memory
	for (auto* Level : Levels)
	{
		ULevelStreamingNotifier* Notifier = NewObject<ULevelStreamingNotifier>(this);
		Notifier->SetLevelStreaming(Level);
		Notifier->OnLevelShown().BindUFunction(
			this, GET_FUNCTION_NAME_CHECKED(USaveManager, DeserializeStreamingLevel));
		Notifier->OnLevelHidden().BindUFunction(
			this, GET_FUNCTION_NAME_CHECKED(USaveManager, SerializeStreamingLevel));
		LevelStreamingNotifiers.Add(Notifier);
	}
}

void USaveManager::SerializeStreamingLevel(ULevelStreaming* LevelStreaming)
{
	if (!LevelStreaming->GetLoadedLevel()->bIsBeingRemoved)
	{
		CreateTask<FSEDataTask_SaveLevel>().Setup(LevelStreaming).Start();
	}
}

void USaveManager::DeserializeStreamingLevel(ULevelStreaming* LevelStreaming)
{
	CreateTask<FSEDataTask_LoadLevel>().Setup(LevelStreaming).Start();
}

void USaveManager::FinishTask(FSEDataTask* Task)
{
	Tasks.RemoveAll([Task](auto& TaskPtr) {
		return TaskPtr.Get() == Task;
	});

	// Start next task
	if (Tasks.Num() > 0)
	{
		Tasks[0]->Start();
	}
}

bool USaveManager::IsLoading() const
{
	return HasTasks() && Tasks[0]->Type == ESETaskType::Load;
}

void USaveManager::Tick(float DeltaTime)
{
	if (Tasks.Num())
	{
		FSEDataTask* Task = Tasks[0].Get();
		check(Task);
		if (Task->IsRunning())
		{
			Task->Tick(DeltaTime);
		}
	}
}

void USaveManager::SubscribeForEvents(const TScriptInterface<ISaveExtensionInterface>& Interface)
{
	SubscribedInterfaces.AddUnique(Interface);
}

void USaveManager::UnsubscribeFromEvents(const TScriptInterface<ISaveExtensionInterface>& Interface)
{
	SubscribedInterfaces.Remove(Interface);
}

void USaveManager::OnSaveBegan()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USaveManager::OnSaveBegan);

	// TODO: Needs reworking
	FSELevelFilter Filter;
	IterateSubscribedInterfaces([&Filter](auto* Object) {
		check(Object->template Implements<USaveExtensionInterface>());

		// C++ event
		if (ISaveExtensionInterface* Interface = Cast<ISaveExtensionInterface>(Object))
		{
			Interface->OnSaveBegan(Filter);
		}
		ISaveExtensionInterface::Execute_ReceiveOnSaveBegan(Object, Filter);
	});
}

void USaveManager::OnSaveFinished(const bool bError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USaveManager::OnSaveFinished);

	// TODO: Needs reworking
	FSELevelFilter Filter;
	IterateSubscribedInterfaces([&Filter, bError](auto* Object) {
		check(Object->template Implements<USaveExtensionInterface>());

		// C++ event
		if (ISaveExtensionInterface* Interface = Cast<ISaveExtensionInterface>(Object))
		{
			Interface->OnSaveFinished(Filter, bError);
		}
		ISaveExtensionInterface::Execute_ReceiveOnSaveFinished(Object, Filter, bError);
	});

	if (!bError)
	{
		OnGameSaved.Broadcast(ActiveSlot);
	}
}

void USaveManager::OnLoadBegan()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USaveManager::OnLoadBegan);
	
	FSELevelFilter Filter;
	IterateSubscribedInterfaces([&Filter](auto* Object) {
		check(Object->template Implements<USaveExtensionInterface>());

		// C++ event
		if (ISaveExtensionInterface* Interface = Cast<ISaveExtensionInterface>(Object))
		{
			Interface->OnLoadBegan(Filter);
		}
		ISaveExtensionInterface::Execute_ReceiveOnLoadBegan(Object, Filter);
	});
}

void USaveManager::OnLoadFinished(const bool bError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USaveManager::OnLoadFinished);

	FSELevelFilter Filter;
	IterateSubscribedInterfaces([&Filter, bError](auto* Object) {
		check(Object->template Implements<USaveExtensionInterface>());

		// C++ event
		if (ISaveExtensionInterface* Interface = Cast<ISaveExtensionInterface>(Object))
		{
			Interface->OnLoadFinished(Filter, bError);
		}
		ISaveExtensionInterface::Execute_ReceiveOnLoadFinished(Object, Filter, bError);
	});

	if (!bError)
	{
		OnGameLoaded.Broadcast(ActiveSlot);
	}
}

void USaveManager::OnMapLoadStarted(const FString& MapName)
{
	SELog(ActiveSlot, "Loading Map '" + MapName + "'", FColor::Purple);
}

void USaveManager::OnMapLoadFinished(UWorld* LoadedWorld)
{
	if (IsLoading())
	{
		static_cast<FSEDataTask_Load*>(Tasks[0].Get())->OnMapLoaded();
	}

	UpdateLevelStreamings();
}

UWorld* USaveManager::GetWorld() const
{
	check(GetGameInstance());

	// If we are a CDO, we must return nullptr instead to fool UObject::ImplementsGetWorld.
	if (HasAllFlags(RF_ClassDefaultObject))
		return nullptr;

	return GetGameInstance()->GetWorld();
}

inline void USaveManager::BPSaveSlot(const USaveSlot* Slot, bool bScreenshot, const FScreenshotSize Size,
	ESEContinueOrFail& Result, struct FLatentActionInfo LatentInfo, bool bOverrideIfNeeded)
{
	if (!Slot)
	{
		Result = ESEContinueOrFail::Failed;
		return;
	}
	BPSaveSlotByName(Slot->Name, bScreenshot, Size, Result, MoveTemp(LatentInfo), bOverrideIfNeeded);
}

void USaveManager::BPLoadSlot(const USaveSlot* Slot, ESEContinueOrFail& Result, FLatentActionInfo LatentInfo)
{
	if (!Slot)
	{
		Result = ESEContinueOrFail::Failed;
		return;
	}
	BPLoadSlotByName(Slot->Name, Result, MoveTemp(LatentInfo));
}

void USaveManager::IterateSubscribedInterfaces(TFunction<void(UObject*)>&& Callback)
{
	for (const TScriptInterface<ISaveExtensionInterface>& Interface : SubscribedInterfaces)
	{
		if (UObject* const Object = Interface.GetObject())
		{
			Callback(Object);
		}
	}
}

USaveManager* USaveManager::Get(const UWorld* World)
{
	if (World)
	{
		return UGameInstance::GetSubsystem<USaveManager>(World->GetGameInstance());
	}
	return nullptr;
}
USaveManager* USaveManager::Get(const UObject* Context)
{
	return USaveManager::Get(
		GEngine->GetWorldFromContextObject(Context, EGetWorldErrorMode::LogAndReturnNull));
}

bool USaveManager::IsTickable() const
{
	return !HasAnyFlags(RF_ClassDefaultObject) && IsValid(this);
}

UWorld* USaveManager::GetTickableGameObjectWorld() const
{
	return bTickWithGameWorld ? GetWorld() : nullptr;
}

TStatId USaveManager::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USaveManager, STATGROUP_Tickables);
}
