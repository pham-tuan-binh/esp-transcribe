#ifndef RUNNER_H_
#define RUNNER_H_

#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Wrapper around a FreeRTOS task
 */
class Runner
{
public:
    /**
     * Default constructor
     */
    explicit Runner(void) = default;

    /**
     * Delete move and copy constructors/assignments
     */
    Runner(const Runner &) = delete;
    Runner(const Runner &&) = delete;
    Runner &operator=(const Runner &) = delete;
    Runner &&operator=(const Runner &&) = delete;

    /**
     * Creates and starts a new FreeRTOS task with the given parameters, which constantly calls the Update method.
     *
     * @param   task_name       The name of this task
     * @param   task_stack_size The stack size in bytes
     * @param   task_priority   The priority for this task
     * @param   task_core_id    The core on which to run this task
     * @return  The task handle of the created task
     */
    TaskHandle_t Start(const std::string task_name, const configSTACK_DEPTH_TYPE task_stack_size, const UBaseType_t task_priority, const BaseType_t task_core_id);

    /**
     * Task loop, to be implemented by inheriting class
     */
    virtual void Update(void) = 0;

    /**
     * Task entry point. Periodically calls the Update method of the given runner instance
     *
     * @param   instance_ptr    Pointer to a runner instance
     */
    static void Run(void *instance_ptr)
    {
        Runner *instance = static_cast<Runner *>(instance_ptr);

        while (1)
        {
            instance->Update();
        }
    }

private:
    TaskHandle_t task_handle_{nullptr}; /*!< Task handle */
};

#endif