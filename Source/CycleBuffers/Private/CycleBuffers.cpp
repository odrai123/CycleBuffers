#include "CycleBuffers.h"

#include "Buildables/FGBuildableGeneratorFuel.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "EngineUtils.h"
#include "FGInventoryComponent.h"
#include "FGRecipe.h"
#include "ItemAmount.h"
#include "Patching/NativeHookManager.h"
#include "Resources/FGItemDescriptor.h"
#include "SessionSettings/SessionSettingsManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogCycleBuffers, Log, All);
#define LOCTEXT_NAMESPACE "FCycleBuffersModule"

namespace
{
	static const FString InputSettingId = TEXT("CycleBuffers.InputBufferCycles");
	static const FString OutputSettingId = TEXT("CycleBuffers.OutputBufferCycles");
	static const FString EnableGeneratorBuffersSettingId = TEXT("CycleBuffers.EnableGeneratorBuffers");
	static const FString GeneratorBufferCyclesSettingId = TEXT("CycleBuffers.GeneratorBufferCycles");

	struct FCachedItemLimit
	{
		TSubclassOf<UFGItemDescriptor> ItemClass;
		int32 BufferLimit = 0;
	};

	struct FCachedRecipeLimits
	{
		TArray<FCachedItemLimit> Inputs;
		TArray<FCachedItemLimit> Outputs;
	};

	class FManufacturerCacheAccessor : public AFGBuildableManufacturer
	{
	public:
		static auto GetCachedIngredientDataMember()
		{
			return &FManufacturerCacheAccessor::mCachedIngredientData;
		}
	};

	using FFactoryCollectFuelSignature = void (AFGBuildableGeneratorFuel::*)(float);
	using FFactoryCollectSupplementalSignature = void (AFGBuildableGeneratorFuel::*)(float);

	class FGeneratorFuelAccessor : public AFGBuildableGeneratorFuel
	{
	public:
		inline static constexpr FFactoryCollectFuelSignature FactoryCollectFuelPtr =
			static_cast<FFactoryCollectFuelSignature>(&AFGBuildableGeneratorFuel::Factory_CollectFuel);

		inline static constexpr FFactoryCollectSupplementalSignature FactoryCollectSupplementalPtr =
			static_cast<FFactoryCollectSupplementalSignature>(
				&AFGBuildableGeneratorFuel::Factory_CollectSupplementalResource);

		static auto GetFuelLoadAmountMember()
		{
			return &FGeneratorFuelAccessor::mFuelLoadAmount;
		}

		static auto GetUsesFluidsAsFuelMember()
		{
			return &FGeneratorFuelAccessor::mUsesFluidsAsFuel;
		}

		static auto GetSupplementalToPowerRatioMember()
		{
			return &FGeneratorFuelAccessor::mSupplementalToPowerRatio;
		}
	};

	TWeakObjectPtr<UWorld> CachedWorld;
	TMap<TSubclassOf<UFGRecipe>, FCachedRecipeLimits> RecipeLimitCache;
	TMap<TWeakObjectPtr<AFGBuildableGeneratorFuel>, int32> NativeWaterSlotSizeCache;

	int32 CachedInputBufferCycles = INDEX_NONE;
	int32 CachedOutputBufferCycles = INDEX_NONE;
	bool CachedGeneratorBuffersEnabled = true;
	int32 CachedGeneratorBufferCycles = 3;

	TWeakObjectPtr<USessionSettingsManager> SubscribedSessionSettingsManager;
	FOnOptionUpdated SessionSettingsUpdatedDelegate;

	void ApplyCycleBuffers(AFGBuildableManufacturer* Manufacturer);
	void ApplyGeneratorBuffers(AFGBuildableGeneratorFuel* Generator);

	int32 CalculateCycleCap(const int32 AmountPerCycle, const int32 NumberOfCycles)
	{
		return FMath::Max(1, AmountPerCycle * NumberOfCycles);
	}

	int32 ClampToEffectiveStackSize(
		const TSubclassOf<UFGItemDescriptor> ItemClass,
		const int32 RequestedLimit)
	{
		if (!ItemClass)
		{
			return FMath::Max(1, RequestedLimit);
		}

		const int32 EffectiveStackSize = UFGItemDescriptor::GetStackSize(ItemClass);
		if (EffectiveStackSize <= 0)
		{
			return FMath::Max(1, RequestedLimit);
		}

		return FMath::Max(1, FMath::Min(RequestedLimit, EffectiveStackSize));
	}

