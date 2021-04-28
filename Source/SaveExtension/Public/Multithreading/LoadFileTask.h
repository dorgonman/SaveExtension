// Copyright 2015-2020 Piperift. All Rights Reserved.

#pragma once

#include <Async/AsyncWork.h>
#include "FileAdapter.h"


/////////////////////////////////////////////////////
// FLoadFileTask
// Async task to load a File
class FLoadFileTask : public FNonAbandonableTask
{
protected:

	TWeakObjectPtr<USaveManager> Manager;
	const FString SlotName;

	TWeakObjectPtr<USlotInfo> SlotInfo;
	TWeakObjectPtr<USlotData> SlotData;


public:

	explicit FLoadFileTask(USaveManager* Manager, FStringView SlotName)
		: Manager(Manager)
		, SlotName(SlotName)
	{}
	~FLoadFileTask()
	{
		if(SlotInfo.IsValid())
		{
			SlotInfo->ClearInternalFlags(EInternalObjectFlags::Async);
			SlotInfo->RemoveFromRoot();
		}
		if(SlotData.IsValid())
		{
			SlotData->ClearInternalFlags(EInternalObjectFlags::Async);
			SlotData->RemoveFromRoot();
		}
	}

	void DoWork()
	{
		FScopedFileReader FileReader(FFileAdapter::GetSlotPath(SlotName));
		if(FileReader.IsValid())
		{
			FSaveFile File;
			File.Read(FileReader, false);
			SlotInfo = File.CreateAndDeserializeInfo(Manager.Get());
			SlotInfo->AddToRoot();
			SlotData = File.CreateAndDeserializeData(Manager.Get());
			SlotData->AddToRoot();
		}
	}

	USlotInfo* GetInfo()
	{
		return SlotInfo.Get();
	}

	USlotData* GetData()
	{
		return SlotData.Get();
	}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FLoadFileTask, STATGROUP_ThreadPoolAsyncTasks);
	}
};
