// Fill out your copyright notice in the Description page of Project Settings.

#include "PressableButtonComponent.h"
#include "Native/PressableButton.h"
#include "TouchPointer.h"
#include <GameFramework/Actor.h>
#include <DrawDebugHelpers.h>
#include <Components/ShapeComponent.h>

namespace HandUtils = Microsoft::MixedReality::HandUtils;
using namespace DirectX;


// Sets default values for this component's properties
UPressableButtonComponent::UPressableButtonComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = ETickingGroup::TG_PostPhysics;

	Extents.Set(10, 10, 10);
	PressedFraction = 0.5f;
	ReleasedFraction = 0.2f;
}

USceneComponent* UPressableButtonComponent::GetVisuals() const 
{
	return Cast<USceneComponent>(VisualsReference.GetComponent(GetOwner()));
}

void UPressableButtonComponent::SetVisuals(USceneComponent* Visuals)
{
	VisualsReference.OverrideComponent = Visuals;

	if (Visuals)
	{
		const auto VisualsOffset = Visuals->GetComponentLocation() - GetComponentLocation();
		VisualsOffsetLocal = GetComponentTransform().InverseTransformVector(VisualsOffset);
	}
}

static XMVECTOR ToXM(const FVector& vectorUE)
{
	return XMLoadFloat3((const XMFLOAT3*)&vectorUE);
}

static XMVECTOR ToXM(const FQuat& quaternion)
{
	return XMLoadFloat4A((const XMFLOAT4A*)&quaternion);
}

static FVector ToUE(XMVECTOR vectorXM)
{
	FVector vectorUE;
	XMStoreFloat3((XMFLOAT3*)&vectorUE, vectorXM);
	return vectorUE;
}

static FVector ToUEPosition(XMVECTOR vectorXM)
{
	return ToUE(XMVectorSwizzle<2, 0, 1, 3>(vectorXM) * g_XMNegateX);
}

static XMVECTOR ToMRPosition(const FVector& vectorUE)
{
	auto vectorXM = ToXM(vectorUE);
	return XMVectorSwizzle<1, 2, 0, 3>(vectorXM) * g_XMNegateZ;
}

static FQuat ToUERotation(XMVECTOR quaternionXM)
{
	FQuat quaternionUE;
	XMStoreFloat4A((XMFLOAT4A*)&quaternionUE, XMVectorSwizzle<2, 0, 1, 3>(quaternionXM) * g_XMNegateY * g_XMNegateZ);
	return quaternionUE;
}

static XMVECTOR ToMRRotation(const FQuat& quatUE)
{
	auto quatXM = ToXM(quatUE);
	return XMVectorSwizzle<1, 2, 0, 3>(quatXM) * g_XMNegateX * g_XMNegateY;
}

struct FButtonHandler : public HandUtils::IButtonHandler
{
	FButtonHandler(UPressableButtonComponent& PressableButtonComponent) : PressableButtonComponent(PressableButtonComponent) {}

	virtual void OnButtonHoverStart(HandUtils::PressableButton& button, HandUtils::PointerId pointerId) override;

	virtual void OnButtonHoverEnd(HandUtils::PressableButton& button, HandUtils::PointerId pointerId) override;

	virtual void OnButtonPressed(
		HandUtils::PressableButton& button,
		HandUtils::PointerId pointerId,
		DirectX::FXMVECTOR touchPoint) override;

	virtual void OnButtonReleased(
		HandUtils::PressableButton& button,
		HandUtils::PointerId pointerId) override;

	UPressableButtonComponent& PressableButtonComponent;
};

void FButtonHandler::OnButtonHoverStart(HandUtils::PressableButton& button, HandUtils::PointerId pointerId)
{
	// TODO Review use of raw pointers in events.
	PressableButtonComponent.OnButtonHoverStart.Broadcast(&PressableButtonComponent, reinterpret_cast<USceneComponent*>(pointerId));
}

void FButtonHandler::OnButtonHoverEnd(HandUtils::PressableButton& button, HandUtils::PointerId pointerId)
{
	PressableButtonComponent.OnButtonHoverEnd.Broadcast(&PressableButtonComponent, reinterpret_cast<USceneComponent*>(pointerId));
}

void FButtonHandler::OnButtonPressed(HandUtils::PressableButton& button, HandUtils::PointerId pointerId, DirectX::FXMVECTOR touchPoint)
{
	PressableButtonComponent.OnButtonPressed.Broadcast(&PressableButtonComponent);
}

void FButtonHandler::OnButtonReleased(HandUtils::PressableButton& button, HandUtils::PointerId pointerId)
{
	PressableButtonComponent.OnButtonReleased.Broadcast(&PressableButtonComponent);
}