	int32 GetGeneratorFuelLoadAmount(AFGBuildableGeneratorFuel* Generator)
	{
		if (!IsValid(Generator))
		{
			return 1;
		}

		auto Member = FGeneratorFuelAccessor::GetFuelLoadAmountMember();
		return FMath::Max(1, static_cast<int32>(Generator->*Member));
	}

	bool GeneratorUsesFluidFuel(AFGBuildableGeneratorFuel* Generator)
	{
		if (!IsValid(Generator))
		{
			return false;
		}

		auto Member = FGeneratorFuelAccessor::GetUsesFluidsAsFuelMember();
		return Generator->*Member;
	}

	float GetGeneratorSupplementalToPowerRatio(AFGBuildableGeneratorFuel* Generator)
	{
		if (!IsValid(Generator))
		{
			return 0.0f;
		}

		auto Member = FGeneratorFuelAccessor::GetSupplementalToPowerRatioMember();
		return Generator->*Member;
	}

	int32 CalculateGeneratorFuelLimit(AFGBuildableGeneratorFuel* Generator)
	{
		return CalculateCycleCap(GetGeneratorFuelLoadAmount(Generator), CachedGeneratorBufferCycles);
	}

	/*
	 * Supplemental resources in the vanilla generators are water.
	 * mSupplementalToPowerRatio is the raw supplemental quantity consumed per
	 * MW-second of generated energy. Fuel energy is also expressed in MW-seconds
	 * (MJ), so their product is the raw water amount consumed by one fuel item.
	 *
	 * Coal sanity check: 300 MJ * 10 L/MW-s = 3000 L = 3 m3 per coal.
	 * Nuclear sanity check: one rod resolves to 1,200,000 L = 1200 m3.
	 */
	int32 CalculateGeneratorWaterLimit(AFGBuildableGeneratorFuel* Generator)
	{
		if (!IsValid(Generator))
		{
			return INDEX_NONE;
		}

		const TSubclassOf<UFGItemDescriptor> FuelClass = Generator->GetCurrentFuelClass();
		if (!FuelClass)
		{
			return INDEX_NONE;
		}

		const float FuelEnergyMJ = UFGItemDescriptor::GetEnergyValue(FuelClass);
		const float SupplementalToPowerRatio = GetGeneratorSupplementalToPowerRatio(Generator);
		if (FuelEnergyMJ <= 0.0f || SupplementalToPowerRatio <= 0.0f)
		{
			return INDEX_NONE;
		}

		const float RawWaterPerFuel = FuelEnergyMJ * SupplementalToPowerRatio;
		const float RawWaterLimit = RawWaterPerFuel * static_cast<float>(CachedGeneratorBufferCycles);
		return FMath::Max(1, FMath::CeilToInt(RawWaterLimit));
	}

	void ResetRecipeCache()
	{
		RecipeLimitCache.Empty();
		CachedInputBufferCycles = INDEX_NONE;
		CachedOutputBufferCycles = INDEX_NONE;
	}

	void UnsubscribeFromSessionSettings()
	{
		if (SessionSettingsUpdatedDelegate.IsBound())
		{
			if (USessionSettingsManager* Manager = SubscribedSessionSettingsManager.Get())
			{
				Manager->UnsubscribeToAllOptionUpdates(SessionSettingsUpdatedDelegate);
			}
		}

		SubscribedSessionSettingsManager.Reset();
		SessionSettingsUpdatedDelegate.Unbind();
	}

	void ReapplyAllManufacturers(UWorld* World)
	{
		if (!IsValid(World))
		{
			return;
		}

		for (TActorIterator<AFGBuildableManufacturer> It(World); It; ++It)
		{
			if (AFGBuildableManufacturer* Manufacturer = *It; IsValid(Manufacturer))
			{
				ApplyCycleBuffers(Manufacturer);
			}
		}
	}

	void ReapplyAllGenerators(UWorld* World)
	{
		if (!IsValid(World))
		{
			return;
		}

		for (TActorIterator<AFGBuildableGeneratorFuel> It(World); It; ++It)
		{
			if (AFGBuildableGeneratorFuel* Generator = *It; IsValid(Generator))
			{
				ApplyGeneratorBuffers(Generator);
			}
		}
	}

