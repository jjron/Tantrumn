// Fill out your copyright notice in the Description page of Project Settings.


#include "TantrumnCharacterBase.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "TantrumnPlayerController.h"
#include "ThrowableActor.h"
#include "Net/UnrealNetwork.h"
#include "TantrumnGameInstance.h"
#include "TantrumnPlayerState.h"
#include "DrawDebugHelpers.h"
#include "VisualLogger/VisualLogger.h"

constexpr int CVSphereCastPlayerView = 0;
constexpr int CVSphereCastActorTransform = 1;
constexpr int CVLineCastActorTransform = 2;

static TAutoConsoleVariable<int> CVarTraceMode(
	TEXT("Tantrumn.Character.Debug.TraceMode"),
	0,
	TEXT("  0: Sphere cast PlayerView used for direction/rotation \n")
	TEXT("  1: Sphere cast using ActorTransform \n")
	TEXT("  2: Line cast using ActorTransform \n"),
	ECVF_Default
);

static TAutoConsoleVariable<bool> CVarDisplayTrace(
	TEXT("Tantrumn.Character.Debug.DisplayTrace"),
	false,
	TEXT("Display Trace"),
	ECVF_Default
);

static TAutoConsoleVariable<bool> CVarDisplayThrowVelocity(
	TEXT("Tantrumn.Character.Debug.DisplayThrowVelocity"),
	false,
	TEXT("Display Throw Velocity"),
	ECVF_Default
);

DEFINE_LOG_CATEGORY_STATIC(LogTantrumnChar,Verbose, Verbose)

// Sets default values
ATantrumnCharacterBase::ATantrumnCharacterBase()
{
 	// Set this character to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicateMovement(true);
}

void ATantrumnCharacterBase::GetLifetimeReplicatedProps(TArray< FLifetimeProperty >& OutLifetimeProps) const {
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	FDoRepLifetimeParams SharedParams;
	SharedParams.bIsPushBased = true;
	SharedParams.Condition = COND_SkipOwner;

	DOREPLIFETIME_WITH_PARAMS_FAST(ATantrumnCharacterBase, CharacterThrowState, SharedParams);

	SharedParams.Condition = COND_None;
	DOREPLIFETIME_WITH_PARAMS_FAST(ATantrumnCharacterBase, LastGroundPosition, SharedParams);

	//DOREPLIFETIME(ATantrumnCharacterBase, CharacterThrowState);
}

// Called when the game starts or when spawned
void ATantrumnCharacterBase::BeginPlay()
{
	Super::BeginPlay();
	EffectCooldown = DefaultEffectCooldown;
	if (GetCharacterMovement()) {
		MaxWalkSpeed = GetCharacterMovement()->MaxWalkSpeed;
	}
	
}

// Called every frame
void ATantrumnCharacterBase::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (CharacterThrowState == ECharacterThrowState::Throwing) {
		UpdateThrowMontagePlayRate();
		return;
	}

	if (!IsLocallyControlled()) {
		return;
	}

	if (bIsStunned) {
		UpdateStun(DeltaTime);
		return;
	}

	if (bIsUnderEffect) {
		UpdateEffect(DeltaTime);
		return;
	}

	//check that player can pick up objects
	if (CharacterThrowState == ECharacterThrowState::None || CharacterThrowState == ECharacterThrowState::RequestingPull) {
		switch (CVarTraceMode->GetInt()) {
			case CVSphereCastPlayerView:
				SphereCastPlayerView();
				break;
			case CVSphereCastActorTransform:
				SphereCastActorTransform();
				break;
			case CVLineCastActorTransform:
				LineCastActorTransform();
				break;
			default:
				SphereCastPlayerView();
				break;
		}
	}
}

// Called to bind functionality to input
void ATantrumnCharacterBase::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

}

