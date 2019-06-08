// Copyright 2015-2019 Piperift. All Rights Reserved
#pragma once

#include <CoreMinimal.h>
#include <Serialization/ObjectAndNameAsStringProxyArchive.h>


/** Serializes world data */
struct FSEArchive : public FObjectAndNameAsStringProxyArchive
{
private:
	UObject* rootOuter;
	TArray<UObject*> outerStack;

public:

	FSEArchive(FArchive &InInnerArchive, bool bInLoadIfFindFails)
		: FObjectAndNameAsStringProxyArchive(InInnerArchive,bInLoadIfFindFails)
	{
		ArIsSaveGame = true;
		ArNoDelta = true;
	}

	virtual FArchive& operator<<(UObject*& Obj) override;

private:

	bool IsObjectOwned(const UObject* Obj) const;
};