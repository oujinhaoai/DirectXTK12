//--------------------------------------------------------------------------------------
// File: GraphicsMemory.cpp
//
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
// http://go.microsoft.com/fwlink/?LinkID=615561
//--------------------------------------------------------------------------------------

#include "pch.h"
#include "GraphicsMemory.h"
#include "PlatformHelpers.h"
#include "LinearAllocator.h"

#include <atomic>

using namespace DirectX;
using Microsoft::WRL::ComPtr;
using ScopedLock = std::lock_guard<std::mutex>;

namespace
{
    static const size_t MinPageSize = 64 * 1024;
    static const size_t MinAllocSize = 4 * 1024;
    static const size_t AllocatorIndexShift = 12; // start block sizes at 4KB
    static const size_t AllocatorPoolCount = 21; // allocation sizes up to 2GB supported
    static const size_t PoolIndexScale = 1; // multiply the allocation size this amount to push large values into the next bucket

    static_assert((1 << AllocatorIndexShift) == MinAllocSize, "1 << AllocatorIndexShift must == MinPageSize (in KiB)");
    static_assert((MinPageSize & (MinPageSize - 1)) == 0, "MinPageSize size must be a power of 2");
    static_assert((MinAllocSize & (MinAllocSize - 1)) == 0, "MinAllocSize size must be a power of 2");
    static_assert(MinAllocSize >= (4 * 1024), "MinAllocSize size must be greater than 4K");

    inline constexpr bool WordSize64()
    {
        return sizeof(size_t) == 8;
    }

    inline size_t NextPow2(size_t x)
    {
        x--;
        x |= x >> 1;
        x |= x >> 2;
        x |= x >> 4;
        x |= x >> 8;
        x |= x >> 16;
#ifdef _WIN64
        x |= x >> 32;
#endif
        return ++x;
    }

    inline size_t GetPoolIndexFromSize(size_t x)
    {
        size_t allocatorPageSize = x >> AllocatorIndexShift;
        // gives a value from range:
        // 0 - sub-4k allocator
        // 1 - 4k allocator
        // 2 - 8k allocator
        // 4 - 16k allocator
        // etc...
        // Need to convert to an index.
        DWORD bitIndex = 0;
#ifdef _WIN64
        return _BitScanForward64(&bitIndex, allocatorPageSize) ? bitIndex + 1 : 0;
#else
        return _BitScanForward(&bitIndex, (DWORD)allocatorPageSize) ? bitIndex + 1 : 0;
#endif
    }

    inline size_t GetPageSizeFromPoolIndex(size_t x)
    {
        x = (x == 0) ? 0 : x - 1; // clamp to zero
        return std::max<size_t>(MinPageSize, size_t(1) << (x + AllocatorIndexShift));
    }

    //--------------------------------------------------------------------------------------
    // DeviceAllocator : honors memory requests associated with a particular device
    //--------------------------------------------------------------------------------------
    class DeviceAllocator
    {
    public:
        DeviceAllocator(_In_ ComPtr<ID3D12Device> device)
            : mDevice(device)
        {
            for (size_t i = 0; i < mPools.size(); ++i)
            {
                size_t pageSize = GetPageSizeFromPoolIndex(i);
                mPools[i] = std::make_unique<LinearAllocator>(
                    mDevice.Get(),
                    pageSize);
            }
        }

        GraphicsResource Alloc(_In_ size_t size, _In_ size_t alignment)
        {
            ScopedLock lock(mMutex);

            // Which memory pool does it live in?
            size_t poolSize = NextPow2((alignment + size) * PoolIndexScale);
            size_t poolIndex = GetPoolIndexFromSize(poolSize);
            assert(poolIndex < mPools.size());

            // If the allocator isn't initialized yet, do so now
            auto& allocator = mPools[poolIndex];
            assert(allocator != nullptr);
            assert(poolSize < MinPageSize || poolSize == allocator->PageSize());

            LinearAllocatorPage* page = allocator->FindPageForAlloc(size, alignment);
            if (page == nullptr)
                throw std::exception("Out of memory");

            size_t offset = page->Suballocate(size, alignment);

            // Return the information to the user
            return std::move(GraphicsResource(
                page,
                page->GpuAddress() + offset,
                page->UploadResource(),
                reinterpret_cast<BYTE*>(page->BaseMemory()) + offset,
                offset,
                size));
        }

