#ifndef FLASH_MANAGER_H_
#define FLASH_MANAGER_H_

#include <stddef.h>
#include <cstdint>

namespace tlib::flash {

/**
 * Sets the flash address of the model file. Must be called before tlib::flash::Initialize.
 *
 * @param   model_flash_address Flash address of the partition containing the model file
 */
void SetModelFlashAddress(uint64_t model_flash_address);

/**
 * Flash callback to be registered with the tlib flash module. Maps flash memory
 * regions into address space on request. If the requested partition is not mapped
 * already, the currently mapped partition is unmapped first, which invalidates
 * all pointers in that region. An exception is the meta data partition, which
 * is permanently mapped after the first request.
 * 
 * @param   partition_flash_address Flash address of the partition to map
 * @param   partition_size          Size of partition in bytes
 * 
 * @return  Memory address pointing to start of mapped partition, nullptr if mapping failed
 */
uint8_t * OnFlashReadRequest(uint64_t partition_flash_address, size_t partition_size);

} // tlib::flash

#endif