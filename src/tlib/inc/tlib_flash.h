#ifndef TLIB_FLASH_H_
#define TLIB_FLASH_H_

#include <cstddef>
#include <cstdint>
#include <functional>

#include <tlib_tensor.h>
#include <tlib_heap.h>

namespace tlib::flash
{

    /**
     * Possible error codes when initialzing flash
     */
    enum class FlashError : uint8_t
    {
        None = 0U,
        MagicInvalid = 1U,
        TensorCountInvalid = 2U,
        FileSizeInvalid = 3U,
        MetaSectorMountFailed = 4U,
        TensorSectorMountFailed = 5U,
        Uninitialized = 6U
    };

    /**
     * Initializes the flash module at the given flash address and registers the given flash callback.
     * The flash module expects a valid model file at the given flash address.
     * The given flash callback will be called by the flash module when data from flash is required.
     */
    FlashError Initialize(uint64_t flash_address, const std::function<const uint8_t *(uint64_t, std::size_t)> &flash_mmap_callback);

    /**
     * Maps a tensor with the given ID from flash. The tensor data is mapped as read-only data.
     *
     * Important: The returned tensor view is only valid as long as no partition remapping is performed.
     *
     * @param   tensor_id   ID of the tensor that should be mapped.
     *
     * @return  The requested tensor as a read-only tensor view.
     */
    template <typename dtype>
    TensorView<dtype> MapTensor(const uint32_t tensor_id);

    /**
     * Loads a tensor with the given ID from flash. The tensor data is copied from flash into the
     * speficied heap.
     *
     * @param   tensor_id   ID of the tensor that should be loaded.
     *
     * @return  The requested tensor.
     */
    template <typename dtype>
    Tensor<dtype> LoadTensor(const uint32_t tensor_id, const heap::Type type = heap::Type::PSRAM);

    /**
     * Prints some information about the flash module.
     */
    void Summary(void);

} // tlib::flash

#endif
