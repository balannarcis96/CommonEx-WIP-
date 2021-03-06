#include "Task.h"
#pragma once
/**
 * @file Memory.h
 *
 * @brief Memory base abstractions
 *
 * @author Balan Narcis
 * Contact: balannarcis96@gmail.com
 *
 */

namespace CommonEx
{
	using MemoryBlockDestroyCallback = _TaskEx<sizeof(ptr_t), void(bool)>;

	class alignas(ALIGNMENT) NotSharedMemoryResourceBase
	{
	public:
		//Destroy callback (deleter) void(bool bCallDestructor)
		MemoryBlockDestroyCallback	Destroy{  };
	};

	//base for all Memory Resource objects
	class alignas(ALIGNMENT) MemoryResourceBase: public NotSharedMemoryResourceBase
	{
	public:
		//Object's reference count
		mutable uint32_t RefCount{ 1 };

		//Object's flags
		union
		{
			struct
			{
				unsigned bDontDestruct : 1;
			};

			uint32_t MemoryResourceFlags{ 0 };
		};

		//Is this instance waiting to be destroyed ?
		std::atomic_flag bIsPendingDestroy{};

		/**
		 * \brief Set the bIsPendingDestroy flag value and notifies other waiting threads if possible.
		 * 
		 * \param bValue New bIsPendingDestroy value.
		 * \param bNotify If true it will notify all waiting threads.
		 */
		FORCEINLINE void SetIsPendingDestroy(bool bValue, bool bNotify = true)noexcept
		{
			if (bValue)
			{
				bIsPendingDestroy.test_and_set(std::memory_order::memory_order_acq_rel);
			}
			else
			{
				bIsPendingDestroy.clear(std::memory_order::memory_order_release);
			}

			if (bNotify)
			{
				bIsPendingDestroy.notify_all();
			}
		}

		/**
		 * \return true if this instance is pending destroy.
		 */
		FORCEINLINE bool IsPendingDestroy() const noexcept
		{
			return bIsPendingDestroy.test(std::memory_order::acquire);
		}

		FORCEINLINE uint32_t GetRefCount() const noexcept
		{
			return InterlockedCompareExchange(&RefCount, 0, 0);
		}

		/**
		 * \brief Wait for the bIsPendingDestroy to change its value to [bValue].
		 * 
		 * \param bValue The value to wait for.
		 */
		FORCEINLINE void WaitForPendingDestroy(bool bValue) noexcept
		{
			bIsPendingDestroy.wait(bValue, std::memory_order::acquire);
		}

		/**
		 * \brief Reinitialize the main values of this instance.
		 */
		FORCEINLINE void ResetResource()noexcept
		{
			//clear ref count
			_InterlockedExchange_HLERelease((volatile long*)&RefCount, 1);

			//clear destroy flag
			SetIsPendingDestroy(false);
		}
	};

	template<bool bAtomicRef = true>
	class MemoryResource : public MemoryResourceBase
	{
	public:
		/**
		 * \brief Add 1 to the reference count of this instance. 
		 * 
		 * \important Only call this function while holding a valid reference this the instance.
		 */
		FORCEINLINE void AddReference() const noexcept
		{
			if constexpr (bAtomicRef)
			{
				auto& VolatileRefCount = reinterpret_cast<volatile long&>(this->RefCount);

				long RefCount = __iso_volatile_load32(reinterpret_cast<volatile int*>(&VolatileRefCount));
				while (RefCount != 0)
				{
					const long OldValue = _InterlockedCompareExchange(&VolatileRefCount, RefCount + 1, RefCount);
					if (OldValue == RefCount)
					{
						return;
					}

					RefCount = OldValue;
				}
			}
			else
			{
				RefCount++;
			}
		}