        // Submit page fences to the command queue
        void KickFences(_In_ ID3D12CommandQueue* commandQueue)
        {
            ScopedLock lock(mMutex);

            for (auto& i : mPools)
            {
                if (i != nullptr)
                {
                    i->RetirePendingPages();
                    i->FenceCommittedPages(commandQueue);
                }
            }
        }

        void GarbageCollect()
        {
            ScopedLock lock(mMutex);

            for (auto& i : mPools)
            {
                if (i != nullptr)
                {
                    i->Shrink();
                }
            }
        }

    private:
        ComPtr<ID3D12Device> mDevice;
        std::array<std::unique_ptr<LinearAllocator>, AllocatorPoolCount> mPools;
        std::mutex mMutex;
    };
} // anonymous namespace


//--------------------------------------------------------------------------------------
// GraphicsMemory::Impl
//--------------------------------------------------------------------------------------

class GraphicsMemory::Impl
{
public:
    Impl(GraphicsMemory* owner)
        : mOwner(owner)
    {
        if (s_graphicsMemory)
        {
            throw std::exception("GraphicsMemory is a singleton");
        }

        s_graphicsMemory = this;
    }

    ~Impl()
    {
        mDeviceAllocator.reset();
        s_graphicsMemory = nullptr;
    }

    void Initialize(_In_ ID3D12Device* device)
    {
        mDeviceAllocator = std::make_unique<DeviceAllocator>(device);
    }

    GraphicsResource Allocate(size_t size, size_t alignment)
    {
        return std::move(mDeviceAllocator->Alloc(size, alignment));
    }

    void Commit(_In_ ID3D12CommandQueue* commandQueue)
    {
        mDeviceAllocator->KickFences(commandQueue);
    }

    void GarbageCollect()
    {
        mDeviceAllocator->GarbageCollect();
    }

    GraphicsMemory* mOwner;
    static GraphicsMemory::Impl* s_graphicsMemory;

private:
    std::unique_ptr<DeviceAllocator> mDeviceAllocator;
};

GraphicsMemory::Impl* GraphicsMemory::Impl::s_graphicsMemory = nullptr;


//--------------------------------------------------------------------------------------
// GraphicsMemory
//--------------------------------------------------------------------------------------

// Public constructor.
GraphicsMemory::GraphicsMemory(_In_ ID3D12Device* device)
    : pImpl(new Impl(this))
{
    pImpl->Initialize(device);
}


// Move constructor.
GraphicsMemory::GraphicsMemory(GraphicsMemory&& moveFrom)
    : pImpl(std::move(moveFrom.pImpl))
{
    pImpl->mOwner = this;
}


// Move assignment.
GraphicsMemory& GraphicsMemory::operator= (GraphicsMemory&& moveFrom)
{
    pImpl = std::move(moveFrom.pImpl);
    pImpl->mOwner = this;
    return *this;
}


// Public destructor.
GraphicsMemory::~GraphicsMemory()
{
}

GraphicsResource GraphicsMemory::Allocate(size_t size, size_t alignment)
{
    return std::move(pImpl->Allocate(size, alignment));
}


void GraphicsMemory::Commit(_In_ ID3D12CommandQueue* commandQueue)
{
    pImpl->Commit(commandQueue);
}

void GraphicsMemory::GarbageCollect()
{
    pImpl->GarbageCollect();
}


GraphicsMemory& GraphicsMemory::Get()
{
    if (!Impl::s_graphicsMemory || !Impl::s_graphicsMemory->mOwner)
        throw std::exception("GraphicsMemory singleton not created");

    return *Impl::s_graphicsMemory->mOwner;
}


//--------------------------------------------------------------------------------------
// GraphicsResource smart-pointer interface
//--------------------------------------------------------------------------------------

