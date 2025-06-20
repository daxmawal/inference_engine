#pragma once

#include "inference_runner.hpp"


// =============================================================================
// Compares the result of an inference job to a reference output generated
// from the same model on the same device. Returns true if close enough.
// =============================================================================
auto validate_inference_result(
    const InferenceResult& result, torch::jit::script::Module& module,
    const VerbosityLevel& verbosity) -> bool;