void ATantrumnCharacterBase::Landed(const FHitResult& Hit) {
	Super::Landed(Hit);

	ATantrumnPlayerController* TantrumnPlayerController = GetController<ATantrumnPlayerController>();
	if (TantrumnPlayerController) {
		const float FallImpactSpeed = FMath::Abs(GetVelocity().Z);
		if (FallImpactSpeed < MinImpactSpeed) {
			return;
		}

		const float DeltaImpact = MaxImpactSpeed - MinImpactSpeed;
		const float FallRatio = FMath::Clamp((FallImpactSpeed - MinImpactSpeed) / DeltaImpact, 0.0f, 1.0f);
		const bool bAffectSmall = FallRatio <= 0.5;
		const bool bAffectLarge = FallRatio > 0.5;
		TantrumnPlayerController->PlayDynamicForceFeedback(FallRatio, 0.5f, bAffectLarge, bAffectSmall, bAffectLarge, bAffectSmall);
		if (bAffectLarge) {
			OnStunBegin(FallRatio);
		}
	}
}

void ATantrumnCharacterBase::RequestSprintStart() {
	if (!bIsStunned) {
		GetCharacterMovement()->MaxWalkSpeed = SprintSpeed;
		ServerSprintStart();
	}
}

void ATantrumnCharacterBase::RequestSprintEnd() {
	GetCharacterMovement()->MaxWalkSpeed = MaxWalkSpeed;
	ServerSprintEnd();
}

void ATantrumnCharacterBase::ServerSprintStart_Implementation() {
	GetCharacterMovement()->MaxWalkSpeed = SprintSpeed;
}

void ATantrumnCharacterBase::ServerSprintEnd_Implementation() {
	GetCharacterMovement()->MaxWalkSpeed = MaxWalkSpeed;
}

void ATantrumnCharacterBase::OnStunBegin(float StunRatio) {
	if (bIsStunned) {
		//for now just early exit, alternative option would be to add to the stun time
		return;
	}

	const float StunDelta = MaxStunTime - MinStunTime;
	StunTime = MinStunTime + (StunRatio * StunDelta);
	CurrentStunTimer = 0.0f;
	//StunBeginTimestamp = FApp::GetCurrentTime();
	bIsStunned = true;
	if (bIsSprinting) {
		RequestSprintEnd();
	}
	ResetThrowableObject();
}

void ATantrumnCharacterBase::UpdateStun(float DeltaTime) {
	if (bIsStunned) {
		CurrentStunTimer += DeltaTime;
		bIsStunned = CurrentStunTimer < StunTime;
		if (!bIsStunned) {
			OnStunEnd();
		}
	}
}

void ATantrumnCharacterBase::OnStunEnd() {
	CurrentStunTimer = 0.0f;
	//StunBeginTimestamp = 0.0f;
	StunTime = 0.0f;
}

void ATantrumnCharacterBase::RequestThrowObject() {
	if (CanThrowObject()) {
		if (PlayThrowMontage()) {
			CharacterThrowState = ECharacterThrowState::Throwing;
			ServerRequestThrowObject();
		}
		else {
			ResetThrowableObject();
		}
	}
}

bool ATantrumnCharacterBase::ServerRequestThrowObject_Validate() {
	// can check the state of if the throwable actor exists etc to prevent this being broadcasted
	return true;
}

void ATantrumnCharacterBase::ServerRequestThrowObject_Implementation() {
	// server needs to call the multicast
	MulticastRequestThrowObject();
}

void ATantrumnCharacterBase::MulticastRequestThrowObject_Implementation() {
	// locally controlled actor has already set up binding and played montage
	if (IsLocallyControlled()) {
		return;
	}

	PlayThrowMontage();
	// transition out of camera if coming from aiming
	CharacterThrowState = ECharacterThrowState::Throwing;
}

void ATantrumnCharacterBase::RequestPullObject() {
	//make sure we are in idle
	if (!bIsStunned && CharacterThrowState == ECharacterThrowState::None) {
		CharacterThrowState = ECharacterThrowState::RequestingPull;
		ServerRequestPullObject(true);
	}
}

bool ATantrumnCharacterBase::AttemptPullObjectAtLocation(const FVector& InLocation) {
	if (CharacterThrowState != ECharacterThrowState::None && CharacterThrowState != ECharacterThrowState::RequestingPull) {
		return false;
	}

	FVector StartPos = GetActorLocation();
	FVector EndPos = InLocation;
	FHitResult HitResult;
	GetWorld() ? GetWorld()->LineTraceSingleByChannel(HitResult, StartPos, EndPos, ECollisionChannel::ECC_Visibility) : false;
#if ENABLE_DRAW_DEBUG
	if (CVarDisplayTrace->GetBool()) {
		DrawDebugLine(GetWorld(), StartPos, EndPos, HitResult.bBlockingHit ? FColor::Red : FColor::White, false);
	}
#endif
	CharacterThrowState = ECharacterThrowState::RequestingPull;
	ProcessTraceResult(HitResult, false);
	if (CharacterThrowState == ECharacterThrowState::Pulling) {
		return true;
	}

	CharacterThrowState = ECharacterThrowState::None;
	return false;
}

