#include "inference_runner.hpp"

#include <ATen/core/ScalarType.h>
#include <ATen/core/TensorBody.h>
#include <c10/core/ScalarType.h>
#include <c10/util/Exception.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "Inference_queue.hpp"
#include "client_utils.hpp"
#include "inference_validator.hpp"
#include "input_generator.hpp"
#include "logger.hpp"
#include "runtime_config.hpp"
#include "server_worker.hpp"
#include "starpu_setup.hpp"
#include "warmup.hpp"

constexpr size_t NUM_PREGENERATED_INPUTS = 10;
constexpr unsigned int NUM_WARMUP_ITERATIONS = 2;

// =============================================================================
// InferenceJob: Encapsulates a single inference task, including input data,
// types, ID, and completion callback
// =============================================================================

InferenceJob::InferenceJob(
    std::vector<torch::Tensor> inputs, std::vector<at::ScalarType> types,
    unsigned int job_identifier,
    std::function<void(std::vector<torch::Tensor>, double)> callback)
    : input_tensors_(std::move(inputs)), input_types_(std::move(types)),
      job_id_(job_identifier), on_complete_(std::move(callback)),
      start_time_(std::chrono::high_resolution_clock::now())
{
}

auto
InferenceJob::make_shutdown_job() -> std::shared_ptr<InferenceJob>
{
  auto job = std::make_shared<InferenceJob>();
  job->is_shutdown_signal_ = true;
  return job;
}

// =============================================================================
// Client Logic: Generates and enqueues inference jobs into the shared queue
// =============================================================================

void
client_worker(
    InferenceQueue& queue, const RuntimeConfig& opts,
    const std::vector<torch::Tensor>& outputs_ref,
    const unsigned int iterations)
{
  auto pregen_inputs =
      client_utils::pre_generate_inputs(opts, NUM_PREGENERATED_INPUTS);
  std::mt19937 rng(std::random_device{}());

  for (unsigned int job_id = 0; job_id < iterations; ++job_id) {
    const auto& inputs = client_utils::pick_random_input(pregen_inputs, rng);
    auto job = client_utils::create_job(inputs, outputs_ref, job_id);

    client_utils::log_job_enqueued(
        opts, job_id, iterations, job->timing_info().enqueued_time);
    queue.push(job);

    std::this_thread::sleep_for(std::chrono::milliseconds(opts.delay_ms));
  }

  queue.shutdown();
}

// =============================================================================
// Model Loading and Cloning to GPU
// =============================================================================

auto
load_model(const std::string& model_path) -> torch::jit::script::Module
{
  try {
    return torch::jit::load(model_path);
  }
  catch (const c10::Error& e) {
    log_error("Failed to load model: " + std::string(e.what()));
    throw;
  }
}

auto
clone_model_to_gpus(
    const torch::jit::script::Module& model_cpu,
    const std::vector<unsigned int>& device_ids)
    -> std::vector<torch::jit::script::Module>
{
  std::vector<torch::jit::script::Module> models_gpu;
  models_gpu.reserve(device_ids.size());

  for (const auto& device_id : device_ids) {
    torch::jit::script::Module model_gpu = model_cpu.clone();
    model_gpu.to(
        torch::Device(torch::kCUDA, static_cast<c10::DeviceIndex>(device_id)));
    models_gpu.emplace_back(std::move(model_gpu));
  }

  return models_gpu;
}

// =============================================================================
// Input Generation and Reference Inference Execution (CPU only)
// =============================================================================

auto
generate_inputs(
    const std::vector<std::vector<int64_t>>& shapes,
    const std::vector<torch::Dtype>& types) -> std::vector<torch::Tensor>
{
  return input_generator::generate_random_inputs(shapes, types);
}

auto
run_reference_inference(
    torch::jit::script::Module& model,
    const std::vector<torch::Tensor>& inputs) -> std::vector<torch::Tensor>
{
  std::vector<torch::Tensor> output_refs;
  const std::vector<torch::IValue> input_ivalues(inputs.begin(), inputs.end());

  const auto output = model.forward(input_ivalues);

  if (output.isTensor()) {
    output_refs.push_back(output.toTensor());
  } else if (output.isTuple()) {
    for (const auto& val : output.toTuple()->elements()) {
      if (val.isTensor()) {
        output_refs.push_back(val.toTensor());
      }
    }
  } else if (output.isTensorList()) {
    output_refs.insert(
        output_refs.end(), output.toTensorList().begin(),
        output.toTensorList().end());
  } else {
    log_error("Unsupported output type from model.");
    throw std::runtime_error("Unsupported model output type");
  }

  return output_refs;
}

// =============================================================================
// Model and Reference Output Loader: returns CPU model, GPU clones, and ref
// outputs
// =============================================================================