	void GetGeneratorBufferSettings(UObject* WorldContext, bool& OutEnabled, int32& OutGeneratorCycles)
	{
		OutEnabled = true;
		OutGeneratorCycles = 3;

		if (!IsValid(WorldContext))
		{
			return;
		}

		UWorld* World = WorldContext->GetWorld();
		if (!IsValid(World))
		{
			return;
		}

		USessionSettingsManager* SessionSettings = World->GetSubsystem<USessionSettingsManager>();
		if (!IsValid(SessionSettings))
		{
			return;
		}

		OutEnabled = SessionSettings->GetBoolOptionValue(EnableGeneratorBuffersSettingId);
		OutGeneratorCycles = SessionSettings->GetIntOptionValue(GeneratorBufferCyclesSettingId);
		if (OutGeneratorCycles < 1)
		{
			OutGeneratorCycles = 3;
		}
	}

	void RefreshGeneratorSettings(UObject* WorldContext)
	{
		bool Enabled = true;
		int32 GeneratorCycles = 3;
		GetGeneratorBufferSettings(WorldContext, Enabled, GeneratorCycles);
		CachedGeneratorBuffersEnabled = Enabled;
		CachedGeneratorBufferCycles = GeneratorCycles;
	}

	void HandleSessionSettingUpdated(const FString SettingId, const FVariant Value)
	{
		const bool ManufacturerSettingChanged =
			SettingId == InputSettingId || SettingId == OutputSettingId;
		const bool GeneratorSettingChanged =
			SettingId == EnableGeneratorBuffersSettingId || SettingId == GeneratorBufferCyclesSettingId;

		if (!ManufacturerSettingChanged && !GeneratorSettingChanged)
		{
			return;
		}

		UWorld* World = CachedWorld.Get();
		if (!IsValid(World))
		{
			return;
		}

		if (ManufacturerSettingChanged)
		{
			ResetRecipeCache();
			UE_LOG(LogCycleBuffers, Log, TEXT("Manufacturer buffer settings changed - reapplying limits."));
			ReapplyAllManufacturers(World);
		}

		if (GeneratorSettingChanged)
		{
			RefreshGeneratorSettings(World);
			UE_LOG(LogCycleBuffers, Log, TEXT("Generator buffer settings changed - reapplying limits."));
			ReapplyAllGenerators(World);
		}
	}

	void SubscribeToSessionSettings(UWorld* World)
	{
		if (!IsValid(World))
		{
			return;
		}

		USessionSettingsManager* Manager = World->GetSubsystem<USessionSettingsManager>();
		if (!IsValid(Manager))
		{
			return;
		}

		if (SubscribedSessionSettingsManager.Get() == Manager && SessionSettingsUpdatedDelegate.IsBound())
		{
			return;
		}

		UnsubscribeFromSessionSettings();
		SessionSettingsUpdatedDelegate = FOnOptionUpdated::CreateStatic(&HandleSessionSettingUpdated);
		Manager->SubscribeToAllOptionUpdates(SessionSettingsUpdatedDelegate);
		SubscribedSessionSettingsManager = Manager;
	}

	void EnsureCorrectWorld(UObject* WorldContext)
	{
		if (!IsValid(WorldContext))
		{
			return;
		}

		UWorld* CurrentWorld = WorldContext->GetWorld();
		if (!IsValid(CurrentWorld))
		{
			return;
		}

		if (CachedWorld.Get() != CurrentWorld)
		{
			UnsubscribeFromSessionSettings();
			NativeWaterSlotSizeCache.Empty();
			CachedWorld = CurrentWorld;
			ResetRecipeCache();
			SubscribeToSessionSettings(CurrentWorld);
			RefreshGeneratorSettings(CurrentWorld);
		}
		else if (!SubscribedSessionSettingsManager.IsValid())
		{
			SubscribeToSessionSettings(CurrentWorld);
		}
	}

	void GetCycleBufferSettings(UObject* WorldContext, int32& OutInputCycles, int32& OutOutputCycles)
	{
		OutInputCycles = 3;
		OutOutputCycles = 10;

		if (!IsValid(WorldContext))
		{
			return;
		}

		UWorld* World = WorldContext->GetWorld();
		if (!IsValid(World))
		{
			return;
		}

		USessionSettingsManager* SessionSettings = World->GetSubsystem<USessionSettingsManager>();
		if (!IsValid(SessionSettings))
		{
			return;
		}

		OutInputCycles = SessionSettings->GetIntOptionValue(InputSettingId);
		OutOutputCycles = SessionSettings->GetIntOptionValue(OutputSettingId);

		if (OutInputCycles < 1)
		{
			OutInputCycles = 3;
		}
		if (OutOutputCycles < 1)
		{
			OutOutputCycles = 10;
		}
	}

