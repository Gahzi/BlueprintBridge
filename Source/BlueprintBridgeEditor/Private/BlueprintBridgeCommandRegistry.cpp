// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandRegistry.h"

namespace BlueprintBridge
{
bool FCommandRegistry::AddCommand(const TSharedRef<ICommand>& Command)
{
	const FString Name = Command->GetName().TrimStartAndEnd();
	if (Name.IsEmpty())
	{
		return false;
	}

	if (FindCommand(Name).IsValid())
	{
		return false;
	}

	Commands.Add(Command);
	return true;
}

bool FCommandRegistry::RemoveCommand(const FString& Name)
{
	return Commands.RemoveAll([&Name](const TSharedRef<ICommand>& Command)
	{
		return Command->GetName().Equals(Name, ESearchCase::IgnoreCase);
	}) > 0;
}

TSharedPtr<ICommand> FCommandRegistry::FindCommand(const FString& Name) const
{
	const TSharedRef<ICommand>* Command = Commands.FindByPredicate([&Name](const TSharedRef<ICommand>& Candidate)
	{
		return Candidate->GetName().Equals(Name, ESearchCase::IgnoreCase);
	});

	if (Command)
	{
		return *Command;
	}

	return nullptr;
}

FCommandRegistry& GetCommandRegistry()
{
	static FCommandRegistry Registry;
	return Registry;
}
} // namespace BlueprintBridge
