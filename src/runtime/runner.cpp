#include "runner.h"

TaskHandle_t Runner::Start(const std::string task_name, const configSTACK_DEPTH_TYPE task_stack_size, const UBaseType_t task_priority, const BaseType_t task_core_id) {
    xTaskCreatePinnedToCore(Run, task_name.c_str(), task_stack_size, this, task_priority, &task_handle_, task_core_id);
    return task_handle_;
}