	void CheckForConfigurationChange(const int32 InputCycles, const int32 OutputCycles)
	{
		if (InputCycles == CachedInputBufferCycles && OutputCycles == CachedOutputBufferCycles)
		{
			return;
		}

		RecipeLimitCache.Empty();
		CachedInputBufferCycles = InputCycles;
		CachedOutputBufferCycles = OutputCycles;
	}

	const FCachedRecipeLimits* GetOrCreateRecipeLimits(AFGBuildableManufacturer* Manufacturer)
	{
		if (!IsValid(Manufacturer))
		{
			return nullptr;
		}

		EnsureCorrectWorld(Manufacturer);

		const TSubclassOf<UFGRecipe> Recipe = Manufacturer->GetCurrentRecipe();
		if (!Recipe)
		{
			return nullptr;
		}

		int32 InputCycles = 3;
		int32 OutputCycles = 10;
		GetCycleBufferSettings(Manufacturer, InputCycles, OutputCycles);
		CheckForConfigurationChange(InputCycles, OutputCycles);

		if (const FCachedRecipeLimits* Existing = RecipeLimitCache.Find(Recipe))
		{
			return Existing;
		}

		FCachedRecipeLimits NewLimits;
		const TArray<FItemAmount> Ingredients = UFGRecipe::GetIngredients(Manufacturer, Recipe);
		const TArray<FItemAmount> Products = UFGRecipe::GetProducts(Recipe);
		NewLimits.Inputs.Reserve(Ingredients.Num());
		NewLimits.Outputs.Reserve(Products.Num());

		for (const FItemAmount& Ingredient : Ingredients)
		{
			if (!Ingredient.ItemClass)
			{
				continue;
			}

			FCachedItemLimit& Limit = NewLimits.Inputs.AddDefaulted_GetRef();
			Limit.ItemClass = Ingredient.ItemClass;
			Limit.BufferLimit = ClampToEffectiveStackSize(
				Ingredient.ItemClass,
				CalculateCycleCap(Ingredient.Amount, InputCycles));
		}

		for (const FItemAmount& Product : Products)
		{
			if (!Product.ItemClass)
			{
				continue;
			}

			FCachedItemLimit& Limit = NewLimits.Outputs.AddDefaulted_GetRef();
			Limit.ItemClass = Product.ItemClass;
			Limit.BufferLimit = ClampToEffectiveStackSize(
				Product.ItemClass,
				CalculateCycleCap(Product.Amount, OutputCycles));
		}

		return &RecipeLimitCache.Add(Recipe, MoveTemp(NewLimits));
	}

	const FCachedItemLimit* FindCachedLimit(
		const TArray<FCachedItemLimit>& Limits,
		TSubclassOf<UFGItemDescriptor> ItemClass)
	{
		return Limits.FindByPredicate(
			[ItemClass](const FCachedItemLimit& Entry)
			{
				return Entry.ItemClass == ItemClass;
			});
	}