		/**
		 * \brief Remove 1 from the reference count of this instance.
		 * 
		 * \returns true if this call Removed the last reference (RefCount == 0) otherwise it returns false.
		 */
		FORCEINLINE bool ReleaseReference() const noexcept
		{
			if constexpr (bAtomicRef)
			{
				if (_InterlockedDecrement(reinterpret_cast<volatile long*>(&this->RefCount)) == 0)
				{
					return true;
				}
			}
			else
			{
				RefCount--;

				if (RefCount == 0)
				{
					return true;
				}
			}

			return false;
		}

		/**
		 * \brief Remove 1 from the reference count of this instance and tried to call the Destroy(true) handler [See full description].
		 * 
		 * \important If the Destroy handler is not set, delete is called on the [this] pointer.
		 * 
		 * \returns true if the current call removed the last reference (RefCount == 0) otherwise it returns false.
		 */
		FORCEINLINE bool ReleaseReferenceAndDestroy() noexcept
		{
			if (ReleaseReference())
			{
				if (Destroy)
				{
					Destroy(true);
				}
				else
				{
					GFreeCpp(this);
				}

				return true;
			}

			return false;
		}
	};

	class MemoryBlockBase
	{
	public:
		ulong_t					const	BlockSize{ 0 };
		ulong_t					const	ElementSize{ 0 };
		ulong_t					const	ElementsCount{ 1 };
		uint8_t* PTR					Block{ nullptr };

		FORCEINLINE MemoryBlockBase(ulong_t BlockSize, uint8_t* Block, ulong_t ElementSize) noexcept
			: BlockSize(BlockSize)
			, ElementSize(ElementSize)
			, Block(Block)
		{}

		FORCEINLINE MemoryBlockBase(ulong_t BlockSize, uint8_t* Block, ulong_t ElementSize, ulong_t ElementsCount) noexcept
			: BlockSize(BlockSize)
			, ElementSize(ElementSize)
			, ElementsCount(ElementsCount)
			, Block(Block)
		{}

		//Cant copy
		MemoryBlockBase(const MemoryBlockBase&) = delete;
		MemoryBlockBase& operator=(const MemoryBlockBase&) = delete;

		//Cant move
		MemoryBlockBase(MemoryBlockBase&&) = delete;
		MemoryBlockBase& operator=(MemoryBlockBase&& Other) = delete;

		FORCEINLINE const uint8_t* CanFit(ulong_t Length, ulong_t StartOffset = 0) const noexcept
		{
			if (BlockSize < (Length + StartOffset))
			{
				return nullptr;
			}

			return Block + StartOffset;
		}

		FORCEINLINE const uint8_t* GetBegin(ulong_t StartOffset = 0) const noexcept
		{
			return Block + StartOffset;
		}

		FORCEINLINE const uint8_t* GetEnd() const noexcept
		{
			return Block + BlockSize;
		}

		FORCEINLINE void ZeroMemoryBlock() noexcept
		{
			memset(
				Block,
				0,
				BlockSize
			);
		}
	};

	class MemoryBlockBaseResource : public MemoryResource<true>, public MemoryBlockBase
	{
	public:
		using Base1 = MemoryResource<true>;
		using Base2 = MemoryBlockBase;

		MemoryBlockBaseResource(ulong_t BlockSize, uint8_t* Block, ulong_t ElementSize) noexcept
			:Base1()
			, Base2(BlockSize, Block, ElementSize)
		{}

		MemoryBlockBaseResource(ulong_t BlockSize, uint8_t* Block, ulong_t ElementSize, ulong_t ElementsCount) noexcept
			:Base1()
			, Base2(BlockSize, Block, ElementSize, ElementsCount)
		{}
	};

	template<ulong_t Size, bool bIsMemoryResource = false>
	class MemoryBlock : public MemoryBlockBase
	{
		static_assert(Size% ALIGNMENT == 0, "Size of MemoryBlock<Size> must be a multiple of ALIGNMENT");

	public:
		uint8_t		FixedSizeBlock[Size];

