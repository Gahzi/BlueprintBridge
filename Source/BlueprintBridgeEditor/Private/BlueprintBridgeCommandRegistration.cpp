// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandHandlers.h"

#include "BlueprintBridgeCommandRegistry.h"
#include "BlueprintBridgeSchema.h"

namespace BlueprintBridge
{
static TSharedPtr<FJsonObject> MakeEmptyObjectSchema()
{
	return FSchemaBuilder::Object().Build();
}

static void RegisterCommand(
	const TCHAR* Name,
	const TCHAR* Description,
	const TCHAR* Category,
	const ECommandRisk Risk,
	TSharedPtr<FJsonObject> InputSchema,
	FFunctionCommand::FHandler Handler,
	TSharedPtr<FJsonObject> OutputSchema = nullptr)
{
	const TSharedRef<FFunctionCommand> Command = MakeShared<FFunctionCommand>(Name, Description, Category, Risk, InputSchema.IsValid() ? InputSchema : MakeEmptyObjectSchema(), OutputSchema, MoveTemp(Handler));
	ensureMsgf(GetCommandRegistry().AddCommand(Command), TEXT("Failed to register Blueprint Bridge command '%s'."), Name);
}

static FSchemaBuilder AddAsset(FSchemaBuilder Builder)
{
	return Builder.RequiredString(TEXT("asset"), TEXT("Asset path, e.g. /Game/Path/BP_Asset."));
}

static FSchemaBuilder AddAssetGraph(FSchemaBuilder Builder)
{
	return AddAsset(Builder)
		.RequiredString(TEXT("graph"), TEXT("Blueprint graph name."));
}

static TSharedPtr<FJsonObject> MakeVector2Schema()
{
	return FSchemaBuilder::Object()
		.RequiredNumber(TEXT("x"), TEXT("X value."))
		.RequiredNumber(TEXT("y"), TEXT("Y value."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeVector3Schema()
{
	return FSchemaBuilder::Object()
		.RequiredNumber(TEXT("x"), TEXT("X value."))
		.RequiredNumber(TEXT("y"), TEXT("Y value."))
		.RequiredNumber(TEXT("z"), TEXT("Z value."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeRotatorSchema()
{
	return FSchemaBuilder::Object()
		.RequiredNumber(TEXT("pitch"), TEXT("Pitch in degrees."))
		.RequiredNumber(TEXT("yaw"), TEXT("Yaw in degrees."))
		.RequiredNumber(TEXT("roll"), TEXT("Roll in degrees."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAnchorsSchema()
{
	return FSchemaBuilder::Object()
		.RequiredNumber(TEXT("minimumX"), TEXT("Minimum X anchor."))
		.RequiredNumber(TEXT("minimumY"), TEXT("Minimum Y anchor."))
		.RequiredNumber(TEXT("maximumX"), TEXT("Maximum X anchor."))
		.RequiredNumber(TEXT("maximumY"), TEXT("Maximum Y anchor."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeMarginSchema()
{
	return FSchemaBuilder::Object()
		.OptionalNumber(TEXT("left"), TEXT("Left padding."))
		.OptionalNumber(TEXT("top"), TEXT("Top padding."))
		.OptionalNumber(TEXT("right"), TEXT("Right padding."))
		.OptionalNumber(TEXT("bottom"), TEXT("Bottom padding."))
		.Build();
}

static FSchemaBuilder AddGraphEditBase(FSchemaBuilder Builder)
{
	return AddAssetGraph(Builder)
		.RequiredNumber(TEXT("x"), TEXT("Node X position."))
		.RequiredNumber(TEXT("y"), TEXT("Node Y position."))
		.OptionalObject(TEXT("pinDefaults"), TEXT("Optional map of input pin names to default values."));
}

static FSchemaBuilder AddPinTypeFields(FSchemaBuilder Builder)
{
	return Builder
		.OptionalString(TEXT("category"), TEXT("Pin category, e.g. bool, int, float, name, string, object, class, struct."))
		.OptionalString(TEXT("sourceVariable"), TEXT("Optional existing variable to copy the pin type from."))
		.OptionalString(TEXT("subCategory"), TEXT("Optional pin sub-category."))
		.OptionalString(TEXT("subCategoryObject"), TEXT("Optional object path for object, class, enum, or struct pin types."))
		.OptionalStringEnum(TEXT("containerType"), TEXT("Optional container type."), { TEXT("None"), TEXT("Array"), TEXT("Set") })
		.OptionalBoolean(TEXT("isArray"), TEXT("Legacy shortcut for containerType=Array."))
		.OptionalBoolean(TEXT("byRef"), TEXT("Whether the pin should be passed by reference."))
		.OptionalBoolean(TEXT("const"), TEXT("Whether the reference pin should be const."));
}

static TSharedPtr<FJsonObject> MakeBatchSchema()
{
	return FSchemaBuilder::Object()
		.RequiredArray(TEXT("requests"), TEXT("Bridge request objects to execute."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeDescribeCommandSchema()
{
	return FSchemaBuilder::Object()
		.RequiredString(TEXT("command"), TEXT("Command name to describe."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeGenerateCommandDocsSchema()
{
	return FSchemaBuilder::Object()
		.OptionalString(TEXT("directory"), TEXT("Optional output directory. Defaults to Project/Saved/BlueprintBridge."))
		.OptionalStringEnum(TEXT("format"), TEXT("Documentation format to generate."), { TEXT("both"), TEXT("json"), TEXT("markdown") })
		.Build();
}

static TSharedPtr<FJsonObject> MakeTypeSchema(const TCHAR* Type, const TCHAR* Description)
{
	TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), Type);
	Schema->SetStringField(TEXT("description"), Description);
	return Schema;
}

static TSharedPtr<FJsonObject> MakePingOutputSchema()
{
	return MakeTypeSchema(TEXT("string"), TEXT("Pong response string."));
}

static TSharedPtr<FJsonObject> MakeListCommandsOutputSchema()
{
	return FSchemaBuilder::Object()
		.RequiredArray(TEXT("commands"), TEXT("Registered command summaries with name, description, category, and risk."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeDescribeCommandOutputSchema()
{
	return FSchemaBuilder::Object()
		.RequiredString(TEXT("name"), TEXT("Canonical command name."))
		.RequiredString(TEXT("description"), TEXT("Command description."))
		.RequiredString(TEXT("category"), TEXT("Command category."))
		.RequiredString(TEXT("risk"), TEXT("Risk classification."))
		.OptionalObject(TEXT("inputSchema"), TEXT("Descriptive JSON schema for params."))
		.OptionalObject(TEXT("outputSchema"), TEXT("Descriptive JSON schema for result."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeGenerateCommandDocsOutputSchema()
{
	return FSchemaBuilder::Object()
		.OptionalString(TEXT("jsonPath"), TEXT("Generated JSON schema document path."))
		.OptionalString(TEXT("markdownPath"), TEXT("Generated Markdown reference document path."))
		.RequiredNumber(TEXT("commandCount"), TEXT("Number of commands documented."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeDescribeBlueprintOutputSchema()
{
	return FSchemaBuilder::Object()
		.RequiredString(TEXT("asset"), TEXT("Requested Blueprint asset path."))
		.RequiredString(TEXT("name"), TEXT("Blueprint asset name."))
		.RequiredString(TEXT("parentClass"), TEXT("Blueprint parent class path."))
		.RequiredArray(TEXT("variables"), TEXT("Blueprint variables with type, flags, and replication metadata."))
		.RequiredArray(TEXT("graphs"), TEXT("Blueprint graph names."))
		.OptionalObject(TEXT("widgetTree"), TEXT("Widget tree description for Widget Blueprints."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeDescribeGraphOutputSchema()
{
	return FSchemaBuilder::Object()
		.RequiredString(TEXT("asset"), TEXT("Requested Blueprint asset path."))
		.RequiredString(TEXT("graph"), TEXT("Resolved graph name."))
		.RequiredArray(TEXT("nodes"), TEXT("Graph node descriptions including pins, defaults, links, and positions."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeDescribeNodeOutputSchema()
{
	return FSchemaBuilder::Object()
		.RequiredObject(TEXT("node"), TEXT("Graph node description including pins, defaults, links, and position."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeFindNodesOutputSchema()
{
	return FSchemaBuilder::Object()
		.RequiredArray(TEXT("nodes"), TEXT("Matching graph node descriptions. Each match includes the owning graph name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeCompileBlueprintOutputSchema()
{
	return FSchemaBuilder::Object()
		.RequiredString(TEXT("status"), TEXT("Post-compile Blueprint status."))
		.RequiredBoolean(TEXT("success"), TEXT("Whether compile completed without errors."))
		.RequiredNumber(TEXT("errorCount"), TEXT("Number of compiler errors captured."))
		.RequiredNumber(TEXT("warningCount"), TEXT("Number of compiler warnings captured."))
		.RequiredArray(TEXT("messages"), TEXT("Compiler diagnostics with severity, message, and optional node or pin references."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAssetSchema()
{
	return AddAsset(FSchemaBuilder::Object()).Build();
}

static TSharedPtr<FJsonObject> MakeAssetGraphSchema()
{
	return AddAssetGraph(FSchemaBuilder::Object()).Build();
}

static TSharedPtr<FJsonObject> MakeDescribeNodeSchema()
{
	return AddAssetGraph(FSchemaBuilder::Object())
		.RequiredString(TEXT("node"), TEXT("Node GUID to describe."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeFindNodesSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.OptionalString(TEXT("graph"), TEXT("Optional graph name to search."))
		.OptionalString(TEXT("class"), TEXT("Optional node class path substring."))
		.OptionalString(TEXT("title"), TEXT("Optional node title substring."))
		.OptionalString(TEXT("variable"), TEXT("Optional variable name filter."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeFindVariableReferencesSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("variable"), TEXT("Blueprint variable name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeDescribeSubobjectsSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.OptionalString(TEXT("subobjectClass"), TEXT("Optional subobject class path filter."))
		.OptionalBoolean(TEXT("includeProperties"), TEXT("Whether to include editable property values."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddComponentSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("name"), TEXT("New component name."))
		.RequiredString(TEXT("componentClass"), TEXT("Component class path."))
		.OptionalString(TEXT("parent"), TEXT("Optional parent component name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAttachComponentSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("name"), TEXT("Component name."))
		.RequiredString(TEXT("parent"), TEXT("Parent component name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeComponentNameSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("name"), TEXT("Component name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeSetComponentTransformSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("name"), TEXT("Scene component name."))
		.OptionalObject(TEXT("location"), TEXT("Optional location object with x, y, and z."), MakeVector3Schema())
		.OptionalObject(TEXT("rotation"), TEXT("Optional rotation object with pitch, yaw, and roll."), MakeRotatorSchema())
		.OptionalObject(TEXT("scale"), TEXT("Optional scale object with x, y, and z."), MakeVector3Schema())
		.Build();
}

static TSharedPtr<FJsonObject> MakeSetComponentPropertySchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("name"), TEXT("Component name."))
		.RequiredString(TEXT("property"), TEXT("Property name."))
		.RequiredString(TEXT("value"), TEXT("Property value as text."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeSetStaticMeshSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("name"), TEXT("StaticMeshComponent name."))
		.RequiredString(TEXT("mesh"), TEXT("Static mesh asset path."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeSetCollisionProfileNameSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("name"), TEXT("Primitive component name."))
		.RequiredString(TEXT("profile"), TEXT("Collision profile name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeSetBoxExtentSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("name"), TEXT("Box component name."))
		.RequiredObject(TEXT("extent"), TEXT("Extent object with x, y, and z."), MakeVector3Schema())
		.Build();
}

static TSharedPtr<FJsonObject> MakeSetGenerateOverlapEventsSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("name"), TEXT("Primitive component name."))
		.RequiredBoolean(TEXT("generateOverlapEvents"), TEXT("Whether overlap events are generated."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeCreateFunctionGraphSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("function"), TEXT("Function graph name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddFunctionPinSchema()
{
	return AddPinTypeFields(AddAssetGraph(FSchemaBuilder::Object())
		.RequiredString(TEXT("name"), TEXT("Pin name.")))
		.Build();
}

static TSharedPtr<FJsonObject> MakeRenameCustomEventSchema()
{
	return AddAssetGraph(FSchemaBuilder::Object())
		.RequiredString(TEXT("node"), TEXT("Custom event node GUID."))
		.RequiredString(TEXT("newName"), TEXT("New custom event name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeEditUserDefinedPinSchema()
{
	return AddPinTypeFields(AddAssetGraph(FSchemaBuilder::Object())
		.RequiredString(TEXT("node"), TEXT("Editable node GUID."))
		.RequiredString(TEXT("pin"), TEXT("Existing pin name."))
		.OptionalString(TEXT("newName"), TEXT("Optional new pin name.")))
		.Build();
}

static TSharedPtr<FJsonObject> MakeRenameGraphSchema()
{
	return AddAssetGraph(FSchemaBuilder::Object())
		.RequiredString(TEXT("newName"), TEXT("New graph name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddVariableGetterFunctionSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("function"), TEXT("Function graph name to create."))
		.RequiredString(TEXT("variable"), TEXT("Variable name to return."))
		.OptionalString(TEXT("output"), TEXT("Optional output pin name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeVariableNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.RequiredString(TEXT("variable"), TEXT("Blueprint variable name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeGraphNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object()).Build();
}

static TSharedPtr<FJsonObject> MakeAddSequenceNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.OptionalNumber(TEXT("extraOutputs"), TEXT("Additional output pins to add."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddCommentNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.OptionalString(TEXT("text"), TEXT("Comment text."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeEnumNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.RequiredString(TEXT("enum"), TEXT("Enum object path."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddFunctionCallNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.RequiredString(TEXT("functionClass"), TEXT("Class containing the function."))
		.RequiredString(TEXT("function"), TEXT("Function name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddArrayFunctionNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.RequiredStringEnum(TEXT("operation"), TEXT("Array operation name."), { TEXT("Add"), TEXT("AddUnique"), TEXT("Remove"), TEXT("RemoveItem"), TEXT("Clear"), TEXT("Length"), TEXT("Get"), TEXT("Contains") })
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddTimerNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.RequiredStringEnum(TEXT("operation"), TEXT("Timer operation name."), { TEXT("SetByEvent"), TEXT("SetByFunctionName"), TEXT("ClearByHandle"), TEXT("ClearAndInvalidateByHandle") })
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddLineTraceNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.OptionalStringEnum(TEXT("operation"), TEXT("Trace operation."), { TEXT("Single"), TEXT("Multi") })
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddMathNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.RequiredString(TEXT("function"), TEXT("Kismet math library function name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddWidgetFunctionNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.RequiredString(TEXT("widgetClass"), TEXT("Widget class path."))
		.RequiredString(TEXT("function"), TEXT("Widget function name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddCustomEventNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.RequiredString(TEXT("name"), TEXT("Custom event name."))
		.OptionalArray(TEXT("inputs"), TEXT("Optional user-defined output pins."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddEventNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.RequiredString(TEXT("eventClass"), TEXT("Class containing the event."))
		.RequiredString(TEXT("event"), TEXT("Event function name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddClassNodeSchema(const TCHAR* FieldName, const TCHAR* Description)
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.RequiredString(FieldName, Description)
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddEventDispatcherSchema()
{
	return AddPinTypeFields(AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("name"), TEXT("Event dispatcher name."))
		.OptionalArray(TEXT("inputs"), TEXT("Optional dispatcher input pins.")))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddComponentEventNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.RequiredString(TEXT("component"), TEXT("Component variable name."))
		.RequiredString(TEXT("delegate"), TEXT("Delegate name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeDelegateNodeSchema(const bool bRequireFunction)
{
	FSchemaBuilder Builder = AddGraphEditBase(FSchemaBuilder::Object())
		.RequiredString(TEXT("delegate"), TEXT("Delegate name."))
		.OptionalString(TEXT("ownerClass"), TEXT("Optional owner class path."));
	if (bRequireFunction)
	{
		Builder.RequiredString(TEXT("function"), TEXT("Function name."));
	}
	else
	{
		Builder.OptionalString(TEXT("function"), TEXT("Optional function name."));
	}
	return Builder.Build();
}

static TSharedPtr<FJsonObject> MakeSetCreateDelegateFunctionSchema()
{
	return AddAssetGraph(FSchemaBuilder::Object())
		.RequiredString(TEXT("node"), TEXT("Create delegate node GUID."))
		.RequiredString(TEXT("function"), TEXT("Function name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeMacroNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.OptionalString(TEXT("macro"), TEXT("Optional macro name override."))
		.OptionalString(TEXT("macroLibrary"), TEXT("Optional macro library path override."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeStructNodeSchema()
{
	return AddGraphEditBase(FSchemaBuilder::Object())
		.RequiredString(TEXT("struct"), TEXT("Struct object path."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeConnectPinsSchema()
{
	return AddAssetGraph(FSchemaBuilder::Object())
		.RequiredString(TEXT("fromNode"), TEXT("Source node GUID."))
		.RequiredString(TEXT("fromPin"), TEXT("Source pin name."))
		.RequiredString(TEXT("toNode"), TEXT("Target node GUID."))
		.RequiredString(TEXT("toPin"), TEXT("Target pin name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeSetPinDefaultSchema()
{
	return AddAssetGraph(FSchemaBuilder::Object())
		.RequiredString(TEXT("node"), TEXT("Node GUID."))
		.RequiredString(TEXT("pin"), TEXT("Pin name."))
		.RequiredString(TEXT("value"), TEXT("New pin default value."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeSetNodePositionSchema()
{
	return AddAssetGraph(FSchemaBuilder::Object())
		.RequiredString(TEXT("node"), TEXT("Node GUID."))
		.RequiredNumber(TEXT("x"), TEXT("Node X position."))
		.RequiredNumber(TEXT("y"), TEXT("Node Y position."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeNodePinSchema()
{
	return AddAssetGraph(FSchemaBuilder::Object())
		.RequiredString(TEXT("node"), TEXT("Node GUID."))
		.RequiredString(TEXT("pin"), TEXT("Pin name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeDeleteNodeSchema()
{
	return AddAssetGraph(FSchemaBuilder::Object())
		.RequiredString(TEXT("node"), TEXT("Node GUID."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeCreateBlueprintAssetSchema()
{
	return FSchemaBuilder::Object()
		.RequiredString(TEXT("asset"), TEXT("New Blueprint asset path."))
		.RequiredString(TEXT("parentClass"), TEXT("Parent class path."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeCreateWidgetBlueprintAssetSchema()
{
	return FSchemaBuilder::Object()
		.RequiredString(TEXT("asset"), TEXT("New Widget Blueprint asset path."))
		.OptionalString(TEXT("parentClass"), TEXT("Optional widget parent class path."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeDuplicateAssetSchema()
{
	return FSchemaBuilder::Object()
		.RequiredString(TEXT("sourceAsset"), TEXT("Source asset path."))
		.RequiredString(TEXT("destAsset"), TEXT("Destination asset path."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddWidgetSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("name"), TEXT("Widget name."))
		.RequiredString(TEXT("widgetClass"), TEXT("Widget class path."))
		.OptionalString(TEXT("parent"), TEXT("Optional parent widget name."))
		.OptionalBoolean(TEXT("root"), TEXT("Whether to set the widget as root."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeSetRootWidgetSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("widget"), TEXT("Widget name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddWidgetToParentSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("parent"), TEXT("Parent widget name."))
		.RequiredString(TEXT("child"), TEXT("Child widget name."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeSetWidgetSlotLayoutSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("widget"), TEXT("Widget name."))
		.OptionalObject(TEXT("position"), TEXT("Optional canvas position object."), MakeVector2Schema())
		.OptionalObject(TEXT("size"), TEXT("Optional canvas size object."), MakeVector2Schema())
		.OptionalObject(TEXT("alignment"), TEXT("Optional canvas alignment object."), MakeVector2Schema())
		.OptionalObject(TEXT("anchors"), TEXT("Optional canvas anchors object."), MakeAnchorsSchema())
		.OptionalObject(TEXT("padding"), TEXT("Optional panel slot padding object."), MakeMarginSchema())
		.Build();
}

static TSharedPtr<FJsonObject> MakeSetBlueprintDefaultSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("property"), TEXT("Property name."))
		.RequiredString(TEXT("value"), TEXT("Property value as text."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeSetSubobjectDefaultSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("subobject"), TEXT("Subobject name or path."))
		.RequiredString(TEXT("property"), TEXT("Property name."))
		.RequiredString(TEXT("value"), TEXT("Property value as text."))
		.OptionalString(TEXT("subobjectClass"), TEXT("Optional subobject class path filter."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeSetBlueprintVariableFlagsSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("variable"), TEXT("Blueprint variable name."))
		.OptionalBoolean(TEXT("instanceEditable"), TEXT("Whether the variable is instance editable."))
		.OptionalBoolean(TEXT("blueprintReadOnly"), TEXT("Whether the variable is Blueprint read-only."))
		.OptionalBoolean(TEXT("exposeOnSpawn"), TEXT("Whether the variable is exposed on spawn."))
		.OptionalBoolean(TEXT("private"), TEXT("Whether the variable is private."))
		.OptionalString(TEXT("categoryName"), TEXT("Variable category name."))
		.OptionalString(TEXT("tooltip"), TEXT("Variable tooltip."))
		.OptionalStringEnum(TEXT("replication"), TEXT("Replication mode."), { TEXT("None"), TEXT("Replicated"), TEXT("RepNotify") })
		.OptionalString(TEXT("repNotifyFunc"), TEXT("RepNotify function name when replication is RepNotify."))
		.Build();
}

static TSharedPtr<FJsonObject> MakeAddBlueprintVariableSchema()
{
	return AddAsset(FSchemaBuilder::Object())
		.RequiredString(TEXT("name"), TEXT("Variable name."))
		.RequiredString(TEXT("category"), TEXT("Pin category."))
		.OptionalString(TEXT("subCategory"), TEXT("Optional pin sub-category."))
		.OptionalString(TEXT("subCategoryObject"), TEXT("Optional sub-category object path."))
		.OptionalStringEnum(TEXT("containerType"), TEXT("Optional container type."), { TEXT("None"), TEXT("Array"), TEXT("Set") })
		.OptionalBoolean(TEXT("isArray"), TEXT("Legacy shortcut for containerType=Array."))
		.OptionalString(TEXT("defaultValue"), TEXT("Optional default value as text."))
		.Build();
}

void RegisterBlueprintBridgeCommands()
{
	RegisterCommand(TEXT("Batch"), TEXT("Executes multiple bridge requests and returns their responses."), TEXT("Protocol"), ECommandRisk::ReadOnly, MakeBatchSchema(), &BatchCommand);
	RegisterCommand(TEXT("Ping"), TEXT("Returns Pong."), TEXT("Basic"), ECommandRisk::ReadOnly, MakeEmptyObjectSchema(), &PingCommand, MakePingOutputSchema());
	RegisterCommand(TEXT("GetProjectName"), TEXT("Returns the current Unreal project name."), TEXT("Basic"), ECommandRisk::ReadOnly, MakeEmptyObjectSchema(), &GetProjectNameCommand, MakeTypeSchema(TEXT("string"), TEXT("Current Unreal project name.")));
	RegisterCommand(TEXT("GetEngineVersion"), TEXT("Returns the current engine version string."), TEXT("Basic"), ECommandRisk::ReadOnly, MakeEmptyObjectSchema(), &GetEngineVersionCommand, MakeTypeSchema(TEXT("string"), TEXT("Current Unreal Engine version string.")));
	RegisterCommand(TEXT("ListCommands"), TEXT("Lists registered Blueprint Bridge commands."), TEXT("Protocol"), ECommandRisk::ReadOnly, MakeEmptyObjectSchema(), &ListCommands, MakeListCommandsOutputSchema());
	RegisterCommand(TEXT("DescribeCommand"), TEXT("Returns metadata and schemas for a registered command."), TEXT("Protocol"), ECommandRisk::ReadOnly, MakeDescribeCommandSchema(), &DescribeCommand, MakeDescribeCommandOutputSchema());
	RegisterCommand(TEXT("GenerateCommandDocs"), TEXT("Generates JSON and/or Markdown command schema documentation."), TEXT("Protocol"), ECommandRisk::ReadOnly, MakeGenerateCommandDocsSchema(), &GenerateCommandDocs, MakeGenerateCommandDocsOutputSchema());

	RegisterCommand(TEXT("DescribeBlueprint"), TEXT("Returns parent class, variables, and graph names for a Blueprint."), TEXT("BlueprintInspection"), ECommandRisk::ReadOnly, MakeAssetSchema(), &DescribeBlueprint, MakeDescribeBlueprintOutputSchema());
	RegisterCommand(TEXT("DescribeGraph"), TEXT("Returns nodes, pins, defaults, links, and positions for a Blueprint graph."), TEXT("BlueprintInspection"), ECommandRisk::ReadOnly, MakeAssetGraphSchema(), &DescribeGraph, MakeDescribeGraphOutputSchema());
	RegisterCommand(TEXT("DescribeNode"), TEXT("Returns a single Blueprint graph node description."), TEXT("BlueprintInspection"), ECommandRisk::ReadOnly, MakeDescribeNodeSchema(), &DescribeNodeCommand, MakeDescribeNodeOutputSchema());
	RegisterCommand(TEXT("FindNodes"), TEXT("Finds Blueprint graph nodes matching optional filters."), TEXT("BlueprintInspection"), ECommandRisk::ReadOnly, MakeFindNodesSchema(), &FindNodes, MakeFindNodesOutputSchema());
	RegisterCommand(TEXT("FindVariableReferences"), TEXT("Finds get/set nodes referencing a Blueprint variable."), TEXT("BlueprintInspection"), ECommandRisk::ReadOnly, MakeFindVariableReferencesSchema(), &FindVariableReferences);
	RegisterCommand(TEXT("DescribeComponents"), TEXT("Returns Blueprint SCS component information."), TEXT("ComponentInspection"), ECommandRisk::ReadOnly, MakeAssetSchema(), &DescribeComponents);
	RegisterCommand(TEXT("DescribeWidgetTree"), TEXT("Returns UMG widget tree information for a Widget Blueprint."), TEXT("WidgetInspection"), ECommandRisk::ReadOnly, MakeAssetSchema(), &DescribeWidgetTree);
	RegisterCommand(TEXT("DescribeSubobjects"), TEXT("Returns Blueprint subobject data."), TEXT("BlueprintInspection"), ECommandRisk::ReadOnly, MakeDescribeSubobjectsSchema(), &DescribeSubobjects);

	RegisterCommand(TEXT("AddComponent"), TEXT("Adds a component to a Blueprint SCS tree."), TEXT("ComponentEditing"), ECommandRisk::ModifiesAsset, MakeAddComponentSchema(), &AddComponent);
	RegisterCommand(TEXT("AttachComponent"), TEXT("Attaches one Blueprint component to another."), TEXT("ComponentEditing"), ECommandRisk::ModifiesAsset, MakeAttachComponentSchema(), &AttachComponent);
	RegisterCommand(TEXT("SetComponentTransform"), TEXT("Sets a scene component template transform."), TEXT("ComponentEditing"), ECommandRisk::ModifiesAsset, MakeSetComponentTransformSchema(), &SetComponentTransform);
	RegisterCommand(TEXT("SetComponentProperty"), TEXT("Sets a property on a component template."), TEXT("ComponentEditing"), ECommandRisk::ModifiesAsset, MakeSetComponentPropertySchema(), &SetComponentProperty);
	RegisterCommand(TEXT("SetRootComponent"), TEXT("Sets the Blueprint root component."), TEXT("ComponentEditing"), ECommandRisk::ModifiesAsset, MakeComponentNameSchema(), &SetRootComponent);
	RegisterCommand(TEXT("SetStaticMesh"), TEXT("Sets a StaticMeshComponent template mesh."), TEXT("ComponentEditing"), ECommandRisk::ModifiesAsset, MakeSetStaticMeshSchema(), &SetStaticMesh);
	RegisterCommand(TEXT("SetCollisionProfileName"), TEXT("Sets a primitive component collision profile name."), TEXT("ComponentEditing"), ECommandRisk::ModifiesAsset, MakeSetCollisionProfileNameSchema(), &SetCollisionProfileName);
	RegisterCommand(TEXT("SetBoxExtent"), TEXT("Sets a BoxComponent extent."), TEXT("ComponentEditing"), ECommandRisk::ModifiesAsset, MakeSetBoxExtentSchema(), &SetBoxExtent);
	RegisterCommand(TEXT("SetGenerateOverlapEvents"), TEXT("Sets primitive component overlap generation."), TEXT("ComponentEditing"), ECommandRisk::ModifiesAsset, MakeSetGenerateOverlapEventsSchema(), &SetGenerateOverlapEvents);

	RegisterCommand(TEXT("CreateFunctionGraph"), TEXT("Creates a user function graph."), TEXT("GraphManagement"), ECommandRisk::ModifiesAsset, MakeCreateFunctionGraphSchema(), &CreateFunctionGraph);
	RegisterCommand(TEXT("CreateEventGraph"), TEXT("Creates an event graph page."), TEXT("GraphManagement"), ECommandRisk::ModifiesAsset, MakeAssetGraphSchema(), &CreateEventGraph);
	RegisterCommand(TEXT("AddFunctionInput"), TEXT("Adds an input pin to a function graph."), TEXT("GraphManagement"), ECommandRisk::ModifiesAsset, MakeAddFunctionPinSchema(), &AddFunctionInput);
	RegisterCommand(TEXT("AddFunctionOutput"), TEXT("Adds an output pin to a function graph."), TEXT("GraphManagement"), ECommandRisk::ModifiesAsset, MakeAddFunctionPinSchema(), &AddFunctionOutput);
	RegisterCommand(TEXT("DeleteGraph"), TEXT("Deletes a Blueprint graph."), TEXT("GraphManagement"), ECommandRisk::ModifiesAsset, MakeAssetGraphSchema(), &DeleteGraph);
	RegisterCommand(TEXT("RenameCustomEvent"), TEXT("Renames a custom event node."), TEXT("GraphManagement"), ECommandRisk::ModifiesAsset, MakeRenameCustomEventSchema(), &RenameCustomEvent);
	RegisterCommand(TEXT("EditUserDefinedPin"), TEXT("Edits a user-defined event or function pin."), TEXT("GraphManagement"), ECommandRisk::ModifiesAsset, MakeEditUserDefinedPinSchema(), &EditUserDefinedPin);
	RegisterCommand(TEXT("RenameGraph"), TEXT("Renames a Blueprint graph."), TEXT("GraphManagement"), ECommandRisk::ModifiesAsset, MakeRenameGraphSchema(), &RenameGraph);
	RegisterCommand(TEXT("AddVariableGetterFunction"), TEXT("Creates a function that returns a Blueprint variable."), TEXT("GraphManagement"), ECommandRisk::ModifiesAsset, MakeAddVariableGetterFunctionSchema(), &AddVariableGetterFunction);

	RegisterCommand(TEXT("AddVariableGetNode"), TEXT("Adds a Blueprint variable getter node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeVariableNodeSchema(), &AddVariableGetNode);
	RegisterCommand(TEXT("AddVariableSetNode"), TEXT("Adds a Blueprint variable setter node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeVariableNodeSchema(), &AddVariableSetNode);
	RegisterCommand(TEXT("AddBranchNode"), TEXT("Adds a Branch node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeGraphNodeSchema(), &AddBranchNode);
	RegisterCommand(TEXT("AddSequenceNode"), TEXT("Adds a Sequence node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddSequenceNodeSchema(), &AddSequenceNode);
	RegisterCommand(TEXT("AddRerouteNode"), TEXT("Adds a reroute node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeGraphNodeSchema(), &AddRerouteNode);
	RegisterCommand(TEXT("AddCommentNode"), TEXT("Adds a graph comment node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddCommentNodeSchema(), &AddCommentNode);
	RegisterCommand(TEXT("AddEnumSwitchNode"), TEXT("Adds an enum switch node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeEnumNodeSchema(), &AddEnumSwitchNode);
	RegisterCommand(TEXT("AddEnumEqualityNode"), TEXT("Adds an enum equality node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeGraphNodeSchema(), &AddEnumEqualityNode);
	RegisterCommand(TEXT("AddFunctionCallNode"), TEXT("Adds a function call node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddFunctionCallNodeSchema(), &AddFunctionCallNode);
	RegisterCommand(TEXT("AddSelfNode"), TEXT("Adds a Self node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeGraphNodeSchema(), &AddSelfNode);
	RegisterCommand(TEXT("AddArrayFunctionNode"), TEXT("Adds an array library function node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddArrayFunctionNodeSchema(), &AddArrayFunctionNode);
	RegisterCommand(TEXT("AddTimerNode"), TEXT("Adds a timer function node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddTimerNodeSchema(), &AddTimerNode);
	RegisterCommand(TEXT("AddLineTraceNode"), TEXT("Adds a line trace function node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddLineTraceNodeSchema(), &AddLineTraceNode);
	RegisterCommand(TEXT("AddMathNode"), TEXT("Adds a math library function node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddMathNodeSchema(), &AddMathNode);
	RegisterCommand(TEXT("AddWidgetFunctionNode"), TEXT("Adds a widget library function node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddWidgetFunctionNodeSchema(), &AddWidgetFunctionNode);
	RegisterCommand(TEXT("AddCustomEventNode"), TEXT("Adds a custom event node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddCustomEventNodeSchema(), &AddCustomEventNode);
	RegisterCommand(TEXT("AddEventNode"), TEXT("Adds an event node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddEventNodeSchema(), &AddEventNode);
	RegisterCommand(TEXT("AddDynamicCastNode"), TEXT("Adds a dynamic cast node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddClassNodeSchema(TEXT("targetClass"), TEXT("Target class path.")), &AddDynamicCastNode);
	RegisterCommand(TEXT("AddSpawnActorNode"), TEXT("Adds a SpawnActor node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddClassNodeSchema(TEXT("actorClass"), TEXT("Actor class path.")), &AddSpawnActorNode);
	RegisterCommand(TEXT("AddEventDispatcher"), TEXT("Adds an event dispatcher to a Blueprint."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddEventDispatcherSchema(), &AddEventDispatcher);
	RegisterCommand(TEXT("AddComponentEventNode"), TEXT("Adds a component-bound event node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddComponentEventNodeSchema(), &AddComponentEventNode);
	RegisterCommand(TEXT("AddDelegateBindNode"), TEXT("Adds a delegate bind node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeDelegateNodeSchema(false), &AddDelegateBindNode);
	RegisterCommand(TEXT("AddDelegateBroadcastNode"), TEXT("Adds a delegate broadcast node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeDelegateNodeSchema(false), &AddDelegateBroadcastNode);
	RegisterCommand(TEXT("AddCreateDelegateNode"), TEXT("Adds a create delegate node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeDelegateNodeSchema(false), &AddCreateDelegateNode);
	RegisterCommand(TEXT("SetCreateDelegateFunction"), TEXT("Sets the function name on a create delegate node."), TEXT("NodeEditing"), ECommandRisk::ModifiesAsset, MakeSetCreateDelegateFunctionSchema(), &SetCreateDelegateFunction);
	RegisterCommand(TEXT("AddForLoopNode"), TEXT("Adds a ForLoop macro instance node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeMacroNodeSchema(), &AddForLoopNode);
	RegisterCommand(TEXT("AddForEachLoopNode"), TEXT("Adds a ForEachLoop macro instance node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeMacroNodeSchema(), &AddForEachLoopNode);
	RegisterCommand(TEXT("AddAuthoritySwitchNode"), TEXT("Adds an authority switch macro instance node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeMacroNodeSchema(), &AddAuthoritySwitchNode);
	RegisterCommand(TEXT("AddMakeStructNode"), TEXT("Adds a Make Struct node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeStructNodeSchema(), &AddMakeStructNode);
	RegisterCommand(TEXT("AddBreakStructNode"), TEXT("Adds a Break Struct node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeStructNodeSchema(), &AddBreakStructNode);
	RegisterCommand(TEXT("AddCreateWidgetNode"), TEXT("Adds a Create Widget node."), TEXT("NodeCreation"), ECommandRisk::ModifiesAsset, MakeAddClassNodeSchema(TEXT("widgetClass"), TEXT("Widget class path.")), &AddCreateWidgetNode);

	RegisterCommand(TEXT("ConnectPins"), TEXT("Connects two graph pins."), TEXT("PinEditing"), ECommandRisk::ModifiesAsset, MakeConnectPinsSchema(), &ConnectPins);
	RegisterCommand(TEXT("MovePinLinks"), TEXT("Moves all links from one pin to another."), TEXT("PinEditing"), ECommandRisk::ModifiesAsset, MakeConnectPinsSchema(), &MovePinLinksCommand);
	RegisterCommand(TEXT("SetPinDefault"), TEXT("Sets a pin default value."), TEXT("PinEditing"), ECommandRisk::ModifiesAsset, MakeSetPinDefaultSchema(), &SetPinDefault);
	RegisterCommand(TEXT("SetNodePosition"), TEXT("Sets a graph node position."), TEXT("NodeEditing"), ECommandRisk::ModifiesAsset, MakeSetNodePositionSchema(), &SetNodePosition);
	RegisterCommand(TEXT("BreakPinLinks"), TEXT("Breaks all links on a pin."), TEXT("PinEditing"), ECommandRisk::ModifiesAsset, MakeNodePinSchema(), &BreakPinLinks);
	RegisterCommand(TEXT("CopyPinType"), TEXT("Copies a pin type from one pin to another."), TEXT("PinEditing"), ECommandRisk::ModifiesAsset, MakeConnectPinsSchema(), &CopyPinType);
	RegisterCommand(TEXT("DeleteNode"), TEXT("Deletes a graph node."), TEXT("NodeEditing"), ECommandRisk::ModifiesAsset, MakeDeleteNodeSchema(), &DeleteNode);

	RegisterCommand(TEXT("CreateBlueprintAsset"), TEXT("Creates a Blueprint asset."), TEXT("Asset"), ECommandRisk::CreatesAsset, MakeCreateBlueprintAssetSchema(), &CreateBlueprintAsset);
	RegisterCommand(TEXT("CreateWidgetBlueprintAsset"), TEXT("Creates a Widget Blueprint asset."), TEXT("Asset"), ECommandRisk::CreatesAsset, MakeCreateWidgetBlueprintAssetSchema(), &CreateWidgetBlueprintAsset);
	RegisterCommand(TEXT("DuplicateAsset"), TEXT("Duplicates an editor asset."), TEXT("Asset"), ECommandRisk::CreatesAsset, MakeDuplicateAssetSchema(), &DuplicateAsset);
	RegisterCommand(TEXT("CheckoutAsset"), TEXT("Checks out an asset through source control."), TEXT("SourceControl"), ECommandRisk::SourceControl, MakeAssetSchema(), &CheckoutAsset);
	RegisterCommand(TEXT("CompileBlueprint"), TEXT("Compiles a Blueprint asset."), TEXT("Asset"), ECommandRisk::ModifiesAsset, MakeAssetSchema(), &CompileBlueprint, MakeCompileBlueprintOutputSchema());
	RegisterCommand(TEXT("SaveAsset"), TEXT("Saves an asset package."), TEXT("Asset"), ECommandRisk::SavePackage, MakeAssetSchema(), &SaveAsset);

	RegisterCommand(TEXT("AddWidget"), TEXT("Adds a widget to a Widget Blueprint tree."), TEXT("WidgetEditing"), ECommandRisk::ModifiesAsset, MakeAddWidgetSchema(), &AddWidget);
	RegisterCommand(TEXT("SetRootWidget"), TEXT("Sets the root widget in a Widget Blueprint tree."), TEXT("WidgetEditing"), ECommandRisk::ModifiesAsset, MakeSetRootWidgetSchema(), &SetRootWidget);
	RegisterCommand(TEXT("AddWidgetToParent"), TEXT("Adds a widget to a parent widget."), TEXT("WidgetEditing"), ECommandRisk::ModifiesAsset, MakeAddWidgetToParentSchema(), &AddWidgetToParent);
	RegisterCommand(TEXT("SetWidgetSlotLayout"), TEXT("Sets slot layout data for a widget."), TEXT("WidgetEditing"), ECommandRisk::ModifiesAsset, MakeSetWidgetSlotLayoutSchema(), &SetWidgetSlotLayout);

	RegisterCommand(TEXT("SetBlueprintDefault"), TEXT("Sets a Blueprint class default value."), TEXT("BlueprintVariables"), ECommandRisk::ModifiesAsset, MakeSetBlueprintDefaultSchema(), &SetBlueprintDefault);
	RegisterCommand(TEXT("SetSubobjectDefault"), TEXT("Sets a Blueprint subobject default value."), TEXT("BlueprintVariables"), ECommandRisk::ModifiesAsset, MakeSetSubobjectDefaultSchema(), &SetSubobjectDefault);
	RegisterCommand(TEXT("SetBlueprintVariableFlags"), TEXT("Sets Blueprint variable flags."), TEXT("BlueprintVariables"), ECommandRisk::ModifiesAsset, MakeSetBlueprintVariableFlagsSchema(), &SetBlueprintVariableFlags);
	RegisterCommand(TEXT("AddBlueprintVariable"), TEXT("Adds a Blueprint member variable."), TEXT("BlueprintVariables"), ECommandRisk::ModifiesAsset, MakeAddBlueprintVariableSchema(), &AddBlueprintVariable);
}
} // namespace BlueprintBridge