	void ApplyCycleBuffers(AFGBuildableManufacturer* Manufacturer)
	{
		if (!IsValid(Manufacturer) || !Manufacturer->GetCurrentRecipe())
		{
			return;
		}

		UFGInventoryComponent* InputInventory = Manufacturer->GetInputInventory();
		UFGInventoryComponent* OutputInventory = Manufacturer->GetOutputInventory();
		if (!IsValid(InputInventory) || !IsValid(OutputInventory))
		{
			return;
		}

		const FCachedRecipeLimits* Limits = GetOrCreateRecipeLimits(Manufacturer);
		if (Limits == nullptr)
		{
			return;
		}

		for (int32 SlotIndex = 0; SlotIndex < InputInventory->GetSizeLinear(); ++SlotIndex)
		{
			const TSubclassOf<UFGItemDescriptor> AllowedItem =
				InputInventory->GetAllowedItemOnIndex(SlotIndex);
			if (!AllowedItem)
			{
				continue;
			}

			if (const FCachedItemLimit* Limit = FindCachedLimit(Limits->Inputs, AllowedItem))
			{
				InputInventory->AddArbitrarySlotSize(SlotIndex, Limit->BufferLimit);
			}
		}

		for (int32 SlotIndex = 0; SlotIndex < OutputInventory->GetSizeLinear(); ++SlotIndex)
		{
			const TSubclassOf<UFGItemDescriptor> AllowedItem =
				OutputInventory->GetAllowedItemOnIndex(SlotIndex);
			if (!AllowedItem)
			{
				continue;
			}

			if (const FCachedItemLimit* Limit = FindCachedLimit(Limits->Outputs, AllowedItem))
			{
				OutputInventory->AddArbitrarySlotSize(SlotIndex, Limit->BufferLimit);
			}
		}

		auto CachedIngredientDataMember = FManufacturerCacheAccessor::GetCachedIngredientDataMember();
		TArray<FSimpleIngredientData>& CachedIngredientData = Manufacturer->*CachedIngredientDataMember;
		const int32 EntriesToUpdate = FMath::Min(CachedIngredientData.Num(), Limits->Inputs.Num());

		for (int32 Index = 0; Index < EntriesToUpdate; ++Index)
		{
			CachedIngredientData[Index].StackSize = Limits->Inputs[Index].BufferLimit;
		}

		if (CachedIngredientData.Num() != Limits->Inputs.Num())
		{
			UE_LOG(
				LogCycleBuffers,
				Warning,
				TEXT("Ingredient cache count mismatch for %s: native=%d cached=%d"),
				*Manufacturer->GetName(),
				CachedIngredientData.Num(),
				Limits->Inputs.Num());
		}
	}

	int32 GetOrCacheNativeWaterSlotSize(AFGBuildableGeneratorFuel* Generator)
	{
		if (!IsValid(Generator))
		{
			return INDEX_NONE;
		}

		const TWeakObjectPtr<AFGBuildableGeneratorFuel> Key(Generator);
		if (const int32* CachedSize = NativeWaterSlotSizeCache.Find(Key))
		{
			return *CachedSize;
		}

		UFGInventoryComponent* FuelInventory = Generator->GetFuelInventory();
		if (!IsValid(FuelInventory))
		{
			return INDEX_NONE;
		}

		const int32 WaterIndex = Generator->GetSupplementalResourceInventoryIndex();
		if (WaterIndex < 0 || WaterIndex >= FuelInventory->GetSizeLinear())
		{
			return INDEX_NONE;
		}

		const int32 NativeSlotSize = FuelInventory->GetSlotSize(WaterIndex);
		if (NativeSlotSize <= 0)
		{
			return INDEX_NONE;
		}

		NativeWaterSlotSizeCache.Add(Key, NativeSlotSize);
		return NativeSlotSize;
	}

	void RemoveGeneratorFuelOverride(AFGBuildableGeneratorFuel* Generator)
	{
		if (!IsValid(Generator))
		{
			return;
		}

		UFGInventoryComponent* FuelInventory = Generator->GetFuelInventory();
		if (!IsValid(FuelInventory))
		{
			return;
		}

		const int32 FuelIndex = Generator->GetFuelInventoryIndex();
		if (FuelIndex >= 0 && FuelIndex < FuelInventory->GetSizeLinear())
		{
			FuelInventory->RemoveArbitrarySlotSize(FuelIndex);
		}
	}

	void RemoveGeneratorWaterOverride(AFGBuildableGeneratorFuel* Generator)
	{
		if (!IsValid(Generator))
		{
			return;
		}

		UFGInventoryComponent* FuelInventory = Generator->GetFuelInventory();
		if (!IsValid(FuelInventory))
		{
			return;
		}

		const int32 WaterIndex = Generator->GetSupplementalResourceInventoryIndex();
		if (WaterIndex >= 0 && WaterIndex < FuelInventory->GetSizeLinear())
		{
			FuelInventory->RemoveArbitrarySlotSize(WaterIndex);
		}
	}

