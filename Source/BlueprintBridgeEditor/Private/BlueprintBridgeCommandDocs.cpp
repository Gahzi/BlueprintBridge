// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace BlueprintBridge
{
static FString GetDefaultCommandDocsDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintBridge"));
}

static TSharedRef<FJsonObject> BuildCommandDocsJson()
{
	TArray<TSharedPtr<FJsonValue>> Commands;
	for (const TSharedRef<ICommand>& Command : GetCommandRegistry().GetCommands())
	{
		Commands.Add(MakeShared<FJsonValueObject>(MakeCommandDescription(*Command, true)));
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schemaVersion"), TEXT("1"));
	Root->SetArrayField(TEXT("commands"), Commands);
	return Root;
}

static FString JsonSchemaToPrettyString(const TSharedPtr<FJsonObject>& Schema)
{
	if (!Schema.IsValid())
	{
		return TEXT("null");
	}

	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Schema.ToSharedRef(), Writer);
	return Output;
}

static FString BuildCommandDocsMarkdown()
{
	FString Markdown;
	Markdown += TEXT("# BlueprintBridge Command Reference\n\n");
	Markdown += TEXT("Generated from the runtime command registry. Command schemas describe `params` and `result` payloads; bridge responses still use the common `{ id, ok, result|error }` envelope.\n\n");

	for (const TSharedRef<ICommand>& Command : GetCommandRegistry().GetCommands())
	{
		Markdown += FString::Printf(TEXT("## `%s`\n\n"), *Command->GetName());
		Markdown += FString::Printf(TEXT("%s\n\n"), *Command->GetDescription());
		Markdown += FString::Printf(TEXT("- Category: `%s`\n"), *Command->GetCategory());
		Markdown += FString::Printf(TEXT("- Risk: `%s`\n\n"), *CommandRiskToString(Command->GetRisk()));

		Markdown += TEXT("### Input schema\n\n```json\n");
		Markdown += JsonSchemaToPrettyString(Command->GetInputJsonSchema());
		Markdown += TEXT("\n```\n\n");

		if (TSharedPtr<FJsonObject> OutputSchema = Command->GetOutputJsonSchema())
		{
			Markdown += TEXT("### Output schema\n\n```json\n");
			Markdown += JsonSchemaToPrettyString(OutputSchema);
			Markdown += TEXT("\n```\n\n");
		}
	}

	return Markdown;
}

TSharedRef<FJsonObject> GenerateCommandDocs(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString Directory = GetDefaultCommandDocsDirectory();
	FString Format = TEXT("both");
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("directory"), Directory);
		Params->TryGetStringField(TEXT("format"), Format);
	}

	if (!Format.Equals(TEXT("both"), ESearchCase::IgnoreCase) && !Format.Equals(TEXT("json"), ESearchCase::IgnoreCase) && !Format.Equals(TEXT("markdown"), ESearchCase::IgnoreCase))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("GenerateCommandDocs format must be both, json, or markdown."));
	}

	if (Directory.IsEmpty())
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("GenerateCommandDocs directory cannot be empty."));
	}

	IFileManager::Get().MakeDirectory(*Directory, true);

	FString JsonPath;
	FString MarkdownPath;
	if (Format.Equals(TEXT("both"), ESearchCase::IgnoreCase) || Format.Equals(TEXT("json"), ESearchCase::IgnoreCase))
	{
		JsonPath = FPaths::Combine(Directory, TEXT("BlueprintBridgeCommands.json"));
		if (!FFileHelper::SaveStringToFile(JsonToString(BuildCommandDocsJson()), *JsonPath))
		{
			return MakeBridgeError(Id, TEXT("WriteFailed"), FString::Printf(TEXT("Could not write '%s'."), *JsonPath));
		}
	}

	if (Format.Equals(TEXT("both"), ESearchCase::IgnoreCase) || Format.Equals(TEXT("markdown"), ESearchCase::IgnoreCase))
	{
		MarkdownPath = FPaths::Combine(Directory, TEXT("BlueprintBridgeCommands.md"));
		if (!FFileHelper::SaveStringToFile(BuildCommandDocsMarkdown(), *MarkdownPath))
		{
			return MakeBridgeError(Id, TEXT("WriteFailed"), FString::Printf(TEXT("Could not write '%s'."), *MarkdownPath));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!JsonPath.IsEmpty())
	{
		Result->SetStringField(TEXT("jsonPath"), JsonPath);
	}
	if (!MarkdownPath.IsEmpty())
	{
		Result->SetStringField(TEXT("markdownPath"), MarkdownPath);
	}
	Result->SetNumberField(TEXT("commandCount"), GetCommandRegistry().GetCommands().Num());
	return MakeSuccess(Id, Result);
}
} // namespace BlueprintBridge
