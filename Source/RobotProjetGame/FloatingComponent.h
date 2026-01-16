#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "FloatingComponent.generated.h"

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class ROBOTPROJETGAME_API UFloatingComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UFloatingComponent();

protected:
	virtual void BeginPlay() override;

public:
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction
	) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Floating")
	float Amplitude = 20.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Floating")
	float Speed = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Floating")
	float Phase = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Floating")
	FVector Axis = FVector(0, 0, 1);

private:
	FVector InitialRelativeLocation;
	float Time;
};