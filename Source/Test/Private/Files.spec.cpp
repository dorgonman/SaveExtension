// Copyright 2015-2024 Piperift. All Rights Reserved.

#include "Automatron.h"
#include "Helpers/TestActor.h"

#include <SEFileHelpers.h>
#include <SaveManager.h>


class FSaveSpec_Files : public Automatron::FTestSpec
{
	GENERATE_SPEC(FSaveSpec_Files, "SaveExtension.Files",
		EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::ProductFilter);

	USaveManager* SaveManager = nullptr;
	ATestActor* TestActor = nullptr;

	// Helper for some test delegates
	bool bFinishTick = false;

	FSaveSpec_Files() : Automatron::FTestSpec()
	{
		bReuseWorldForAllTests = false;
		bCanUsePIEWorld = false;
	}
};


void FSaveSpec_Files::Define()
{
	BeforeEach([this]() {
		SaveManager = USaveManager::Get(GetMainWorld());
		TestNotNull(TEXT("SaveManager"), SaveManager);

		SaveManager->bTickWithGameWorld = true;

		SaveManager->GetActiveSlot()->MultithreadedSerialization = ESEAsyncMode::SaveAndLoadSync;
	});

	It("Can save files synchronously", [this]() {
		SaveManager->GetActiveSlot()->MultithreadedFiles = ESEAsyncMode::SaveAndLoadSync;

		TestTrue("Saved", SaveManager->SaveSlot(0));

		TestTrue("Info File exists in disk", FSEFileHelpers::FileExists(TEXT("0")));
	});

	It("Can save files asynchronously", [this]() {
		SaveManager->GetActiveSlot()->MultithreadedFiles = ESEAsyncMode::SaveAsync;
		bFinishTick = false;

		bool bSaving =
			SaveManager->SaveSlot(0, true, false, {}, FOnGameSaved::CreateLambda([this](auto* Info) {
				// Notified that files have been saved asynchronously
				TestTrue("Info File exists in disk", FSEFileHelpers::FileExists(TEXT("0")));
				bFinishTick = true;
			}));
		TestTrue("Started Saving", bSaving);

		// Files shouldn't exist yet
		TestFalse("Info File exists in disk", FSEFileHelpers::FileExists(TEXT("0")));

		TickWorldUntil(GetMainWorld(), true, [this](float) {
			return !bFinishTick;
		});
	});

	It("Can load files synchronously", [this]() {
		SaveManager->GetActiveSlot()->MultithreadedFiles = ESEAsyncMode::SaveAndLoadSync;

		TestTrue("Saved", SaveManager->SaveSlot(0));

		USaveSlot* Slot = FSEFileHelpers::LoadFileSync(TEXT("0"), nullptr, true, SaveManager);
		TestNotNull("Slot is valid", Slot);
		TestNotNull("Data is valid", Slot->GetData());
	});

	AfterEach([this]() {
		if (SaveManager)
		{
			bFinishTick = false;
			SaveManager->DeleteAllSlots([this](int32 Count) {
				bFinishTick = true;
			});
			TickWorldUntil(GetMainWorld(), true, [this](float) {
				return !bFinishTick;
			});
		}
		SaveManager = nullptr;
	});
}
