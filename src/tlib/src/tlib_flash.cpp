#include "../inc/tlib_flash.h"
#include <cstdint>
#include <cassert>
#include <print>

#include <config.h>

enum class TensorType : uint8_t
{
    UINT8 = 0U,
    INT8 = 1U,
    UINT16 = 2U,
    INT16 = 3U,
    UINT32 = 4U,
    INT32 = 5U,
    INT64 = 6U,
    FLOAT32 = 7U
};

struct TensorHeader
{
    TensorType type;
    uint8_t dims;
    uint32_t shape[kFlashTensorMaxDims];
    uint32_t stride[kFlashTensorMaxDims];
    uint8_t shift;
    uint32_t numel;
    bool quantized;
};

struct FileHeader
{
    uint32_t n_tensors;
    uint64_t file_size;
};

static constexpr std::string kFileMagic{"CONFORMER1"};
std::function<const uint8_t *(uint64_t, std::size_t)> flash_mmap_callback_;
uint64_t flash_address_{0};
const uint32_t *tensor_byte_offsets_{};
const uint8_t *tensor_data_{};
FileHeader file_header_{};

namespace tlib::flash
{

    FlashError parsing_error_{FlashError::Uninitialized};

    namespace utils
    {

        const uint8_t *AlignTensorDataPtr(const uint8_t *ptr)
        {
            uint32_t remainder = (uintptr_t)ptr % kFlashTensorAlignment;
            if (remainder == 0)
            {
                return ptr;
            }
            else
            {
                return ptr + (kFlashTensorAlignment - remainder);
            }
        }

    } // namespace utils

    FlashError ParseFileHeader(void)
    {
        // Request to map meta data sector
        auto read_ptr = flash_mmap_callback_(flash_address_, kFlashMetaPartitionSize);
        if (read_ptr == nullptr)
        {
            return FlashError::MetaSectorMountFailed;
        }

        if (kFileMagic.compare(0, kFileMagic.length(), reinterpret_cast<const char *>(read_ptr)) != 0)
        {
            return FlashError::MagicInvalid;
        }

        read_ptr += kFileMagic.length() + 1;

        file_header_.n_tensors = *reinterpret_cast<const uint32_t *>(read_ptr);
        read_ptr += sizeof(uint32_t);
        file_header_.file_size = *reinterpret_cast<const uint64_t *>(read_ptr);
        read_ptr += sizeof(uint64_t);

        if (file_header_.n_tensors == 0)
        {
            return FlashError::TensorCountInvalid;
        }

        if (file_header_.file_size == 0)
        {
            return FlashError::FileSizeInvalid;
        }

        tensor_byte_offsets_ = reinterpret_cast<const uint32_t *>(read_ptr);

        return FlashError::None;
    }

    FlashError Initialize(uint64_t flash_address, const std::function<const uint8_t *(uint64_t, std::size_t)> &flash_mmap_callback)
    {
        flash_address_ = flash_address;
        flash_mmap_callback_ = flash_mmap_callback;

        parsing_error_ = ParseFileHeader();

        if (parsing_error_ != FlashError::None)
        {
            return parsing_error_;
        }

        // Request to map tensor data sector
        tensor_data_ = flash_mmap_callback_(flash_address + kFlashMetaPartitionSize, file_header_.file_size - kFlashMetaPartitionSize);
        if (tensor_data_ == nullptr)
        {
            parsing_error_ = FlashError::TensorSectorMountFailed;
        }

        return parsing_error_;
    }

    template <typename dtype>
    constexpr bool CheckTensorType(const TensorType &tensor_type);

    template <>
    constexpr bool CheckTensorType<int8_t>(const TensorType &tensor_type)
    {
        return tensor_type == TensorType::INT8;
    }

    template <>
    constexpr bool CheckTensorType<int16_t>(const TensorType &tensor_type)
    {
        return tensor_type == TensorType::INT16;
    }

    template <>
    constexpr bool CheckTensorType<int32_t>(const TensorType &tensor_type)
    {
        return tensor_type == TensorType::INT32;
    }

    template <>
    constexpr bool CheckTensorType<float>(const TensorType &tensor_type)
    {
        return tensor_type == TensorType::FLOAT32;
    }

    template <typename dtype>
    TensorView<dtype> MapTensor(const uint32_t tensor_id)
    {
        const uint32_t byte_offset = tensor_byte_offsets_[tensor_id];
        const uint8_t *read_ptr = tensor_data_ + byte_offset;

        // Deserialize tensor header
        const TensorType tensor_type = *reinterpret_cast<const TensorType *>(read_ptr);
        read_ptr += sizeof(TensorType);
        const uint8_t dims = *reinterpret_cast<const uint8_t *>(read_ptr);
        read_ptr += sizeof(uint8_t);
        std::vector<uint32_t> shape(dims);
        std::memcpy(shape.data(), read_ptr, sizeof(uint32_t) * dims);
        read_ptr += sizeof(uint32_t) * kFlashTensorMaxDims;
        std::vector<uint32_t> stride(dims);
        std::memcpy(stride.data(), read_ptr, sizeof(uint32_t) * dims);
        read_ptr += sizeof(uint32_t) * kFlashTensorMaxDims;
        const uint8_t shift = *reinterpret_cast<const uint8_t *>(read_ptr);
        read_ptr += sizeof(uint8_t);
        // numel
        read_ptr += sizeof(uint32_t);
        const bool quantized = *reinterpret_cast<const bool *>(read_ptr);
        read_ptr += sizeof(bool);

        assert(CheckTensorType<dtype>(tensor_type));

        const dtype *data = reinterpret_cast<const dtype *>(utils::AlignTensorDataPtr(read_ptr));
        return TensorView<dtype>(std::move(shape), std::move(stride), data, shift, quantized);
    }

    template TensorView<int8_t> MapTensor<int8_t>(const uint32_t tensor_id);
    template TensorView<int16_t> MapTensor<int16_t>(const uint32_t tensor_id);
    template TensorView<int32_t> MapTensor<int32_t>(const uint32_t tensor_id);
    template TensorView<float> MapTensor<float>(const uint32_t tensor_id);

    template <typename dtype>
    Tensor<dtype> LoadTensor(const uint32_t tensor_id, const heap::Type type)
    {
        return Tensor<dtype>(MapTensor<dtype>(tensor_id), type);
    }

    template Tensor<int8_t> LoadTensor<int8_t>(const uint32_t tensor_id, const heap::Type type);
    template Tensor<int16_t> LoadTensor<int16_t>(const uint32_t tensor_id, const heap::Type type);
    template Tensor<int32_t> LoadTensor<int32_t>(const uint32_t tensor_id, const heap::Type type);
    template Tensor<float> LoadTensor<float>(const uint32_t tensor_id, const heap::Type type);

    void Summary(void)
    {

        std::println("Flash Summary");
        if (parsing_error_ != FlashError::None)
        {
            std::println("|--Parsing error code: {}", std::to_underlying(parsing_error_));
            std::println("+\n");
            return;
        }
        std::println("|--File size: {}", file_header_.file_size);
        std::println("|--Tensor Count: {}", file_header_.n_tensors);
        std::println("+\n");
    }

} // tlib::flash