GraphicsResource::GraphicsResource()
    : mPage(nullptr)
    , mGpuAddress {}
    , mResource(nullptr)
    , mMemory(nullptr)
    , mBufferOffset(0)
    , mSize(0)
{
}

GraphicsResource::GraphicsResource(
    _In_ LinearAllocatorPage* page,
    _In_ D3D12_GPU_VIRTUAL_ADDRESS gpuAddress,
    _In_ ID3D12Resource* resource,
    _In_ void* memory,
    _In_ size_t offset,
    _In_ size_t size)
    : mPage(page)
    , mGpuAddress(gpuAddress)
    , mResource(resource)
    , mMemory(memory)
    , mBufferOffset(offset)
    , mSize(size)
{
    mPage->AddRef();
}

GraphicsResource::GraphicsResource(GraphicsResource&& other)
    : mPage(nullptr)
{
    Reset(std::move(other));
}

GraphicsResource::~GraphicsResource()
{
    if (mPage != nullptr)
    {
        mPage->Release();
    }
}

GraphicsResource&& GraphicsResource::operator= (GraphicsResource&& other)
{
    Reset(std::move(other));
    return std::move(*this);
}

void GraphicsResource::Reset()
{
    if (mPage != nullptr)
    {
        mPage->Release();
    }

    mPage = nullptr;
    mGpuAddress = {};
    mResource = nullptr;
    mMemory = nullptr;
    mBufferOffset = 0;
    mSize = 0;
}

void GraphicsResource::Reset(GraphicsResource&& alloc)
{
    if (mPage != nullptr)
    {
        mPage->Release();
    }

    mGpuAddress = alloc.GpuAddress();
    mResource = alloc.Resource();
    mMemory = alloc.Memory();
    mBufferOffset = alloc.ResourceOffset();
    mSize = alloc.Size();
    mPage = alloc.mPage;

    alloc.mGpuAddress = {};
    alloc.mResource = nullptr;
    alloc.mMemory = nullptr;
    alloc.mBufferOffset = 0;
    alloc.mSize = 0;
    alloc.mPage = nullptr;
}


//--------------------------------------------------------------------------------------
// SharedGraphicsResource
//--------------------------------------------------------------------------------------

SharedGraphicsResource::SharedGraphicsResource()
    : mSharedResource(nullptr)
{
}

SharedGraphicsResource::SharedGraphicsResource(GraphicsResource&& resource)
    : mSharedResource(std::make_shared<GraphicsResource>(std::move(resource)))
{
}

SharedGraphicsResource::SharedGraphicsResource(SharedGraphicsResource&& resource)
    : mSharedResource(std::move(resource.mSharedResource))
{
}

SharedGraphicsResource::SharedGraphicsResource(const SharedGraphicsResource& resource)
    : mSharedResource(resource.mSharedResource)
{
}

SharedGraphicsResource::~SharedGraphicsResource()
{
}

SharedGraphicsResource&& SharedGraphicsResource::operator= (SharedGraphicsResource&& resource)
{
    mSharedResource = std::move(resource.mSharedResource);
    return std::move(*this);
}

SharedGraphicsResource&& SharedGraphicsResource::operator= (GraphicsResource&& resource)
{
    mSharedResource = std::make_shared<GraphicsResource>(std::move(resource));
    return std::move(*this);
}

SharedGraphicsResource& SharedGraphicsResource::operator= (const SharedGraphicsResource& resource)
{
    mSharedResource = resource.mSharedResource;
    return *this;
}

void SharedGraphicsResource::Reset()
{
    mSharedResource.reset();
}

void SharedGraphicsResource::Reset(GraphicsResource&& resource)
{
    mSharedResource = std::make_shared<GraphicsResource>(std::move(resource));
}

void SharedGraphicsResource::Reset(SharedGraphicsResource&& resource)
{
    mSharedResource = std::move(resource.mSharedResource);
}

void SharedGraphicsResource::Reset(const SharedGraphicsResource& resource)
{
    mSharedResource = resource.mSharedResource;
}

