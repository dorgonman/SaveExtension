// Copyright 2015-2024 Piperift. All Rights Reserved.

#include "Serialization/SEDataTask_SaveLevel.h"

#include "SaveExtension.h"


/////////////////////////////////////////////////////
// FSaveDataTask_LevelSaver

void FSEDataTask_SaveLevel::OnStart()
{
	if (SlotData && StreamingLevel && StreamingLevel->IsLevelLoaded())
	{
		FLevelRecord* LevelRecord = FindLevelRecord(*SlotData, StreamingLevel);
		if (!LevelRecord)
		{
			Finish(false);
			return;
		}

		PrepareLevel(StreamingLevel->GetLoadedLevel(), *LevelRecord);

		SerializeLevel(StreamingLevel->GetLoadedLevel(), StreamingLevel);

		Finish(true);
		return;
	}
	Finish(false);
}

void FSEDataTask_SaveLevel::OnFinish(bool bSuccess)
{
	SELog(Slot, "Finished Serializing level", FColor::Green);
}
