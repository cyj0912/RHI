#include "BufferVk.h"
#include "DeviceVk.h"

#include <cstring>

namespace RHI
{

CBufferVk::CBufferVk(CDeviceVk& p, size_t size, EBufferUsageFlags usage, const void* initialData)
    : CBuffer(size, usage)
    , Parent(p)
{
    VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = size;

    VmaAllocationCreateInfo allocInfo = {};

    bool gpuOnly = true;

    if (Any(usage, EBufferUsageFlags::Dynamic))
        gpuOnly = false;

    if (Any(usage, EBufferUsageFlags::Index))
        bufferInfo.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (Any(usage, EBufferUsageFlags::Vertex))
        bufferInfo.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (Any(usage, EBufferUsageFlags::IndirectDraw))
        bufferInfo.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (Any(usage, EBufferUsageFlags::Uniform))
        bufferInfo.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (Any(usage, EBufferUsageFlags::Storage))
        bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (Any(usage, EBufferUsageFlags::UniformTexel))
        bufferInfo.usage |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    if (Any(usage, EBufferUsageFlags::StorageTexel))
        bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

    if (gpuOnly)
    {
        bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    }
    else if (Any(usage, EBufferUsageFlags::Dynamic))
    {
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    }
    else if (Any(usage, EBufferUsageFlags::Upload))
    {
        bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    }
    else if (Any(usage, EBufferUsageFlags::Readback))
    {
        bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
    }

    vmaCreateBuffer(Parent.GetAllocator(), &bufferInfo, &allocInfo, &Buffer, &Allocation, nullptr);

    if (initialData && gpuOnly)
    {
        // Prepare a staging buffer
        VkBufferCreateInfo stgbufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        stgbufferInfo.size = size;
        stgbufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo stgallocInfo = {};
        stgallocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;

        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VmaAllocation stagingAlloc = VK_NULL_HANDLE;

        vmaCreateBuffer(Parent.GetAllocator(), &stgbufferInfo, &stgallocInfo, &stagingBuffer,
                        &stagingAlloc, nullptr);

        void* mappedData;
        vmaMapMemory(Parent.GetAllocator(), stagingAlloc, &mappedData);
        memcpy(mappedData, initialData, size);
        vmaUnmapMemory(Parent.GetAllocator(), stagingAlloc);

        // Copy the content
        auto cmdList = Parent.GetDefaultCopyQueue()->CreateCommandList();
        cmdList->Enqueue();
        auto ctx = std::static_pointer_cast<CCommandContextVk>(cmdList->CreateCopyContext());
        auto cmdBuffer = ctx->GetCmdBuffer();
        VkBufferCopy copy;
        copy.srcOffset = 0;
        copy.dstOffset = 0;
        copy.size = size;
        vkCmdCopyBuffer(cmdBuffer, stagingBuffer, Buffer, 1, &copy);
        ctx->FinishRecording();
        cmdList->Commit();
        Parent.GetDefaultCopyQueue()->Flush();

        Parent.AddPostFrameCleanup([stagingBuffer, stagingAlloc](CDeviceVk& p) {
            vmaDestroyBuffer(p.GetAllocator(), stagingBuffer, stagingAlloc);
        });
    }
    else if (initialData)
    {
        void* mappedData;
        vmaMapMemory(Parent.GetAllocator(), Allocation, &mappedData);
        memcpy(mappedData, initialData, size);
        vmaUnmapMemory(Parent.GetAllocator(), Allocation);
    }
}

CBufferVk::~CBufferVk()
{
    auto b = Buffer;
    auto a = Allocation;
    Parent.AddPostFrameCleanup([b, a](CDeviceVk& p) { vmaDestroyBuffer(p.GetAllocator(), b, a); });
}

void* CBufferVk::Map(size_t offset, size_t size)
{
    void* result;
    vmaMapMemory(Parent.GetAllocator(), Allocation, &result);
    return static_cast<uint8_t*>(result) + offset;
}

void CBufferVk::Unmap() { vmaUnmapMemory(Parent.GetAllocator(), Allocation); }

CPersistentMappedRingBuffer::CPersistentMappedRingBuffer(CDeviceVk& p, size_t size,
                                                         VkBufferUsageFlags usage)
    : Parent(p)
    , TotalSize(size)
{
    Remaining = TotalSize;

    VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = TotalSize;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    vmaCreateBuffer(Parent.GetAllocator(), &bufferInfo, &allocInfo, &Handle, &Allocation, nullptr);

    vmaMapMemory(Parent.GetAllocator(), Allocation, &MappedData);
}

CPersistentMappedRingBuffer::~CPersistentMappedRingBuffer()
{
    vmaUnmapMemory(Parent.GetAllocator(), Allocation);
    vmaDestroyBuffer(Parent.GetAllocator(), Handle, Allocation);
}

void* CPersistentMappedRingBuffer::Allocate(size_t size, size_t alignment, size_t& outOffset)
{
    if (CurrBlock.End + size + alignment > TotalSize)
    {
        size_t wastedSpace = TotalSize - CurrBlock.End;
        if (wastedSpace > Remaining)
            return nullptr; // Not enough free space to wrap around
        // Wrap around
        CurrBlock.End = 0;
        Remaining -= wastedSpace;
    }
    size_t allocOffset = (CurrBlock.End + alignment - 1) / alignment * alignment;
    size_t wastedOnAlighment = allocOffset - CurrBlock.End;
    if (allocOffset + size > TotalSize)
        return nullptr;

    Remaining = Remaining - wastedOnAlighment - size;
    CurrBlock.End = allocOffset + size;

    outOffset = allocOffset;
    return reinterpret_cast<void*>(reinterpret_cast<size_t>(MappedData) + allocOffset);
}

void CPersistentMappedRingBuffer::MarkBlockEnd()
{
    vmaFlushAllocation(Parent.GetAllocator(), Allocation, CurrBlock.Begin,
                       CurrBlock.End - CurrBlock.Begin);

    AllocatedBlocks.push(CurrBlock);
    CurrBlock.Begin = CurrBlock.End;
    if (CurrBlock.Begin == TotalSize)
        CurrBlock.Begin = 0;
    CurrBlock.End = CurrBlock.Begin;
}

void CPersistentMappedRingBuffer::FreeBlock()
{
    const auto& firstBlock = AllocatedBlocks.front();
    size_t blockSize = firstBlock.End - firstBlock.Begin;
    if (firstBlock.End < firstBlock.Begin)
        blockSize = firstBlock.End - firstBlock.Begin + TotalSize;
    Remaining += blockSize;
    assert(Remaining <= TotalSize);
    AllocatedBlocks.pop();
}

}
