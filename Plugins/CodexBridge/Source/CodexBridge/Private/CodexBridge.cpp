#include "CodexBridge.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <EdGraph/EdGraphSchema.h>
#include <Editor.h>
#include <K2Node_CallFunction.h>
#include <K2Node_CustomEvent.h>
#include <K2Node_IfThenElse.h>
#include <K2Node_Knot.h>
#include <K2Node_VariableGet.h>
#include <K2Node_VariableSet.h>
#include <Kismet2/BlueprintEditorUtils.h>
#include <Kismet/KismetSystemLibrary.h>
#include <ScopedTransaction.h>
#include <UObject/SavePackage.h>

IMPLEMENT_MODULE(FCodexBridgeModule, CodexBridge)

namespace CodexBridgePrivate
{
	FString RequestBodyToString(const FHttpServerRequest& Request)
	{
		if (Request.Body.Num() == 0)
		{
			return FString();
		}

		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		return FString(Converted.Get(), Converted.Length());
	}

	void AddGraphDetails(const UEdGraph* Graph, TArray<TSharedPtr<FJsonValue>>& OutGraphs, const FString& GraphType)
	{
		if (!Graph)
		{
			return;
		}

		TSharedRef<FJsonObject> GraphJson = MakeShared<FJsonObject>();
		GraphJson->SetStringField(TEXT("name"), Graph->GetName());
		GraphJson->SetStringField(TEXT("path"), Graph->GetPathName());
		GraphJson->SetStringField(TEXT("type"), GraphType);

		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
			NodeJson->SetStringField(TEXT("id"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
			NodeJson->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			NodeJson->SetStringField(TEXT("comment"), Node->NodeComment);
			NodeJson->SetNumberField(TEXT("x"), Node->NodePosX);
			NodeJson->SetNumberField(TEXT("y"), Node->NodePosY);

			TArray<TSharedPtr<FJsonValue>> Pins;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}

				TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
				PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
				PinJson->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
				PinJson->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
				PinJson->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
				PinJson->SetStringField(TEXT("default_value"), Pin->DefaultValue);

				TArray<TSharedPtr<FJsonValue>> LinkedNodeIds;
				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (LinkedPin && LinkedPin->GetOwningNode())
					{
						LinkedNodeIds.Add(MakeShared<FJsonValueString>(LinkedPin->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)));
					}
				}
				PinJson->SetArrayField(TEXT("linked_node_ids"), LinkedNodeIds);
				Pins.Add(MakeShared<FJsonValueObject>(PinJson));
			}

			NodeJson->SetArrayField(TEXT("pins"), Pins);
			Nodes.Add(MakeShared<FJsonValueObject>(NodeJson));
		}

		GraphJson->SetArrayField(TEXT("nodes"), Nodes);
		OutGraphs.Add(MakeShared<FJsonValueObject>(GraphJson));
	}
}

namespace CodexBridgePrivate
{
bool RequireConfirmedWrite(const TSharedPtr<FJsonObject>& Input, FString& OutError)
{
	bool bConfirmed = false;
	if (!Input.IsValid() || !Input->TryGetBoolField(TEXT("confirm"), bConfirmed) || !bConfirmed)
	{
		OutError = TEXT("Write operations require confirm=true.");
		return false;
	}
	return true;
}

bool EnsureEditorIdle(FString& OutError)
{
	if (GEditor && (GEditor->PlayWorld != nullptr || GEditor->bIsSimulatingInEditor))
	{
		OutError = TEXT("Blueprint writes are disabled while PIE or simulation is active.");
		return false;
	}
	return true;
}

UBlueprint* LoadBlueprintFromInput(const TSharedPtr<FJsonObject>& Input, FString& OutError)
{
	FString AssetPath;
	if (!Input.IsValid() || !Input->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("The asset_path field is required.");
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
	}
	return Blueprint;
}

UEdGraph* FindBlueprintGraph(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Input, FString& OutError)
{
	FString GraphPath;
	Input->TryGetStringField(TEXT("graph_path"), GraphPath);
	FString GraphName;
	Input->TryGetStringField(TEXT("graph_name"), GraphName);
	if (GraphPath.IsEmpty() && GraphName.IsEmpty())
	{
		OutError = TEXT("The graph_path or graph_name field is required.");
		return nullptr;
	}

	TArray<UEdGraph*> Graphs;
	Graphs.Append(Blueprint->UbergraphPages);
	Graphs.Append(Blueprint->FunctionGraphs);
	Graphs.Append(Blueprint->MacroGraphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph && ((!GraphPath.IsEmpty() && Graph->GetPathName() == GraphPath) || (!GraphName.IsEmpty() && Graph->GetName() == GraphName)))
		{
			return Graph;
		}
	}

