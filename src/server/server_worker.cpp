#include "server_worker.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Inference_queue.hpp"
#include "exceptions.hpp"
#include "inference_runner.hpp"
#include "inference_task.hpp"
#include "logger.hpp"
#include "runtime_config.hpp"
#include "starpu_setup.hpp"

// =============================================================================
// Constructor
// =============================================================================

ServerWorker::ServerWorker(
    InferenceQueue* queue, torch::jit::script::Module* model_cpu,
    std::vector<torch::jit::script::Module>* models_gpu, StarPUSetup* starpu,
    const RuntimeConfig* opts, std::vector<InferenceResult>* results,
    std::mutex* results_mutex, std::atomic<unsigned int>* completed_jobs,
    std::condition_variable* all_done_cv)
    : queue_(queue), model_cpu_(model_cpu), models_gpu_(models_gpu),
      starpu_(starpu), opts_(opts), results_(results),
      results_mutex_(results_mutex), completed_jobs_(completed_jobs),
      all_done_cv_(all_done_cv)
{
}


// =============================================================================
// Job Queue Management
// =============================================================================

auto
ServerWorker::wait_for_next_job() -> std::shared_ptr<InferenceJob>
{
  std::shared_ptr<InferenceJob> job;
  queue_->wait_and_pop(job);
  return job;
}

auto
ServerWorker::should_shutdown(const std::shared_ptr<InferenceJob>& job) const
    -> bool
{
  if (job->is_shutdown()) {
    log_info(
        opts_->verbosity,
        "Received shutdown signal. Exiting ServerWorker loop.");
    return true;
  }
  return false;
}

// =============================================================================
// Completion Callback Handling
// =============================================================================

void
ServerWorker::prepare_job_completion_callback(
    const std::shared_ptr<InferenceJob>& job)
{
  const auto job_id = job->get_job_id();
  const auto inputs = job->get_input_tensors();
  auto& executed_on = job->get_executed_on();
  auto& timing_info = job->timing_info();
  auto& device_id = job->get_device_id();
  auto& worker_id = job->get_worker_id();

  job->set_on_complete(
      [this, job_id, inputs, &executed_on, &timing_info, &device_id,
       &worker_id](
          const std::vector<torch::Tensor>& results, double latency_ms) {
        {
          const std::lock_guard<std::mutex> lock(*results_mutex_);
          results_->emplace_back(InferenceResult{
              job_id, inputs, results, latency_ms, executed_on, device_id,
              worker_id, timing_info});
        }

        log_stats(
            opts_->verbosity, "Completed job ID: " + std::to_string(job_id) +
                                  ", latency: " + std::to_string(latency_ms) +
                                  " ms");

        completed_jobs_->fetch_add(1);
        all_done_cv_->notify_one();
      });
}

// =============================================================================
// Error Handling for Failed Jobs
// =============================================================================

void
ServerWorker::handle_job_exception(
    const std::shared_ptr<InferenceJob>& job, const std::exception& exception)
{
  const auto job_id = job->get_job_id();
  log_error(
      "[Exception] Job " + std::to_string(job_id) + ": " + exception.what());

  if (job->has_on_complete()) {
    job->get_on_complete()({}, -1);
  }
}

// =============================================================================
// StarPU Task Submission
// =============================================================================

void
ServerWorker::submit_inference_task(const std::shared_ptr<InferenceJob>& job)
{
  InferenceTask task(starpu_, job, model_cpu_, models_gpu_, opts_);
  task.submit();
}

// =============================================================================
// Main run loop: pull jobs, submit them, handle shutdown and errors
// =============================================================================

void
ServerWorker::run()
{
  log_info(opts_->verbosity, "ServerWorker started.");

  while (true) {
    auto job = wait_for_next_job();
    if (should_shutdown(job)) {
      break;
    }

    const auto job_id = job->get_job_id();
    log_trace(opts_->verbosity, "Dequeued job ID: " + std::to_string(job_id));

    prepare_job_completion_callback(job);

    try {
      job->timing_info().dequeued_time =
          std::chrono::high_resolution_clock::now();

      log_debug(
          opts_->verbosity, "Submitting job ID: " + std::to_string(job_id));

      submit_inference_task(job);
    }
    catch (const std::exception& exception) {
      ServerWorker::handle_job_exception(job, exception);
    }
  }

  log_info(opts_->verbosity, "ServerWorker stopped.");
}
