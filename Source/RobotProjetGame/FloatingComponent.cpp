#include "FloatingComponent.h"

UFloatingComponent::UFloatingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UFloatingComponent::BeginPlay()
{
	Super::BeginPlay();

	InitialRelativeLocation = GetRelativeLocation();
	Time = Phase;
}

void UFloatingComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction
)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	Time += DeltaTime * Speed;

	float Offset = FMath::Sin(Time) * Amplitude;
	FVector NewLocation = InitialRelativeLocation + Axis.GetSafeNormal() * Offset;

	SetRelativeLocation(NewLocation);
}