	OutError = FString::Printf(TEXT("Blueprint graph not found: %s"), GraphPath.IsEmpty() ? *GraphName : *GraphPath);
	return nullptr;
}

UEdGraphNode* FindBlueprintNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Input, const TCHAR* FieldName, FString& OutError)
{
	FString NodeId;
	if (!Input->TryGetStringField(FieldName, NodeId) || NodeId.IsEmpty())
	{
		OutError = FString::Printf(TEXT("The %s field is required."), FieldName);
		return nullptr;
	}

	FGuid Guid;
	if (!FGuid::Parse(NodeId, Guid))
	{
		OutError = FString::Printf(TEXT("Invalid node GUID: %s"), *NodeId);
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == Guid)
		{
			return Node;
		}
	}

	OutError = FString::Printf(TEXT("Node not found: %s"), *NodeId);
	return nullptr;
}

UEdGraphPin* FindBlueprintPin(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Input, const TCHAR* FieldName, FString& OutError)
{
	FString PinName;
	if (!Input->TryGetStringField(FieldName, PinName) || PinName.IsEmpty())
	{
		OutError = FString::Printf(TEXT("The %s field is required."), FieldName);
		return nullptr;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName.ToString() == PinName)
		{
			return Pin;
		}
	}

	OutError = FString::Printf(TEXT("Pin not found on node: %s"), *PinName);
	return nullptr;
}

void MarkBlueprintEdited(UBlueprint* Blueprint)
{
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
}

}

void FCodexBridgeModule::StartupModule()
{
	if (IsRunningCommandlet())
	{
		return;
	}

	FHttpServerModule& HttpServer = FModuleManager::LoadModuleChecked<FHttpServerModule>(TEXT("HTTPServer"));
	Router = HttpServer.GetHttpRouter(ServerPort, true);
	if (!Router.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("CodexBridge could not bind localhost:%u. The port may already be in use."), ServerPort);
		return;
	}

	RouteHandles.Add(Router->BindRoute(
		FHttpPath(TEXT("/codex/health")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleHealth(Request, OnComplete);
		})));

	RouteHandles.Add(Router->BindRoute(
		FHttpPath(TEXT("/codex/blueprint/inspect")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleInspectBlueprint(Request, OnComplete);
		})));

	RouteHandles.Add(Router->BindRoute(
		FHttpPath(TEXT("/codex/blueprint/compile")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleCompileBlueprint(Request, OnComplete);
		})));

	RouteHandles.Add(Router->BindRoute(
		FHttpPath(TEXT("/codex/blueprint/add_node")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleAddBlueprintNode(Request, OnComplete);
		})));

	RouteHandles.Add(Router->BindRoute(
		FHttpPath(TEXT("/codex/blueprint/connect_pins")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleConnectBlueprintPins(Request, OnComplete);
		})));

	RouteHandles.Add(Router->BindRoute(
		FHttpPath(TEXT("/codex/blueprint/disconnect_pin")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleDisconnectBlueprintPin(Request, OnComplete);
		})));

	RouteHandles.Add(Router->BindRoute(
		FHttpPath(TEXT("/codex/blueprint/set_pin_default")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleSetBlueprintPinDefault(Request, OnComplete);
		})));

	RouteHandles.Add(Router->BindRoute(
		FHttpPath(TEXT("/codex/blueprint/delete_node")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleDeleteBlueprintNode(Request, OnComplete);
		})));

	RouteHandles.Add(Router->BindRoute(
		FHttpPath(TEXT("/codex/blueprint/save")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleSaveBlueprint(Request, OnComplete);
		})));

	HttpServer.StartAllListeners();
	UE_LOG(LogTemp, Log, TEXT("CodexBridge listening on http://127.0.0.1:%u"), ServerPort);
}