	bool ApplyGeneratorWaterLimitIfReady(AFGBuildableGeneratorFuel* Generator, int32& OutWaterLimit)
	{
		const int32 CalculatedWaterLimit = CalculateGeneratorWaterLimit(Generator);
		if (CalculatedWaterLimit <= 0)
		{
			OutWaterLimit = INDEX_NONE;
			return false;
		}

		UFGInventoryComponent* FuelInventory = Generator->GetFuelInventory();
		if (!IsValid(FuelInventory))
		{
			OutWaterLimit = INDEX_NONE;
			return false;
		}

		const int32 WaterIndex = Generator->GetSupplementalResourceInventoryIndex();
		if (WaterIndex < 0 || WaterIndex >= FuelInventory->GetSizeLinear())
		{
			OutWaterLimit = INDEX_NONE;
			return false;
		}

		const int32 NativeWaterSlotSize = GetOrCacheNativeWaterSlotSize(Generator);
		if (NativeWaterSlotSize <= 0)
		{
			OutWaterLimit = INDEX_NONE;
			return false;
		}

		OutWaterLimit = FMath::Min(CalculatedWaterLimit, NativeWaterSlotSize);

		/*
		 * Supplemental water uses GetSlotSize() before pulling from the pipe.
		 * Clamp the cycle-based target to this generator's original native water
		 * capacity so CycleBuffers can shrink the buffer, but never enlarge it.
		 */
		if (FuelInventory->GetSlotSize(WaterIndex) != OutWaterLimit)
		{
			FuelInventory->RemoveArbitrarySlotSize(WaterIndex);
			FuelInventory->AddArbitrarySlotSize(WaterIndex, OutWaterLimit);
		}

		return true;
	}

	void ApplyGeneratorBuffers(AFGBuildableGeneratorFuel* Generator)
	{
		if (!IsValid(Generator))
		{
			return;
		}

		EnsureCorrectWorld(Generator);
		RefreshGeneratorSettings(Generator);

		UFGInventoryComponent* FuelInventory = Generator->GetFuelInventory();
		if (!IsValid(FuelInventory))
		{
			return;
		}

		const int32 FuelIndex = Generator->GetFuelInventoryIndex();
		const int32 WaterIndex = Generator->GetSupplementalResourceInventoryIndex();

		if (FuelIndex >= 0 && FuelIndex < FuelInventory->GetSizeLinear())
		{
			FuelInventory->RemoveArbitrarySlotSize(FuelIndex);
		}
		if (WaterIndex >= 0 && WaterIndex < FuelInventory->GetSizeLinear())
		{
			FuelInventory->RemoveArbitrarySlotSize(WaterIndex);

			/* Capture the unmodified/native water capacity before applying our cap. */
			const TWeakObjectPtr<AFGBuildableGeneratorFuel> Key(Generator);
			if (!NativeWaterSlotSizeCache.Contains(Key))
			{
				const int32 NativeSlotSize = FuelInventory->GetSlotSize(WaterIndex);
				if (NativeSlotSize > 0)
				{
					NativeWaterSlotSizeCache.Add(Key, NativeSlotSize);
				}
			}
		}

		if (!CachedGeneratorBuffersEnabled)
		{
			return;
		}

		/* Fluid/gas fuels natively honour GetSlotSize(). Solid fuels use the pre-hook. */
		if (
			FuelIndex >= 0 &&
			FuelIndex < FuelInventory->GetSizeLinear() &&
			GeneratorUsesFluidFuel(Generator))
		{
			FuelInventory->AddArbitrarySlotSize(FuelIndex, CalculateGeneratorFuelLimit(Generator));
		}

		/* Fuel may not yet be known at BeginPlay; the supplemental hook retries lazily. */
		int32 IgnoredWaterLimit = INDEX_NONE;
		ApplyGeneratorWaterLimitIfReady(Generator, IgnoredWaterLimit);
	}

	bool ShouldBlockGeneratorFuelCollection(AFGBuildableGeneratorFuel* Generator)
	{
		if (!IsValid(Generator) || !CachedGeneratorBuffersEnabled)
		{
			return false;
		}

		UFGInventoryComponent* FuelInventory = Generator->GetFuelInventory();
		if (!IsValid(FuelInventory))
		{
			return false;
		}

		const int32 FuelIndex = Generator->GetFuelInventoryIndex();
		if (FuelIndex < 0 || FuelIndex >= FuelInventory->GetSizeLinear())
		{
			return false;
		}

		FInventoryStack FuelStack;
		if (!FuelInventory->GetStackFromIndex(FuelIndex, FuelStack))
		{
			return false;
		}

		return FuelStack.NumItems >= CalculateGeneratorFuelLimit(Generator);
	}

