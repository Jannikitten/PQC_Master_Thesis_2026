//
// Created by Jannik Verdoner on 26/02/2026.
//

#ifndef PQC_MASTER_THESIS_2026_IMAGE_H
#define PQC_MASTER_THESIS_2026_IMAGE_H

#include <string>
#include <vulkan/vulkan.h>

namespace Safira {
    enum class ImageFormat {
        None = 0,
        RGBA,
        RGBA32F
    };

    class Image {
    public:
        explicit Image(std::string_view path);
        Image(uint32_t width, uint32_t height, ImageFormat format, const void* data = nullptr);
        ~Image();

        void SetData(const void* data);

        [[nodiscard]] VkDescriptorSet GetDescriptorSet() const { return m_DescriptorSet; }

        void Resize(uint32_t width, uint32_t height);

        [[nodiscard]] uint32_t GetWidth() const { return m_Width; }
        [[nodiscard]] uint32_t GetHeight() const { return m_Height; }

        static void* Decode(const void* data, uint64_t length, uint32_t& outWidth, uint32_t& outHeight);
    private:
        void AllocateMemory(uint64_t size);
        void Release();

        uint32_t m_Width = 0, m_Height = 0;

        VkImage m_Image = nullptr;
        VkImageView m_ImageView = nullptr;
        VkDeviceMemory m_Memory = nullptr;
        VkSampler m_Sampler = nullptr;

        ImageFormat m_Format = ImageFormat::None;

        VkBuffer m_StagingBuffer = nullptr;
        VkDeviceMemory m_StagingBufferMemory = nullptr;

        size_t m_AlignedSize = 0;

        VkDescriptorSet m_DescriptorSet = nullptr;

        std::string m_Filepath;
    };
}

#endif //PQC_MASTER_THESIS_2026_IMAGE_H