void FCodexBridgeModule::ShutdownModule()
{
	if (Router.IsValid())
	{
		for (const FHttpRouteHandle& RouteHandle : RouteHandles)
		{
			if (RouteHandle.IsValid())
			{
				Router->UnbindRoute(RouteHandle);
			}
		}
	}

	RouteHandles.Reset();
	Router.Reset();
}

bool FCodexBridgeModule::ParseJsonBody(const FHttpServerRequest& Request, TSharedPtr<FJsonObject>& OutJson) const
{
	const FString Body = CodexBridgePrivate::RequestBodyToString(Request);
	if (Body.IsEmpty())
	{
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	return FJsonSerializer::Deserialize(Reader, OutJson) && OutJson.IsValid();
}

bool FCodexBridgeModule::SendJson(const FHttpResultCallback& OnComplete, const TSharedRef<FJsonObject>& Json) const
{
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json, Writer);
	Writer->Close();
	OnComplete(FHttpServerResponse::Create(Output, TEXT("application/json")));
	return true;
}

bool FCodexBridgeModule::SendError(const FHttpResultCallback& OnComplete, const FString& Message) const
{
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetBoolField(TEXT("ok"), false);
	Error->SetStringField(TEXT("error"), Message);
	return SendJson(OnComplete, Error);
}

bool FCodexBridgeModule::HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const
{
	TSharedRef<FJsonObject> Health = MakeShared<FJsonObject>();
	Health->SetBoolField(TEXT("ok"), true);
	Health->SetStringField(TEXT("service"), TEXT("CodexBridge"));
	Health->SetStringField(TEXT("engine"), TEXT("Unreal Engine 5.8"));
	Health->SetNumberField(TEXT("port"), ServerPort);
	return SendJson(OnComplete, Health);
}

