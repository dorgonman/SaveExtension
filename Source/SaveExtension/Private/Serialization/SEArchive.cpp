// Copyright 2015-2020 Piperift. All Rights Reserved.

#include "Serialization/SEArchive.h"
#include <UObject/NoExportTypes.h>
#include "ISaveExtension.h"
// GameplayAbilities
#include "AttributeSet.h"

// SaveExtension
#include "Serialization/Records.h"


/////////////////////////////////////////////////////
// FSEArchive

FArchive& FSEArchive::operator<<(UObject*& Obj)
{

	if (IsLoading())
	{
		// Deserialize the path name to the object
		FString ObjectPath;
		InnerArchive << ObjectPath;

		if (ObjectPath.IsEmpty())
		{
			// No object to deserialize
			Obj = nullptr;
			return *this;
		}

		// #FIX: Deserialize and assign outers

		// Look up the object by fully qualified pathname
		Obj = FindObject<UObject>(nullptr, *ObjectPath, false);
		// If we couldn't find it, and we want to load it, do that
		if (!Obj && bLoadIfFindFails)
		{
			Obj = LoadObject<UObject>(nullptr, *ObjectPath);
		}

		// Only serialize owned Objects
		bool bIsLocallyOwned;
		InnerArchive << bIsLocallyOwned;
		if (bIsLocallyOwned) 
		{
			FString AssetPath;
			InnerArchive << AssetPath;
			FSoftClassPath ClassPath(AssetPath);
			UObject* ResolvedOuter;
			FString ResolvedObjName = ObjectPath;
			ResolveName(ResolvedOuter, ResolvedObjName, true, true, false, nullptr);
	
			UClass* Class = ClassPath.TryLoadClass<UObject>();
			Obj = NewObject<UObject>(ResolvedOuter, Class, *ResolvedObjName);
		}

		if (Obj && bIsLocallyOwned)
		{
			Obj->Serialize(*this);
		}
	}
	else
	{
		if (Obj)
		{
			// Serialize the fully qualified object name
			FString SavedString{ Obj->GetPathName() };
			InnerArchive << SavedString;

			bool bIsLocallyOwned = IsObjectOwned(Obj);
			InnerArchive << bIsLocallyOwned;
			if (bIsLocallyOwned)
			{
				FSoftClassPath ClassPath(Obj->GetClass());
				FString AssetPath = ClassPath.GetAssetPathString();
				InnerArchive << AssetPath;
				Obj->Serialize(*this);
			}
		}
		else
		{
			FString SavedString{ "" };
			InnerArchive << SavedString;

			//bool bIsLocallyOwned = false;
			//InnerArchive << bIsLocallyOwned;
		}
	}

	if (Obj) 
	{
		UE_LOG(LogSaveExtension, Log, TEXT("FSEArchive::operator<< %s"), *Obj->GetName());
	
	}
	return *this;
}

bool FSEArchive::IsObjectOwned(UObject*& Obj) 
{
	return Cast<UAttributeSet>(Obj) != nullptr;//(Cast<AActor>(Obj) || Cast<UActorComponent>(Obj));
}