auto
load_model_and_reference_output(const RuntimeConfig& opts)
    -> std::tuple<
        torch::jit::script::Module, std::vector<torch::jit::script::Module>,
        std::vector<torch::Tensor>>
{
  try {
    auto model_cpu = load_model(opts.model_path);
    auto models_gpu = opts.use_cuda
                          ? clone_model_to_gpus(model_cpu, opts.device_ids)
                          : std::vector<torch::jit::script::Module>{};
    auto inputs = generate_inputs(opts.input_shapes, opts.input_types);
    auto output_refs = run_reference_inference(model_cpu, inputs);

    return {model_cpu, models_gpu, output_refs};
  }
  catch (const c10::Error& e) {
    log_error(
        "Failed to load model or run reference inference: " +
        std::string(e.what()));
    throw;
  }
}

// =============================================================================
// Warmup Phase: Run small number of inference tasks to warm up StarPU & Torch
// =============================================================================

void
run_warmup(
    const RuntimeConfig& opts, StarPUSetup& starpu,
    torch::jit::script::Module& model_cpu,
    std::vector<torch::jit::script::Module>& models_gpu,
    const std::vector<torch::Tensor>& outputs_ref)
{
  log_info(
      opts.verbosity, "Starting warmup with " +
                          std::to_string(NUM_WARMUP_ITERATIONS) +
                          " iterations per CUDA device...");

  WarmupRunner warmup_runner(opts, starpu, model_cpu, models_gpu, outputs_ref);
  warmup_runner.run(NUM_WARMUP_ITERATIONS);

  log_info(opts.verbosity, "Warmup complete. Proceeding to real inference.\n");
}

// =============================================================================
// Result Processing: Print latency breakdowns and validate results
// =============================================================================

void
process_results(
    const std::vector<InferenceResult>& results,
    torch::jit::script::Module& model_cpu, VerbosityLevel verbosity)
{
  for (const auto& result : results) {
    if (!result.results[0].defined()) {
      log_error("[Client] Job " + std::to_string(result.job_id) + " failed.");
      continue;
    }

    const auto& time_info = result.timing_info;
    using duration_f = std::chrono::duration<double, std::milli>;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "Job " << result.job_id << " done. Latency = " << result.latency_ms
        << " ms | " << "Queue = "
        << duration_f(time_info.dequeued_time - time_info.enqueued_time).count()
        << " ms, " << "Submit = "
        << duration_f(
               time_info.before_starpu_submitted_time - time_info.dequeued_time)
               .count()
        << " ms, " << "Scheduling = "
        << duration_f(
               time_info.codelet_start_time -
               time_info.before_starpu_submitted_time)
               .count()
        << " ms, " << "Codelet = "
        << duration_f(time_info.codelet_end_time - time_info.codelet_start_time)
               .count()
        << " ms, " << "Inference = "
        << duration_f(
               time_info.callback_start_time - time_info.inference_start_time)
               .count()
        << " ms, " << "Callback = "
        << duration_f(
               time_info.callback_end_time - time_info.callback_start_time)
               .count()
        << " ms";

    log_stats(verbosity, oss.str());
    validate_inference_result(result, model_cpu, verbosity);
  }
}

// =============================================================================
// Main Inference Loop: Initializes models, runs warmup, starts client/server,
// waits for all jobs to complete, processes results
// =============================================================================

void
run_inference_loop(const RuntimeConfig& opts, StarPUSetup& starpu)
{
  torch::jit::script::Module model_cpu;
  std::vector<torch::jit::script::Module> models_gpu;
  std::vector<torch::Tensor> outputs_ref;

  try {
    std::tie(model_cpu, models_gpu, outputs_ref) =
        load_model_and_reference_output(opts);
  }
  catch (...) {
    return;
  }

  run_warmup(opts, starpu, model_cpu, models_gpu, outputs_ref);

  InferenceQueue queue;
  std::vector<InferenceResult> results;
  std::mutex results_mutex;

  if (opts.iterations > 0) {
    results.reserve(static_cast<size_t>(opts.iterations));
  }

  std::atomic<unsigned int> completed_jobs = 0;
  std::condition_variable all_done_cv;
  std::mutex all_done_mutex;

  ServerWorker worker(
      &queue, &model_cpu, &models_gpu, &starpu, &opts, &results, &results_mutex,
      &completed_jobs, &all_done_cv);

  std::thread server(&ServerWorker::run, &worker);
  std::thread client(
      [&]() { client_worker(queue, opts, outputs_ref, opts.iterations); });

  client.join();

  {
    std::unique_lock<std::mutex> lock(all_done_mutex);
    all_done_cv.wait(
        lock, [&]() { return completed_jobs.load() >= opts.iterations; });
  }

  server.join();
  cudaDeviceSynchronize();

  process_results(results, model_cpu, opts.verbosity);
}