bool FCodexBridgeModule::HandleInspectBlueprint(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const
{
	TSharedPtr<FJsonObject> Input;
	if (!ParseJsonBody(Request, Input))
	{
		return SendError(OnComplete, TEXT("Request body must be valid JSON."));
	}

	FString AssetPath;
	if (!Input->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return SendError(OnComplete, TEXT("The asset_path field is required."));
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return SendError(OnComplete, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());

	TArray<TSharedPtr<FJsonValue>> Graphs;
	for (const UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		CodexBridgePrivate::AddGraphDetails(Graph, Graphs, TEXT("event"));
	}
	for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		CodexBridgePrivate::AddGraphDetails(Graph, Graphs, TEXT("function"));
	}
	for (const UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		CodexBridgePrivate::AddGraphDetails(Graph, Graphs, TEXT("macro"));
	}
	Result->SetArrayField(TEXT("graphs"), Graphs);

	return SendJson(OnComplete, Result);
}

bool FCodexBridgeModule::HandleAddBlueprintNode(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const
{
	TSharedPtr<FJsonObject> Input;
	if (!ParseJsonBody(Request, Input))
	{
		return SendError(OnComplete, TEXT("Request body must be valid JSON."));
	}

	FString Error;
	if (!CodexBridgePrivate::RequireConfirmedWrite(Input, Error) || !CodexBridgePrivate::EnsureEditorIdle(Error))
	{
		return SendError(OnComplete, Error);
	}

	UBlueprint* Blueprint = CodexBridgePrivate::LoadBlueprintFromInput(Input, Error);
	UEdGraph* Graph = Blueprint ? CodexBridgePrivate::FindBlueprintGraph(Blueprint, Input, Error) : nullptr;
	if (!Blueprint || !Graph)
	{
		return SendError(OnComplete, Error);
	}

	FString NodeType;
	if (!Input->TryGetStringField(TEXT("node_type"), NodeType) || NodeType.IsEmpty())
	{
		return SendError(OnComplete, TEXT("The node_type field is required. Supported values: branch, delay, call_function, variable_get, variable_set, custom_event, reroute."));
	}

	double XValue = 0.0;
	double YValue = 0.0;
	Input->TryGetNumberField(TEXT("x"), XValue);
	Input->TryGetNumberField(TEXT("y"), YValue);

	FScopedTransaction Transaction(FText::FromString(TEXT("CodexBridge Add Blueprint Node")));
	Blueprint->Modify();
	Graph->Modify();
	UEdGraphNode* Node = nullptr;

	if (NodeType == TEXT("branch"))
	{
		Node = NewObject<UK2Node_IfThenElse>(Graph);
	}
	else if (NodeType == TEXT("delay"))
	{
		UK2Node_CallFunction* DelayNode = NewObject<UK2Node_CallFunction>(Graph);
		DelayNode->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, Delay), UKismetSystemLibrary::StaticClass());
		Node = DelayNode;
	}
	else if (NodeType == TEXT("reroute"))
	{
		Node = NewObject<UK2Node_Knot>(Graph);
	}
	else if (NodeType == TEXT("call_function"))
	{
		FString FunctionName;
		FString OwnerClassPath;
		if (!Input->TryGetStringField(TEXT("function_name"), FunctionName) || !Input->TryGetStringField(TEXT("owner_class"), OwnerClassPath))
		{
			return SendError(OnComplete, TEXT("call_function requires function_name and owner_class."));
		}
		UClass* OwnerClass = LoadObject<UClass>(nullptr, *OwnerClassPath);
		if (!OwnerClass)
		{
			return SendError(OnComplete, FString::Printf(TEXT("Owner class not found: %s"), *OwnerClassPath));
		}
		UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
		CallNode->FunctionReference.SetExternalMember(FName(*FunctionName), OwnerClass);
		Node = CallNode;
	}
	else if (NodeType == TEXT("variable_get") || NodeType == TEXT("variable_set"))
	{
		FString VariableName;
		if (!Input->TryGetStringField(TEXT("variable_name"), VariableName))
		{
			return SendError(OnComplete, TEXT("variable_get and variable_set require variable_name."));
		}
		if (NodeType == TEXT("variable_get"))
		{
			UK2Node_VariableGet* VariableNode = NewObject<UK2Node_VariableGet>(Graph);
			VariableNode->VariableReference.SetSelfMember(FName(*VariableName));
			Node = VariableNode;
		}
		else
		{
			UK2Node_VariableSet* VariableNode = NewObject<UK2Node_VariableSet>(Graph);
			VariableNode->VariableReference.SetSelfMember(FName(*VariableName));
			Node = VariableNode;
		}
	}
	else if (NodeType == TEXT("custom_event"))
	{
		FString EventName;
		if (!Input->TryGetStringField(TEXT("event_name"), EventName))
		{
			return SendError(OnComplete, TEXT("custom_event requires event_name."));
		}
		UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(Graph);
		EventNode->CustomFunctionName = FName(*EventName);
		Node = EventNode;
	}
	else
	{
		return SendError(OnComplete, FString::Printf(TEXT("Unsupported node_type: %s"), *NodeType));
	}

	Graph->AddNode(Node, true, true);
	Node->CreateNewGuid();
	Node->PostPlacedNewNode();
	Node->AllocateDefaultPins();
	Node->NodePosX = static_cast<int32>(XValue);
	Node->NodePosY = static_cast<int32>(YValue);
	FString Comment;
	if (Input->TryGetStringField(TEXT("comment"), Comment))
	{
		Node->NodeComment = Comment;
	}
	CodexBridgePrivate::MarkBlueprintEdited(Blueprint);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("graph_path"), Graph->GetPathName());
	Result->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("node_class"), Node->GetClass()->GetPathName());
	Result->SetBoolField(TEXT("requires_compile"), true);
	Result->SetBoolField(TEXT("requires_save"), true);
	return SendJson(OnComplete, Result);
}