void ATantrumnCharacterBase::RequestStopPullObject() {
	//if pulling an object, drop it
	if (CharacterThrowState == ECharacterThrowState::RequestingPull) {
		CharacterThrowState = ECharacterThrowState::None;
		ServerRequestPullObject(false);
		//ResetThrowableObject();
	}
}

void ATantrumnCharacterBase::ServerRequestPullObject_Implementation(bool bIsPulling) {
	CharacterThrowState = bIsPulling ? ECharacterThrowState::RequestingPull : ECharacterThrowState::None;
}

void ATantrumnCharacterBase::ServerPullObject_Implementation(AThrowableActor* InThrowableActor) {
	if (InThrowableActor && InThrowableActor->Pull(this)) {
		CharacterThrowState = ECharacterThrowState::Pulling;
		ThrowableActor = InThrowableActor;
		ThrowableActor->ToggleHighlight(false);
	}
}

void ATantrumnCharacterBase::ClientThrowableAttached_Implementation(AThrowableActor* InThrowableActor) {
	CharacterThrowState = ECharacterThrowState::Attached;
	ThrowableActor = InThrowableActor;
	MoveIgnoreActorAdd(ThrowableActor);
}

void ATantrumnCharacterBase::ServerBeginThrow_Implementation() {
	//ignore collisions or else the throwable actor hits the player capsule
	if (ThrowableActor->GetRootComponent()) {
		UPrimitiveComponent* RootPrimitiveComponent = Cast<UPrimitiveComponent>(ThrowableActor->GetRootComponent());
		if (RootPrimitiveComponent) {
			RootPrimitiveComponent->IgnoreActorWhenMoving(this, true);
		}
	}
	const FVector& Direction = GetActorForwardVector() * ThrowSpeed;
	ThrowableActor->Launch(Direction);

	if (CVarDisplayThrowVelocity->GetBool()) {
		const FVector& Start = GetMesh()->GetSocketLocation(TEXT("ObjectAttach"));
		DrawDebugLine(GetWorld(), Start, Start + Direction, FColor::Red, false, 5.0f);
	}

	const FVector& Start = GetMesh()->GetSocketLocation(TEXT("ObjectAttach"));
	UE_VLOG_ARROW(this, LogTantrumnChar, Verbose, Start, Start + Direction, FColor::Red, TEXT("Throw Direction"));
}

void ATantrumnCharacterBase::ServerFinishThrow_Implementation() {
	CharacterThrowState = ECharacterThrowState::None;
	MoveIgnoreActorRemove(ThrowableActor);
	if (ThrowableActor->GetRootComponent()) {
		UPrimitiveComponent* RootPrimitiveComponent = Cast<UPrimitiveComponent>(ThrowableActor->GetRootComponent());
		if (RootPrimitiveComponent) {
			RootPrimitiveComponent->IgnoreActorWhenMoving(this, false);
		}
	}
	ThrowableActor = nullptr;
}

void ATantrumnCharacterBase::ResetThrowableObject() {
	if (ThrowableActor) {
		ThrowableActor->Drop();
	}
	CharacterThrowState = ECharacterThrowState::None;
	ThrowableActor = nullptr;
}

void ATantrumnCharacterBase::RequestAim() {
	if (!bIsStunned && CharacterThrowState == ECharacterThrowState::Attached) {
		CharacterThrowState = ECharacterThrowState::Aiming;
		ServerRequestToggleAim(true);
	}
}

void ATantrumnCharacterBase::RequestStopAim() {
	if (CharacterThrowState == ECharacterThrowState::Aiming) {
		CharacterThrowState = ECharacterThrowState::Attached;
		ServerRequestToggleAim(false);
	}
}

