#ifndef CO_INFERENCE_TASK_H_
#define CO_INFERENCE_TASK_H_

#include <runner.h>

/**
 * Additional inference worker task to speed up inference through multi-processing.
 */
class CoInferenceTask : public Runner
{
public:
    /**
     * Task loop. Waits for a task notification containing an execution request
     * and afterwards executes the request.
     */
    void Update(void) override;
};

#endif