bool FCodexBridgeModule::HandleConnectBlueprintPins(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const
{
	TSharedPtr<FJsonObject> Input;
	if (!ParseJsonBody(Request, Input))
	{
		return SendError(OnComplete, TEXT("Request body must be valid JSON."));
	}

	FString Error;
	if (!CodexBridgePrivate::RequireConfirmedWrite(Input, Error) || !CodexBridgePrivate::EnsureEditorIdle(Error))
	{
		return SendError(OnComplete, Error);
	}
	UBlueprint* Blueprint = CodexBridgePrivate::LoadBlueprintFromInput(Input, Error);
	UEdGraph* Graph = Blueprint ? CodexBridgePrivate::FindBlueprintGraph(Blueprint, Input, Error) : nullptr;
	UEdGraphNode* SourceNode = Graph ? CodexBridgePrivate::FindBlueprintNode(Graph, Input, TEXT("source_node_id"), Error) : nullptr;
	UEdGraphNode* TargetNode = SourceNode ? CodexBridgePrivate::FindBlueprintNode(Graph, Input, TEXT("target_node_id"), Error) : nullptr;
	UEdGraphPin* SourcePin = TargetNode ? CodexBridgePrivate::FindBlueprintPin(SourceNode, Input, TEXT("source_pin"), Error) : nullptr;
	UEdGraphPin* TargetPin = SourcePin ? CodexBridgePrivate::FindBlueprintPin(TargetNode, Input, TEXT("target_pin"), Error) : nullptr;
	if (!Blueprint || !Graph || !SourcePin || !TargetPin)
	{
		return SendError(OnComplete, Error);
	}

	if (SourcePin->Direction == EGPD_Input && TargetPin->Direction == EGPD_Output)
	{
		Swap(SourcePin, TargetPin);
	}
	if (SourcePin->Direction != EGPD_Output || TargetPin->Direction != EGPD_Input)
	{
		return SendError(OnComplete, TEXT("connect_pins requires an output source pin and an input target pin."));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("CodexBridge Connect Blueprint Pins")));
	Blueprint->Modify();
	Graph->Modify();
	SourcePin->GetOwningNode()->Modify();
	TargetPin->GetOwningNode()->Modify();
	if (!Graph->GetSchema()->TryCreateConnection(SourcePin, TargetPin))
	{
		return SendError(OnComplete, TEXT("Unreal rejected the pin connection. Check pin types and existing links."));
	}
	CodexBridgePrivate::MarkBlueprintEdited(Blueprint);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetBoolField(TEXT("requires_compile"), true);
	Result->SetBoolField(TEXT("requires_save"), true);
	return SendJson(OnComplete, Result);
}