		MemoryBlock(ulong_t ElementSize) noexcept
			: MemoryBlockBase(Size, FixedSizeBlock, ElementSize)
		{}

		MemoryBlock(ulong_t ElementSize, ulong_t ElementsCount) noexcept
			: MemoryBlockBase(Size, FixedSizeBlock, ElementSize, ElementsCount)
		{}
	};

	template<ulong_t Size>
	class MemoryBlock<Size, true> : public MemoryBlockBaseResource
	{
		static_assert(Size% ALIGNMENT == 0, "Size of MemoryBlock<Size> must be a multiple of ALIGNMENT");

	public:
		uint8_t		FixedSizeBlock[Size];

		MemoryBlock(ulong_t ElementSize) noexcept
			: MemoryBlockBaseResource(Size, FixedSizeBlock, ElementSize)
		{}

		MemoryBlock(ulong_t ElementSize, ulong_t ElementsCount) noexcept
			: MemoryBlockBaseResource(Size, FixedSizeBlock, ElementSize, ElementsCount)
		{}
	};

	template<bool bIsMemoryResource = false>
	class CustomBlock : public MemoryBlockBase
	{
	public:
		CustomBlock(ulong_t Size, ulong_t ElementSize) noexcept
			: MemoryBlockBase(Size, nullptr, ElementSize)
		{
			Block = (uint8_t*)GAllocate(sizeof(uint8_t) * Size, ALIGNMENT);
		}

		CustomBlock(ulong_t Size, ulong_t ElementSize, ulong_t ElementsCount) noexcept
			: MemoryBlockBase(Size, nullptr, ElementSize, ElementsCount)
		{
			Block = (uint8_t*)GAllocate(sizeof(uint8_t) * Size, ALIGNMENT);
		}
	};

	template<>
	class CustomBlock<true> : public MemoryBlockBaseResource
	{
	public:
		CustomBlock(ulong_t Size, ulong_t ElementSize) noexcept
			: MemoryBlockBaseResource(Size, nullptr, ElementSize)
		{
			Block = (uint8_t*)GAllocate(sizeof(uint8_t) * Size, ALIGNMENT);
		}

		CustomBlock(ulong_t Size, ulong_t ElementSize, ulong_t ElementsCount) noexcept
			: MemoryBlockBaseResource(Size, nullptr, ElementSize, ElementsCount)
		{
			Block = (uint8_t*)GAllocate(sizeof(uint8_t) * Size, ALIGNMENT);
		}
	};

	template<bool bIsMemoryResource = false>
	class CustomBlockHeader : public MemoryBlockBase
	{
	public:
		CustomBlockHeader(ulong_t Size, ulong_t ElementSize)
			: MemoryBlockBase(Size, nullptr, ElementSize)
		{
			Block = (reinterpret_cast<uint8_t*>(this) + sizeof(CustomBlockHeader));
		}

		CustomBlockHeader(ulong_t Size, ulong_t ElementSize, ulong_t ElementsCount)
			: MemoryBlockBase(Size, nullptr, ElementSize, ElementsCount)
		{
			Block = (reinterpret_cast<uint8_t*>(this) + sizeof(CustomBlockHeader));
		}
	};

	template<>
	class CustomBlockHeader<true> : public MemoryBlockBaseResource
	{
	public:
		CustomBlockHeader(ulong_t Size, ulong_t ElementSize)
			: MemoryBlockBaseResource(Size, nullptr, ElementSize)
		{
			Block = (reinterpret_cast<uint8_t*>(this) + sizeof(CustomBlockHeader));
		}

		CustomBlockHeader(ulong_t Size, ulong_t ElementSize, ulong_t ElementsCount)
			: MemoryBlockBaseResource(Size, nullptr, ElementSize, ElementsCount)
		{
			Block = (reinterpret_cast<uint8_t*>(this) + sizeof(CustomBlockHeader));
		}
	};
}
