#include "flash_callback.h"

#include <esp_system.h>
#include <spi_flash_mmap.h>

uint64_t model_flash_address_{0};
spi_flash_mmap_handle_t flash_map_meta_data_;
spi_flash_mmap_handle_t flash_map_partition_;
uint64_t mapped_partition_address_{0};
uint64_t mapped_partition_size_{0};
uint8_t *mapped_partition_data_{0};
uint8_t *mapped_model_meta_data_{0};

namespace tlib::flash
{

	void SetModelFlashAddress(uint64_t model_flash_address)
	{
		model_flash_address_ = model_flash_address;
	}

	uint8_t *OnFlashReadRequest(uint64_t partition_flash_address, size_t partition_size)
	{
		// The first partition of the model file is the meta data partition
		const bool is_meta_data_address = partition_flash_address == model_flash_address_;

		if (is_meta_data_address && mapped_model_meta_data_ == 0)
		{
			// Map the meta data partition
			const esp_err_t res = spi_flash_mmap(partition_flash_address, partition_size, SPI_FLASH_MMAP_DATA,
												 (const void **)(&mapped_model_meta_data_), &flash_map_meta_data_);

			if (res != ESP_OK)
			{
				mapped_model_meta_data_ = 0;
				return nullptr;
			}

			return mapped_model_meta_data_;
		}
		else if (partition_flash_address >= mapped_partition_address_ && partition_flash_address <= (mapped_partition_address_ + mapped_partition_size_))
		{
			// partition already mapped
			return mapped_partition_data_;
		}
		else
		{
			// partition not mapped
			if (mapped_partition_address_ != 0)
			{
				// Unmap current partition
				spi_flash_munmap(flash_map_partition_);
			}

			const esp_err_t res = spi_flash_mmap(partition_flash_address, partition_size, SPI_FLASH_MMAP_DATA,
												 (const void **)(&mapped_partition_data_), &flash_map_partition_);

			if (res != ESP_OK)
			{
				mapped_partition_address_ = 0;
				mapped_partition_size_ = 0;
				return nullptr;
			}
			mapped_partition_address_ = partition_flash_address;
			mapped_partition_size_ = partition_size;
			return mapped_partition_data_;
		}
	}

} // tlib::flash