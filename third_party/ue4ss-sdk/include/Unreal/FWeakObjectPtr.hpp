#pragma once

#include <cstdint>
#include <bit>
#include <Unreal/Core/CoreTypes.hpp>
#include <Unreal/Common.hpp>
#include <Unreal/Core/Misc/CoreMiscDefines.hpp>

#include "Misc/AssertionMacros.hpp"
#include "VersionedContainer/Container.hpp"

namespace RC::Unreal
{

#ifndef UE_WEAKOBJECTPTR_ZEROINIT_FIX
    #define UE_WEAKOBJECTPTR_ZEROINIT_FIX 1
#endif
    
    namespace UE::Core::Private
    {
        /** Specifies the ObjectIndex used for invalid object pointers. */
#if UE_WEAKOBJECTPTR_ZEROINIT_FIX
        inline constexpr int32 InvalidWeakObjectIndex = 0;
#else
        inline constexpr int32 InvalidWeakObjectIndex = ::INDEX_NONE;
#endif
    }

    
    struct RC_UE_API FWeakObjectPtr
    {
    private:
        auto InternalGetObjectItem() const -> struct FUObjectItem*;
        auto InternalGet(bool bEvenIfPendingKill) const -> class UObject*;
        auto Reset() -> void;
        FORCEINLINE int32 GetObjectIndex_Private() const
        {
            return ObjectIndex;
        }
        FORCEINLINE FUObjectItem* Internal_GetObjectItem() const
        {
            using namespace UE::Core::Private;

            if (ObjectSerialNumber == 0)
            {
#if UE_WEAKOBJECTPTR_ZEROINIT_FIX
                checkSlow(ObjectIndex == InvalidWeakObjectIndex); // otherwise this is a corrupted weak pointer
#else
                checkSlow(ObjectIndex == 0 || ObjectIndex == -1); // otherwise this is a corrupted weak pointer
#endif

                return nullptr;
            }

            if (ObjectIndex < 0)
            {
                return nullptr;
            }
            FUObjectItem* const ObjectItem = FUObjectArray::IndexToObject(ObjectIndex);
            if (!ObjectItem)
            {
                return nullptr;
            }
            if (!SerialNumbersMatch(ObjectItem))
            {
                return nullptr;
            }
            return ObjectItem;
        }
        /** Private (inlined) version for internal use only. */
        FORCEINLINE_DEBUGGABLE bool Internal_IsValid(bool bEvenIfGarbage, bool bThreadsafeTest) const
        {
            FUObjectItem* const ObjectItem = Internal_GetObjectItem();
            if (bThreadsafeTest)
            {
                return (ObjectItem != nullptr);
            }
            else
            {
                return (ObjectItem != nullptr) && ObjectItem->IsValid(bEvenIfGarbage);
            }
        }

    public:
        // Default constructor - initializes to invalid
        FWeakObjectPtr() 
            : ObjectIndex(UE::Core::Private::InvalidWeakObjectIndex)
            , ObjectSerialNumber(0)
        {
        }

        // Constructor from UObject pointer
        explicit FWeakObjectPtr(class UObject* Object)
            : ObjectIndex(UE::Core::Private::InvalidWeakObjectIndex)
            , ObjectSerialNumber(0)
        {
            if (Object)
            {
                *this = Object;
            }
        }

        // Constructor from const UObject pointer
        explicit FWeakObjectPtr(const class UObject* Object)
            : ObjectIndex(UE::Core::Private::InvalidWeakObjectIndex)
            , ObjectSerialNumber(0)
        {
            if (Object)
            {
                *this = const_cast<class UObject*>(Object);
            }
        }

        auto operator=(class UObject*) -> void;

    public:
#if UE_WEAKOBJECTPTR_ZEROINIT_FIX
        int32       ObjectIndex = UE::Core::Private::InvalidWeakObjectIndex;
        int32       ObjectSerialNumber = 0;
#else
        int32       ObjectIndex;
        int32       ObjectSerialNumber;
#endif
        /**  
         * Dereference the weak pointer.
         * @param bEvenIfGarbage if this is true, Garbage objects are considered valid
         * @return nullptr if this object is gone or the weak pointer is explicitly null, otherwise a valid uobject pointer
         */
        class UObject* Get(bool bEvenIfGarbage) const;
        
        /** Dereference the weak pointer even if it is marked as Garbage or Unreachable */
        class UObject* GetEvenIfUnreachable() const;
        
        auto SerialNumbersMatch(struct FUObjectItem*) const -> bool;
        auto Get() const -> class UObject*;
    };

    template<class T=UObject, class TWeakObjectPtrBase=FWeakObjectPtr>
    struct TWeakObjectPtr;

    template<class T, class TWeakObjectPtrBase>
    struct TWeakObjectPtr : private TWeakObjectPtrBase
    {
        // Default constructor
        TWeakObjectPtr() = default;

        // Constructor from object pointer
        explicit TWeakObjectPtr(T* Object)
            : TWeakObjectPtrBase(Object)
        {
        }

        // Constructor from const object pointer
        explicit TWeakObjectPtr(const T* Object)
            : TWeakObjectPtrBase(Object)
        {
        }

        T* Get()
        {
            return std::bit_cast<T*>(TWeakObjectPtrBase::Get());
        }
    };
}