void ATantrumnCharacterBase::ServerRequestToggleAim_Implementation(bool IsAiming) {
	CharacterThrowState = IsAiming ? ECharacterThrowState::Aiming : ECharacterThrowState::Attached;
}

void ATantrumnCharacterBase::RequestUseObject() {
	ApplyEffect_Implementation(ThrowableActor->GetEffectType(), true);
	ThrowableActor->Destroy();
	ResetThrowableObject();
}

void ATantrumnCharacterBase::OnThrowableAttached(AThrowableActor* InThrowableActor) {
	CharacterThrowState = ECharacterThrowState::Attached;
	ThrowableActor = InThrowableActor;
	MoveIgnoreActorAdd(ThrowableActor);
	ClientThrowableAttached(InThrowableActor);
}

void ATantrumnCharacterBase::NotifyHitByThrowable(AThrowableActor* InThrowable) {
	OnStunBegin(1.0f);
}

bool ATantrumnCharacterBase::IsHovering() const {
	if (ATantrumnPlayerState* TantrumnPlayerState = GetPlayerState<ATantrumnPlayerState>()) {
		return TantrumnPlayerState->GetCurrentState() != EPlayerGameState::Playing;
	}
	return false;
}

void ATantrumnCharacterBase::SphereCastPlayerView() {
	FVector Location;
	FRotator Rotation;
	GetController()->GetPlayerViewPoint(Location, Rotation);
	const FVector PlayerViewForward = Rotation.Vector();
	const float AdditionalDistance = (Location - GetActorLocation()).Size();
	FVector EndPos = Location + (PlayerViewForward * (1000.0f + AdditionalDistance));

	const FVector CharacterForward = GetActorForwardVector();
	const float DotResult = FVector::DotProduct(PlayerViewForward, CharacterForward);

	if (DotResult < -0.23f) {
		if (ThrowableActor) {
			ThrowableActor->ToggleHighlight(false);
			ThrowableActor = nullptr;
		}
		return;
		UE_LOG(LogTemp, Warning, TEXT("Dot Result: %f"), DotResult);
	}

	FHitResult HitResult;
	EDrawDebugTrace::Type DebugTrace = CVarDisplayTrace->GetBool() ? EDrawDebugTrace::ForOneFrame : EDrawDebugTrace::None;
	TArray<AActor*> ActorsToIgnore;
	ActorsToIgnore.Add(this);

	UKismetSystemLibrary::SphereTraceSingle(GetWorld(), Location, EndPos, 70.0f, UEngineTypes::ConvertToTraceType(ECollisionChannel::ECC_Visibility), false, ActorsToIgnore, DebugTrace, HitResult, true);
	ProcessTraceResult(HitResult);

#if ENABLE_DRAW_DEBUG
	if (CVarDisplayTrace->GetBool()) {
		static float FovDeg = 90.0f;
		DrawDebugCamera(GetWorld(), Location, Rotation, FovDeg);
		DrawDebugLine(GetWorld(), Location, EndPos, HitResult.bBlockingHit ? FColor::Red : FColor::White);
		DrawDebugPoint(GetWorld(), EndPos, 70.0f, HitResult.bBlockingHit ? FColor::Red : FColor::White);
	}
#endif
}

void ATantrumnCharacterBase::SphereCastActorTransform() {
	FVector StartPos = GetActorLocation();
	FVector EndPos = StartPos + (GetActorForwardVector() * 1000.0f);

	EDrawDebugTrace::Type DebugTrace = CVarDisplayTrace->GetBool() ? EDrawDebugTrace::ForOneFrame : EDrawDebugTrace::None;
	FHitResult HitResult;
	UKismetSystemLibrary::SphereTraceSingle(GetWorld(), StartPos, EndPos, 70.0f, UEngineTypes::ConvertToTraceType(ECollisionChannel::ECC_Visibility), false, TArray<AActor*>(), DebugTrace, HitResult, true);
	ProcessTraceResult(HitResult);
}

void ATantrumnCharacterBase::LineCastActorTransform() {
	FVector StartPos = GetActorLocation();
	FVector EndPos = StartPos + (GetActorForwardVector() * 1000.0f);
	FHitResult HitResult;
	GetWorld() ? GetWorld()->LineTraceSingleByChannel(HitResult, StartPos, EndPos, ECollisionChannel::ECC_Visibility) : false;
#if ENABLE_DRAW_DEBUG
	if (CVarDisplayTrace->GetBool()) {
		DrawDebugLine(GetWorld(), StartPos, EndPos, HitResult.bBlockingHit ? FColor::Red : FColor::White, false);
	}
#endif
	ProcessTraceResult(HitResult);
}