	bool ShouldBlockGeneratorSupplementalCollection(AFGBuildableGeneratorFuel* Generator)
	{
		if (!IsValid(Generator) || !CachedGeneratorBuffersEnabled)
		{
			return false;
		}

		UFGInventoryComponent* FuelInventory = Generator->GetFuelInventory();
		if (!IsValid(FuelInventory))
		{
			return false;
		}

		const int32 WaterIndex = Generator->GetSupplementalResourceInventoryIndex();
		if (WaterIndex < 0 || WaterIndex >= FuelInventory->GetSizeLinear())
		{
			return false;
		}

		int32 WaterLimit = INDEX_NONE;
		if (!ApplyGeneratorWaterLimitIfReady(Generator, WaterLimit))
		{
			/* No current fuel yet: leave vanilla collection untouched until the ratio is calculable. */
			return false;
		}

		FInventoryStack WaterStack;
		if (!FuelInventory->GetStackFromIndex(WaterIndex, WaterStack))
		{
			return false;
		}

		return WaterStack.NumItems >= WaterLimit;
	}
}

void FCycleBuffersModule::StartupModule()
{
	UE_LOG(LogCycleBuffers, Log, TEXT("CycleBuffers loaded."));

	if (!WITH_EDITOR)
	{
		SUBSCRIBE_METHOD_VIRTUAL_AFTER(
			AFGBuildableManufacturer::BeginPlay,
			GetDefault<AFGBuildableManufacturer>(),
			[](AFGBuildableManufacturer* Manufacturer)
			{
				ApplyCycleBuffers(Manufacturer);
			});

		SUBSCRIBE_METHOD_AFTER(
			AFGBuildableManufacturer::SetRecipe,
			[](AFGBuildableManufacturer* Manufacturer, TSubclassOf<UFGRecipe> Recipe)
			{
				ApplyCycleBuffers(Manufacturer);
			});

		SUBSCRIBE_METHOD_VIRTUAL_AFTER(
			AFGBuildableGeneratorFuel::BeginPlay,
			GetDefault<AFGBuildableGeneratorFuel>(),
			[](AFGBuildableGeneratorFuel* Generator)
			{
				ApplyGeneratorBuffers(Generator);
			});

		using FFactoryCollectFuelHook = HookInvoker<
			FFactoryCollectFuelSignature,
			FGeneratorFuelAccessor::FactoryCollectFuelPtr>;

		Invoke(
			[&]()
			{
				FFactoryCollectFuelHook::InstallHook(TEXT("AFGBuildableGeneratorFuel::Factory_CollectFuel"));
				return FFactoryCollectFuelHook::AddHandlerBefore(
					FFactoryCollectFuelHook::Handler(
						[](auto& Scope, AFGBuildableGeneratorFuel* Generator, float dt)
						{
							if (ShouldBlockGeneratorFuelCollection(Generator))
							{
								Scope.Cancel();
							}
						}));
			});

		using FFactoryCollectSupplementalHook = HookInvoker<
			FFactoryCollectSupplementalSignature,
			FGeneratorFuelAccessor::FactoryCollectSupplementalPtr>;

		Invoke(
			[&]()
			{
				FFactoryCollectSupplementalHook::InstallHook(
					TEXT("AFGBuildableGeneratorFuel::Factory_CollectSupplementalResource"));
				return FFactoryCollectSupplementalHook::AddHandlerBefore(
					FFactoryCollectSupplementalHook::Handler(
						[](auto& Scope, AFGBuildableGeneratorFuel* Generator, float dt)
						{
							if (ShouldBlockGeneratorSupplementalCollection(Generator))
							{
								Scope.Cancel();
							}
						}));
			});
	}
}

void FCycleBuffersModule::ShutdownModule()
{
	UWorld* World = CachedWorld.Get();
	if (IsValid(World))
	{
		for (TActorIterator<AFGBuildableGeneratorFuel> It(World); It; ++It)
		{
			AFGBuildableGeneratorFuel* Generator = *It;
			if (!IsValid(Generator))
			{
				continue;
			}

			RemoveGeneratorFuelOverride(Generator);
			RemoveGeneratorWaterOverride(Generator);
		}
	}

	UnsubscribeFromSessionSettings();
	RecipeLimitCache.Empty();
	NativeWaterSlotSizeCache.Empty();
	CachedWorld.Reset();

	UE_LOG(LogCycleBuffers, Log, TEXT("CycleBuffers unloaded."));
}

#undef LOCTEXT_NAMESPACE
IMPLEMENT_MODULE(FCycleBuffersModule, CycleBuffers)