bool FCodexBridgeModule::HandleDisconnectBlueprintPin(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const
{
	TSharedPtr<FJsonObject> Input;
	if (!ParseJsonBody(Request, Input))
	{
		return SendError(OnComplete, TEXT("Request body must be valid JSON."));
	}
	FString Error;
	if (!CodexBridgePrivate::RequireConfirmedWrite(Input, Error) || !CodexBridgePrivate::EnsureEditorIdle(Error))
	{
		return SendError(OnComplete, Error);
	}
	UBlueprint* Blueprint = CodexBridgePrivate::LoadBlueprintFromInput(Input, Error);
	UEdGraph* Graph = Blueprint ? CodexBridgePrivate::FindBlueprintGraph(Blueprint, Input, Error) : nullptr;
	UEdGraphNode* Node = Graph ? CodexBridgePrivate::FindBlueprintNode(Graph, Input, TEXT("node_id"), Error) : nullptr;
	UEdGraphPin* Pin = Node ? CodexBridgePrivate::FindBlueprintPin(Node, Input, TEXT("pin_name"), Error) : nullptr;
	if (!Blueprint || !Graph || !Pin)
	{
		return SendError(OnComplete, Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("CodexBridge Disconnect Blueprint Pin")));
	Blueprint->Modify();
	Graph->Modify();
	Node->Modify();
	Pin->BreakAllPinLinks(true);
	CodexBridgePrivate::MarkBlueprintEdited(Blueprint);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetBoolField(TEXT("requires_compile"), true);
	Result->SetBoolField(TEXT("requires_save"), true);
	return SendJson(OnComplete, Result);
}

bool FCodexBridgeModule::HandleSetBlueprintPinDefault(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const
{
	TSharedPtr<FJsonObject> Input;
	if (!ParseJsonBody(Request, Input))
	{
		return SendError(OnComplete, TEXT("Request body must be valid JSON."));
	}
	FString Error;
	if (!CodexBridgePrivate::RequireConfirmedWrite(Input, Error) || !CodexBridgePrivate::EnsureEditorIdle(Error))
	{
		return SendError(OnComplete, Error);
	}
	FString Value;
	if (!Input->TryGetStringField(TEXT("value"), Value))
	{
		return SendError(OnComplete, TEXT("The value field is required."));
	}
	UBlueprint* Blueprint = CodexBridgePrivate::LoadBlueprintFromInput(Input, Error);
	UEdGraph* Graph = Blueprint ? CodexBridgePrivate::FindBlueprintGraph(Blueprint, Input, Error) : nullptr;
	UEdGraphNode* Node = Graph ? CodexBridgePrivate::FindBlueprintNode(Graph, Input, TEXT("node_id"), Error) : nullptr;
	UEdGraphPin* Pin = Node ? CodexBridgePrivate::FindBlueprintPin(Node, Input, TEXT("pin_name"), Error) : nullptr;
	if (!Blueprint || !Graph || !Pin)
	{
		return SendError(OnComplete, Error);
	}
	if (Pin->Direction != EGPD_Input)
	{
		return SendError(OnComplete, TEXT("Only input pins can receive a default value."));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("CodexBridge Set Blueprint Pin Default")));
	Blueprint->Modify();
	Graph->Modify();
	Node->Modify();
	Graph->GetSchema()->TrySetDefaultValue(*Pin, Value, true);
	CodexBridgePrivate::MarkBlueprintEdited(Blueprint);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetBoolField(TEXT("requires_compile"), true);
	Result->SetBoolField(TEXT("requires_save"), true);
	return SendJson(OnComplete, Result);
}

bool FCodexBridgeModule::HandleDeleteBlueprintNode(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const
{
	TSharedPtr<FJsonObject> Input;
	if (!ParseJsonBody(Request, Input))
	{
		return SendError(OnComplete, TEXT("Request body must be valid JSON."));
	}
	FString Error;
	if (!CodexBridgePrivate::RequireConfirmedWrite(Input, Error) || !CodexBridgePrivate::EnsureEditorIdle(Error))
	{
		return SendError(OnComplete, Error);
	}
	UBlueprint* Blueprint = CodexBridgePrivate::LoadBlueprintFromInput(Input, Error);
	UEdGraph* Graph = Blueprint ? CodexBridgePrivate::FindBlueprintGraph(Blueprint, Input, Error) : nullptr;
	UEdGraphNode* Node = Graph ? CodexBridgePrivate::FindBlueprintNode(Graph, Input, TEXT("node_id"), Error) : nullptr;
	if (!Blueprint || !Graph || !Node)
	{
		return SendError(OnComplete, Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("CodexBridge Delete Blueprint Node")));
	Blueprint->Modify();
	Graph->Modify();
	Node->Modify();
	Graph->RemoveNode(Node);
	CodexBridgePrivate::MarkBlueprintEdited(Blueprint);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetBoolField(TEXT("requires_compile"), true);
	Result->SetBoolField(TEXT("requires_save"), true);
	return SendJson(OnComplete, Result);
}

bool FCodexBridgeModule::HandleSaveBlueprint(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const
{
	TSharedPtr<FJsonObject> Input;
	if (!ParseJsonBody(Request, Input))
	{
		return SendError(OnComplete, TEXT("Request body must be valid JSON."));
	}
	FString Error;
	if (!CodexBridgePrivate::RequireConfirmedWrite(Input, Error) || !CodexBridgePrivate::EnsureEditorIdle(Error))
	{
		return SendError(OnComplete, Error);
	}
	UBlueprint* Blueprint = CodexBridgePrivate::LoadBlueprintFromInput(Input, Error);
	if (!Blueprint)
	{
		return SendError(OnComplete, Error);
	}

	UPackage* Package = Blueprint->GetOutermost();
	const FString PackageName = Package->GetName();
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_None;
	const bool bSaved = UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs);
	if (!bSaved)
	{
		return SendError(OnComplete, FString::Printf(TEXT("Failed to save Blueprint package: %s"), *PackageFilename));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("package_filename"), PackageFilename);
	Result->SetBoolField(TEXT("saved"), true);
	return SendJson(OnComplete, Result);
}

bool FCodexBridgeModule::HandleCompileBlueprint(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const
{
	TSharedPtr<FJsonObject> Input;
	if (!ParseJsonBody(Request, Input))
	{
		return SendError(OnComplete, TEXT("Request body must be valid JSON."));
	}

	FString AssetPath;
	if (!Input->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return SendError(OnComplete, TEXT("The asset_path field is required."));
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return SendError(OnComplete, FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FCompilerResultsLog ResultsLog;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave, &ResultsLog);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), ResultsLog.NumErrors == 0);
	Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Result->SetNumberField(TEXT("errors"), ResultsLog.NumErrors);
	Result->SetNumberField(TEXT("warnings"), ResultsLog.NumWarnings);
	Result->SetStringField(TEXT("message"), ResultsLog.NumErrors == 0 ? TEXT("Blueprint compiled successfully.") : TEXT("Blueprint compilation failed."));
	return SendJson(OnComplete, Result);
}