void ATantrumnCharacterBase::ProcessTraceResult(const FHitResult& HitResult, bool bHighlight /* = true */) {
	AThrowableActor* HitThrowableActor = HitResult.bBlockingHit ? Cast<AThrowableActor>(HitResult.GetActor()) : nullptr;
	const bool IsSameActor = (ThrowableActor == HitThrowableActor);
	const bool IsValidTarget = HitThrowableActor && HitThrowableActor->IsIdle();

	if (ThrowableActor && (!IsValidTarget || !IsSameActor)) {
		ThrowableActor->ToggleHighlight(false);
		ThrowableActor = nullptr;
	}

	if (!IsValidTarget) {
		return;
	}

	if (!IsSameActor) {
		ThrowableActor = HitThrowableActor;
		if (bHighlight) {
			ThrowableActor->ToggleHighlight(true);
		}
	}

	if (CharacterThrowState == ECharacterThrowState::RequestingPull) {
		if (GetVelocity().SizeSquared() < 100.0f) {
			ServerPullObject(ThrowableActor);
			CharacterThrowState = ECharacterThrowState::Pulling;
			ThrowableActor->ToggleHighlight(false);
		}
	}
}

bool ATantrumnCharacterBase::PlayThrowMontage() {
	const float PlayRate = 1.0f;
	const FName StartSectionName = IsAiming() ? TEXT("AimStart") : TEXT("Default");
	bool bPlayedSuccessfully = PlayAnimMontage(ThrowMontage, PlayRate, StartSectionName) > 0.0f;
	if (bPlayedSuccessfully) {
		if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance()) {
			if (!BlendingOutDelegate.IsBound()) {
				BlendingOutDelegate.BindUObject(this, &ATantrumnCharacterBase::OnMontageBlendingOut);
			}
			AnimInstance->Montage_SetBlendingOutDelegate(BlendingOutDelegate, ThrowMontage);

			if (!MontageEndedDelegate.IsBound()) {
				MontageEndedDelegate.BindUObject(this, &ATantrumnCharacterBase::OnMontageEnded);
			}
			AnimInstance->Montage_SetEndDelegate(MontageEndedDelegate, ThrowMontage);

			if (IsLocallyControlled()) {
				AnimInstance->OnPlayMontageNotifyBegin.AddDynamic(this, &ATantrumnCharacterBase::OnNotifyBeginReceived);
				AnimInstance->OnPlayMontageNotifyEnd.AddDynamic(this, &ATantrumnCharacterBase::OnNotifyEndReceived);
			}
		}
	}
	return bPlayedSuccessfully;
}

bool ATantrumnCharacterBase::PlayCelebrateMontage() {
	const float PlayRate = 1.0f;
	bool bPlayedSuccessfully = PlayAnimMontage(CelebrateMontage, PlayRate) > 0.f;
	if (bPlayedSuccessfully) {
		UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
		if (!BlendingOutDelegate.IsBound()) {
			BlendingOutDelegate.BindUObject(this, &ATantrumnCharacterBase::OnMontageBlendingOut);
		}
		AnimInstance->Montage_SetBlendingOutDelegate(BlendingOutDelegate, CelebrateMontage);

		if (!MontageEndedDelegate.IsBound()) {
			MontageEndedDelegate.BindUObject(this, &ATantrumnCharacterBase::OnMontageEnded);
		}
		AnimInstance->Montage_SetEndDelegate(MontageEndedDelegate, CelebrateMontage);
	}
	return bPlayedSuccessfully;
}

void ATantrumnCharacterBase::ServerPlayCelebrateMontage_Implementation() {
	MulticastPlayCelebrateMontage();
}

void ATantrumnCharacterBase::MulticastPlayCelebrateMontage_Implementation() {
	PlayCelebrateMontage();
}

