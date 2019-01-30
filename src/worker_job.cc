#include "worker_job.h"

#include <spdlog/fmt/fmt.h>

#include "device_queue.h"
#include "handler.h"
#include "runtime.h"
#include "task_manager.h"

namespace celerity {

// --------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- GENERAL ------------------------------------------------------
// --------------------------------------------------------------------------------------------------------------------

void worker_job::update() {
	assert(running && !done);
	const auto before = bench_clock.now();
	done = execute(pkg, job_logger);

	// TODO: We may want to make benchmarking optional with a macro
	const auto dt = std::chrono::duration_cast<std::chrono::microseconds>(bench_clock.now() - before);
	bench_sum_execution_time += dt;
	bench_sample_count++;
	if(dt < bench_min) bench_min = dt;
	if(dt > bench_max) bench_max = dt;

	if(done) {
		const auto bench_avg = bench_sum_execution_time.count() / bench_sample_count;
		job_logger->info(logger_map({{"event", "STOP"}, {"pollDurationAvg", std::to_string(bench_avg)}, {"pollDurationMin", std::to_string(bench_min.count())},
		    {"pollDurationMax", std::to_string(bench_max.count())}, {"pollSamples", std::to_string(bench_sample_count)}}));
	}
}

void worker_job::start() {
	assert(!running);
	running = true;

	auto job_description = get_description(pkg);
	job_logger->info(logger_map({{"cid", std::to_string(pkg.cid)}, {"event", "START"},
	    {"type", command_string[static_cast<std::underlying_type_t<command>>(job_description.first)]}, {"message", job_description.second}}));
}


// --------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- AWAIT PUSH -----------------------------------------------------
// --------------------------------------------------------------------------------------------------------------------

std::pair<command, std::string> await_push_job::get_description(const command_pkg& pkg) {
	return std::make_pair(command::AWAIT_PUSH,
	    fmt::format("AWAIT PUSH of buffer {} by node {}", static_cast<size_t>(pkg.data.await_push.bid), static_cast<size_t>(pkg.data.await_push.source)));
}

bool await_push_job::execute(const command_pkg& pkg, std::shared_ptr<logger> logger) {
	if(data_handle == nullptr) { data_handle = btm.await_push(pkg); }
	return data_handle->complete;
}


// --------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------- PUSH -------------------------------------------------------
// --------------------------------------------------------------------------------------------------------------------

std::pair<command, std::string> push_job::get_description(const command_pkg& pkg) {
	return std::make_pair(
	    command::PUSH, fmt::format("PUSH buffer {} to node {}", static_cast<size_t>(pkg.data.push.bid), static_cast<size_t>(pkg.data.push.target)));
}

bool push_job::execute(const command_pkg& pkg, std::shared_ptr<logger> logger) {
	if(data_handle == nullptr) {
		logger->info(logger_map({{"event", "Submit buffer to BTM"}}));
		data_handle = btm.push(pkg);
		logger->info(logger_map({{"event", "Buffer submitted to BTM"}}));
	}
	return data_handle->complete;
}

// --------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------ COMPUTE -----------------------------------------------------
// --------------------------------------------------------------------------------------------------------------------

std::pair<command, std::string> compute_job::get_description(const command_pkg& pkg) {
	return std::make_pair(command::COMPUTE, "COMPUTE");
}

// TODO: SYCL should have a event::get_profiling_info call. As of ComputeCpp 0.8.0 this doesn't seem to be supported.
std::chrono::time_point<std::chrono::nanoseconds> get_profiling_info(cl_event e, cl_profiling_info param) {
	cl_ulong value;
	const auto result = clGetEventProfilingInfo(e, param, sizeof(cl_ulong), &value, nullptr);
	assert(result == CL_SUCCESS);
	return std::chrono::time_point<std::chrono::nanoseconds>(std::chrono::nanoseconds(value));
};

bool compute_job::execute(const command_pkg& pkg, std::shared_ptr<logger> logger) {
	// A bit of a hack: We cannot be sure the main thread has reached the task definition yet, so we have to check it here
	if(!task_mngr.has_task(pkg.tid)) {
		if(!did_log_task_wait) {
			logger->info(logger_map({{"event", "Waiting for task definition"}}));
			did_log_task_wait = true;
		}
		return false;
	}

	// There currently (since 0.9.0 and up to and including 1.0.2) exists a bug that causes ComputeCpp to block when
	// querying the execution status of a compute command until it is finished. This is bad for us, as it blocks all other
	// jobs and prevents us from executing multiple compute jobs simultaneously.
	// --> See https://codeplay.atlassian.net/servicedesk/customer/portal/1/CPPB-107 (psalz)
	// The workaround for now is to block within a worker thread.
#define COMPUTECPP_BUG_FIXED 0

	if(!submitted) {
		// Note that we have to set the proper global size so the livepass handler can use the assigned chunk as input for range mappers
		const auto ctsk = std::static_pointer_cast<const detail::compute_task>(task_mngr.get_task(pkg.tid));
		auto& cmd_sr = pkg.data.compute.subrange;
		logger->info(logger_map({{"event", "Execute live-pass, submit kernel to SYCL"}}));
		event = queue.execute(pkg.tid, cmd_sr);
		submitted = true;
		logger->info(logger_map({{"event", "Submitted"}}));

#if !COMPUTECPP_BUG_FIXED
		computecpp_workaround_future = runtime::get_instance().execute_async_pooled([this]() {
			while(true) {
				const auto status = event.get_info<cl::sycl::info::event::command_execution_status>();
				if(status == cl::sycl::info::event_command_status::complete) { return; }
			}
		});
#endif
	}

#if COMPUTECPP_BUG_FIXED
	const auto status = event.get_info<cl::sycl::info::event::command_execution_status>();
	if(status == cl::sycl::info::event_command_status::complete) {
#else
	assert(computecpp_workaround_future.valid());
	if(computecpp_workaround_future.wait_for(std::chrono::microseconds(1)) == std::future_status::ready) {
#endif
		if(queue.is_profiling_enabled()) {
			const auto queued = get_profiling_info(event.get(), CL_PROFILING_COMMAND_QUEUED);
			const auto submit = get_profiling_info(event.get(), CL_PROFILING_COMMAND_SUBMIT);
			const auto start = get_profiling_info(event.get(), CL_PROFILING_COMMAND_START);
			const auto end = get_profiling_info(event.get(), CL_PROFILING_COMMAND_END);

			// FIXME: The timestamps logged here don't match the actual values we just queried. Can we fix that?
			logger->info(logger_map({{"event",
			    fmt::format("Delta time queued -> submit : {}us", std::chrono::duration_cast<std::chrono::microseconds>(submit - queued).count())}}));
			logger->info(logger_map(
			    {{"event", fmt::format("Delta time submit -> start: {}us", std::chrono::duration_cast<std::chrono::microseconds>(start - submit).count())}}));
			logger->info(logger_map(
			    {{"event", fmt::format("Delta time start -> end: {}us", std::chrono::duration_cast<std::chrono::microseconds>(end - start).count())}}));
		}
		return true;
	}
	return false;
}

// --------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- MASTER ACCESS --------------------------------------------------
// --------------------------------------------------------------------------------------------------------------------

std::pair<command, std::string> master_access_job::get_description(const command_pkg& pkg) {
	return std::make_pair(command::MASTER_ACCESS, "MASTER ACCESS");
}

bool master_access_job::execute(const command_pkg& pkg, std::shared_ptr<logger> logger) {
	// In this case we can be sure that the task definition exists, as we're on the master node.
	const auto tsk = dynamic_cast<const detail::master_access_task*>(task_mngr.get_task(pkg.tid).get());
	master_access_livepass_handler handler;
	tsk->get_functor()(handler);
	return true;
}

} // namespace celerity
