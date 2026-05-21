// Copyright Odyssey Interactive. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BlueprintBridgeCommand.h"

namespace BlueprintBridge
{
class FCommandRegistry
{
public:
	bool AddCommand(const TSharedRef<ICommand>& Command);
	bool RemoveCommand(const FString& Name);
	TSharedPtr<ICommand> FindCommand(const FString& Name) const;
	const TArray<TSharedRef<ICommand>>& GetCommands() const { return Commands; }
	void Reset() { Commands.Reset(); }

private:
	TArray<TSharedRef<ICommand>> Commands;
};

FCommandRegistry& GetCommandRegistry();
} // namespace BlueprintBridge