void ATantrumnCharacterBase::UpdateThrowMontagePlayRate() {
	if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance()) {
		if (UAnimMontage* CurrentAnimMontage = AnimInstance->GetCurrentActiveMontage()) {
			const float PlayRate = AnimInstance->GetCurveValue(TEXT("ThrowCurve"));
			AnimInstance->Montage_SetPlayRate(CurrentAnimMontage, PlayRate);
		}
	}
}

void ATantrumnCharacterBase::UnbindMontage() {
	if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance()) {
		AnimInstance->OnPlayMontageNotifyBegin.RemoveDynamic(this, &ATantrumnCharacterBase::OnNotifyBeginReceived);
		AnimInstance->OnPlayMontageNotifyEnd.RemoveDynamic(this, &ATantrumnCharacterBase::OnNotifyEndReceived);
	}
}

void ATantrumnCharacterBase::OnMontageBlendingOut(UAnimMontage* Montage, bool bInterrupted) {

}

void ATantrumnCharacterBase::OnMontageEnded(UAnimMontage* Montage, bool bInterrupted) {
	if (IsLocallyControlled()) {
		UnbindMontage();
	}
	
	if (Montage == ThrowMontage) {
		if (IsLocallyControlled()) {
			CharacterThrowState = ECharacterThrowState::None;
			ServerFinishThrow();
			ThrowableActor = nullptr;
		}
	}
	else if (Montage == CelebrateMontage) {
		//if (IsLocallyControlled()) {
		//	//display hud
		//	if (UTantrumnGameInstance* TantrumnGameInstance = GetWorld()->GetGameInstance<UTantrumnGameInstance>()) {
		//		ATantrumnPlayerController* TantrumnPlayerController = GetController<ATantrumnPlayerController>();
		//		if (TantrumnPlayerController) {
		//			TantrumnGameInstance->DisplayLevelComplete(TantrumnPlayerController);
		//		}
		//	}
		//}

		if (ATantrumnPlayerState* TantrumnPlayerState = GetPlayerState<ATantrumnPlayerState>()) {
			if (TantrumnPlayerState->IsWinner()) {
				float length = PlayAnimMontage(CelebrateMontage, 1.0f, TEXT("Winner"));
				ensureAlwaysMsgf(length > 0.f, TEXT("ATantrumnCharacterBase::OnMontageEnded Could Not Play Winner Animation"));
			}
		}
	}
}

void ATantrumnCharacterBase::OnNotifyBeginReceived(FName NotifyName, const FBranchingPointNotifyPayload& BranchingPointNotifyPayload) {
	ServerBeginThrow();
}

void ATantrumnCharacterBase::OnNotifyEndReceived(FName NotifyName, const FBranchingPointNotifyPayload& BranchingPointNotifyPayload) {

}

void ATantrumnCharacterBase::OnRep_CharacterThrowState(const ECharacterThrowState& OldCharacterThrowState) {
	if (CharacterThrowState != OldCharacterThrowState) {
		UE_LOG(LogTemp, Warning, TEXT("OldThrowState: %s"), *UEnum::GetDisplayValueAsText(OldCharacterThrowState).ToString());
		UE_LOG(LogTemp, Warning, TEXT("CharacterThrowState: %s"), *UEnum::GetDisplayValueAsText(CharacterThrowState).ToString());
	}
}

void ATantrumnCharacterBase::ApplyEffect_Implementation(EEffectType EffectType, bool bIsBuff) {
	if (bIsUnderEffect) return;

	CurrentEffect = EffectType;
	bIsUnderEffect = true;
	bIsEffectBuff = bIsBuff;

	switch (CurrentEffect) {
	case EEffectType::Speed :
		bIsEffectBuff ? SprintSpeed *= 2 : GetCharacterMovement()->DisableMovement();
		break;
	default:
		break;
	}
}

void ATantrumnCharacterBase::UpdateEffect(float DeltaTime) {
	if (EffectCooldown > 0) {
		EffectCooldown -= DeltaTime;
	}
	else {
		bIsUnderEffect = false;
		EffectCooldown = DefaultEffectCooldown;
		EndEffect();
	}
}

void ATantrumnCharacterBase::EndEffect() {
	bIsUnderEffect = false;
	switch (CurrentEffect) {
	case EEffectType::Speed :
		bIsEffectBuff ? SprintSpeed /= 2, RequestSprintEnd() : GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		break;
	default:
		break;
	}
}