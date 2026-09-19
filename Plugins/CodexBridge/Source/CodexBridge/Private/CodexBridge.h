#pragma once

#include "CoreMinimal.h"
#include "HttpResultCallback.h"
#include "Modules/ModuleManager.h"
#include "HttpRouteHandle.h"

class IHttpRouter;
struct FHttpServerRequest;
class FJsonObject;
class UBlueprint;
class UEdGraph;

class FCodexBridgeModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	static constexpr uint32 ServerPort = 31010;

	bool HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const;
	bool HandleInspectBlueprint(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const;
	bool HandleCompileBlueprint(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const;
	bool HandleAddBlueprintNode(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const;
	bool HandleConnectBlueprintPins(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const;
	bool HandleDisconnectBlueprintPin(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const;
	bool HandleSetBlueprintPinDefault(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const;
	bool HandleDeleteBlueprintNode(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const;
	bool HandleSaveBlueprint(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) const;

	bool ParseJsonBody(const FHttpServerRequest& Request, TSharedPtr<FJsonObject>& OutJson) const;
	bool SendJson(const FHttpResultCallback& OnComplete, const TSharedRef<FJsonObject>& Json) const;
	bool SendError(const FHttpResultCallback& OnComplete, const FString& Message) const;

	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> RouteHandles;
};
