#include "co_inference_task.h"

#include <print>
#include <algorithm>
#include <cassert>
#include <cstdint>

#include <freertos/FreeRTOS.h>

#include <tlib_linear_shared.h>
#include <tlib_linear_impl.h>

constexpr bool ValidateRequest(const tlib::ops::shared::OperatorExecutionRequest &request)
{
    static constexpr std::array<tlib::ops::shared::OperatorID, 3> kOperatorsWithBias{
        tlib::ops::shared::OperatorID::LINEAR_B_RELU_LSHIFT,
        tlib::ops::shared::OperatorID::LINEAR_B_RELU_RSHIFT,
        tlib::ops::shared::OperatorID::LINEAR_B_DEQ};

    if (request.a == nullptr)
        return false;
    if (request.b == nullptr)
        return false;
    if (request.c == nullptr && std::find(kOperatorsWithBias.begin(), kOperatorsWithBias.end(), request.id) != kOperatorsWithBias.end())
        return false;
    if (request.y == nullptr)
        return false;
    if (request.k == 0)
        return false;
    if (request.n == 0)
        return false;
    if (request.m == 0)
        return false;
    if (request.shift > 14)
        return false;

    return true;
}

void CoInferenceTask::Update(void)
{
    if (!tlib::ops::shared::IsCoWorkerRegistered())
    {
        tlib::ops::shared::RegisterCurrentTask();
    }

    assert(tlib::ops::shared::IsCoWorker() && "Failed to register co worker");

    // The 32-bit notification value is abused here to pass the pointer to an
    // execution request from the main worker to the co worker.
    uint32_t request_ptr{0};
    xTaskNotifyWait(0, 0, &request_ptr, portMAX_DELAY);
    const tlib::ops::shared::OperatorExecutionRequest request{
        *reinterpret_cast<tlib::ops::shared::OperatorExecutionRequest *>(request_ptr)};

    assert(ValidateRequest(request) && "Invalid execution request");

    switch (request.id)
    {
    case tlib::ops::shared::OperatorID::LINEAR_B_RELU_LSHIFT:
        tlib::ops::impl::linear_b_relu_impl_lshift(
            reinterpret_cast<const int8_t *>(request.a),
            reinterpret_cast<const int8_t *>(request.b),
            reinterpret_cast<const int32_t *>(request.c),
            reinterpret_cast<int8_t *>(request.y),
            request.k,
            request.n,
            request.m,
            request.shift);
        break;
    case tlib::ops::shared::OperatorID::LINEAR_B_RELU_RSHIFT:
        tlib::ops::impl::linear_b_relu_impl_rshift(
            reinterpret_cast<const int8_t *>(request.a),
            reinterpret_cast<const int8_t *>(request.b),
            reinterpret_cast<const int32_t *>(request.c),
            reinterpret_cast<int8_t *>(request.y),
            request.k,
            request.n,
            request.m,
            request.shift);
        break;
    case tlib::ops::shared::OperatorID::LINEAR_RELU_LSHIFT:
        tlib::ops::impl::linear_relu_impl_lshift(
            reinterpret_cast<const int8_t *>(request.a),
            reinterpret_cast<const int8_t *>(request.b),
            reinterpret_cast<int8_t *>(request.y),
            request.k,
            request.n,
            request.m,
            request.shift);
        break;
    case tlib::ops::shared::OperatorID::LINEAR_RELU_RSHIFT:
        tlib::ops::impl::linear_relu_impl_rshift(
            reinterpret_cast<const int8_t *>(request.a),
            reinterpret_cast<const int8_t *>(request.b),
            reinterpret_cast<int8_t *>(request.y),
            request.k,
            request.n,
            request.m,
            request.shift);
        break;
    case tlib::ops::shared::OperatorID::LINEAR_DEQ:
        tlib::ops::impl::linear_deq_impl(
            reinterpret_cast<const int8_t *>(request.a),
            reinterpret_cast<const int8_t *>(request.b),
            reinterpret_cast<float *>(request.y),
            request.k,
            request.n,
            request.m,
            request.shift);
        break;
    case tlib::ops::shared::OperatorID::LINEAR_B_DEQ:
        tlib::ops::impl::linear_b_deq_impl(
            reinterpret_cast<const int8_t *>(request.a),
            reinterpret_cast<const int8_t *>(request.b),
            reinterpret_cast<const int32_t *>(request.c),
            reinterpret_cast<float *>(request.y),
            request.k,
            request.n,
            request.m,
            request.shift);
        break;
    case tlib::ops::shared::OperatorID::LINEAR:
        tlib::ops::impl::linear_impl(
            reinterpret_cast<const float *>(request.a),
            reinterpret_cast<const float *>(request.b),
            reinterpret_cast<float *>(request.y),
            request.k,
            request.n,
            request.m);
        break;
    default:
        assert(0 && "Invalid execution request id");
        break;
    }
}