// Called when the game starts
void UPressableButtonComponent::BeginPlay()
{
	Super::BeginPlay();

	const FTransform& Transform = GetComponentTransform();
	const auto WorldDimensions = 2 * Extents * Transform.GetScale3D();
	XMVECTOR Orientation = ToMRRotation(Transform.GetRotation());
	const auto RestPosition = Transform.GetTranslation();

	Button = new HandUtils::PressableButton(ToMRPosition(RestPosition), Orientation, WorldDimensions.Y, WorldDimensions.Z, WorldDimensions.X, PressedFraction * WorldDimensions.X, ReleasedFraction * WorldDimensions.X);
	Button->m_recoverySpeed = 50;

	ButtonHandler = new FButtonHandler(*this);
	Button->Subscribe(ButtonHandler);

	if (auto Visuals = GetVisuals())
	{
		const auto VisualsOffset = Visuals->GetComponentLocation() - GetComponentLocation();
		VisualsOffsetLocal = GetComponentTransform().InverseTransformVector(VisualsOffset);
	}
}

void UPressableButtonComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Button->Unsubscribe(ButtonHandler);
	delete ButtonHandler;
	ButtonHandler = nullptr;
	delete Button;
	Button = nullptr;

	Super::EndPlay(EndPlayReason);
}

// Called every frame
void UPressableButtonComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Update the button rest transform if the component one has changed
	{
		const FTransform& Transform = GetComponentTransform();
		const auto NewRestPosition = ToMRPosition(Transform.GetTranslation());
		const auto NewOrientation = ToMRRotation(Transform.GetRotation());
		const auto LinearEpsilon = XMVectorReplicate(0.01f);	// in cm
		const auto AngularEpsilon = XMVectorReplicate(0.0001f);

		if (!XMVector3NearEqual(Button->GetRestPosition(), NewRestPosition, LinearEpsilon) || 
			!XMVector4NearEqual(Button->GetOrientation(), NewOrientation, AngularEpsilon))
		{
			Button->SetRestTransform(NewRestPosition, NewOrientation);
		}
	}

	std::vector<HandUtils::TouchPointer> TouchPointers;

	// Collect all touch pointers
	{
		TArray<UTouchPointer*> Pointers = GetActivePointers();
		TouchPointers.reserve(Pointers.Num());

		for (UTouchPointer* Pointer : Pointers)
		{
			HandUtils::TouchPointer TouchPointer;
			const FVector PointerPosition = Pointer->GetComponentLocation();
			TouchPointer.m_position = ToMRPosition(PointerPosition);
			TouchPointer.m_id = (HandUtils::PointerId)Pointer;
			TouchPointers.emplace_back(TouchPointer);
		}
	}

	// Update button logic with all known pointers
	Button->Update(DeltaTime, TouchPointers.data(), TouchPointers.size());

	if (auto Visuals = GetVisuals())
	{
		// Update visuals position
		const auto VisualsOffset = GetComponentTransform().TransformVector(VisualsOffsetLocal);
		FVector NewLocation = ToUEPosition(Button->GetCurrentPosition()) + VisualsOffset;
		Visuals->SetWorldLocation(NewLocation);
	}

#if 0
	// Debug display
	{
		// Button face
		{
			FVector Position = ToUEPosition(Button->GetCurrentPosition());
			FQuat Orientation = ToUERotation(Button->GetOrientation());
			FPlane Plane(Position, -Orientation.GetForwardVector());
			FVector2D HalfExtents(Button->GetWidth(), Button->GetHeight());
			DrawDebugSolidPlane(GetWorld(), Plane, Position, 0.5f * HalfExtents, FColor::Blue);
		}

		// Pointers
		for (const auto& Pointer : TouchPointers)
		{
			auto Position = ToUEPosition(Pointer.m_position);

			// Shift it up a bit so it is not hidden by the pointer visuals.
			Position.Z += 2;

			DrawDebugPoint(GetWorld(), Position, 10, FColor::Yellow);
		}
	}
#endif
}

bool UPressableButtonComponent::GetClosestPointOnSurface_Implementation(const FVector& Point, FVector& OutPointOnSurface)
{
	FVector ButtonPosition = ToUEPosition(Button->GetCurrentPosition());
	FQuat ButtonOrientation = ToUERotation(Button->GetOrientation());	
	FVector PointLocal = ButtonOrientation.Inverse().RotateVector(Point - ButtonPosition);

	// In local space the button is a rectangle centered at the origin with -X normal
	const auto halfWidth = 0.5f * Button->GetWidth();
	const auto halfHeight = 0.5f * Button->GetHeight();
	FVector ClosestPointLocal { 0, FMath::Clamp(PointLocal.Y, -halfWidth, halfWidth), FMath::Clamp(PointLocal.Z, -halfHeight, halfHeight) };

	OutPointOnSurface = ButtonOrientation.RotateVector(ClosestPointLocal) + ButtonPosition;

	return true;
}
