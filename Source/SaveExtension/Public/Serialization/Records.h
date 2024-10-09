// Copyright 2015-2020 Piperift. All Rights Reserved.


#pragma once

#include <CoreMinimal.h>
#include <Engine/LevelStreaming.h>
#include <Engine/LevelScriptActor.h>

#include <GameFramework/PlayerState.h>
#include <GameFramework/PlayerController.h>
#include <GameFramework/Pawn.h>
#include <GameFramework/GameStateBase.h>

#include "Records.generated.h"



// SaveExtension
class USlotData;



USTRUCT()
struct FBaseRecord
{
	GENERATED_BODY()

	FName Name;


	FBaseRecord() : Name() {}

	virtual bool Serialize(FArchive& Ar);
	friend FArchive& operator<<(FArchive& Ar, FBaseRecord& Record)
	{
		Record.Serialize(Ar);
		return Ar;
	}
	virtual ~FBaseRecord() {}
};

template<>
struct TStructOpsTypeTraits<FBaseRecord> : public TStructOpsTypeTraitsBase2<FBaseRecord>
{ enum { WithSerializer = true }; };

FORCEINLINE bool operator==(const FBaseRecord& A, const FBaseRecord& B) { return A.Name == B.Name; }


/** Represents a serialized Object */
USTRUCT()
struct FObjectRecord : public FBaseRecord
{
	GENERATED_BODY()

	//UPROPERTY()
	//TObjectPtr<UClass> Class;
	FSoftClassPath SoftClassPath;

	TArray<uint8> Data;
	TArray<FName> Tags;


	FObjectRecord() : Super() {}
	FObjectRecord(const UObject* Object);

	virtual bool Serialize(FArchive& Ar) override;

	bool IsValid() const
	{
		return !Name.IsNone() && SoftClassPath.IsValid() && Data.Num() > 0;
	}

	FORCEINLINE bool operator== (const UObject* Other) const
	{
		return Other && Name == Other->GetFName() &&
			   SoftClassPath.TryLoadClass<UObject>() == Other->GetClass();
	}
};


/** Represents a serialized Component */
USTRUCT()
struct FComponentRecord : public FObjectRecord
{
	GENERATED_BODY()

	FTransform Transform;


	virtual bool Serialize(FArchive& Ar) override;
};


/** Represents a serialized Actor */
USTRUCT()
struct FActorRecord : public FObjectRecord
{
	GENERATED_BODY()

	bool bHiddenInGame;
	/** Whether or not this actor was spawned in runtime */
	bool bIsProcedural;
	FTransform Transform;
	FVector LinearVelocity = FVector::ZeroVector;
	FVector AngularVelocity = FVector::ZeroVector;
	TArray<FComponentRecord> ComponentRecords;


	FActorRecord() : Super() {}
	FActorRecord(const AActor* Actor) : Super(Actor) {}

	virtual bool Serialize(FArchive& Ar) override;
};



USTRUCT()
struct FPlayerRecord : public FActorRecord
{
	GENERATED_BODY()


	//FUniqueNetIdRepl UniqueId;
	FString UniqueId;
	FPlayerRecord() : Super() {}
	FPlayerRecord(const AActor* Actor) : Super(Actor) {}
	virtual ~FPlayerRecord() {}

	virtual bool Serialize(FArchive& Ar) override;

	FORCEINLINE bool operator==(const FString& InUniqueId) const
	{
		return UniqueId == InUniqueId;
	}
};

USTRUCT()
struct FPlayerStateRecord : public FPlayerRecord
{
	GENERATED_BODY()

	FPlayerStateRecord() : Super() {}
	FPlayerStateRecord(const APlayerState* Actor) : Super(Actor) {}
};

USTRUCT()
struct FPlayerControllerRecord : public FPlayerRecord
{
	GENERATED_BODY()

	FPlayerControllerRecord() : Super() {}
	FPlayerControllerRecord(const AController* Actor) : Super(Actor) {}

};


USTRUCT()
struct FPlayerControlleredPawnRecord : public FPlayerRecord
{
	GENERATED_BODY()

	FPlayerControlleredPawnRecord() : Super() {}
	FPlayerControlleredPawnRecord(const APawn* Actor) : Super(Actor) {}

};