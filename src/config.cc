#include "config.h"

#include <cstdlib>
#include <sstream>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <mpi.h>

#include "logger.h"
#include "workaround.h"

std::pair<bool, std::string> get_env(const char* key) {
	bool exists = false;
	std::string result;
#ifdef _MSC_VER
	char* buf;
	_dupenv_s(&buf, nullptr, key);
	if(buf != nullptr) {
		exists = true;
		result = buf;
		delete buf;
	}
#else
	auto value = std::getenv(key);
	if(value != nullptr) {
		exists = true;
		result = value;
	}
#endif
	return {exists, result};
}

std::pair<bool, size_t> parse_uint(const char* str) {
	errno = 0;
	const auto value = std::strtoul(str, nullptr, 10);
	if(errno == 0) { return {true, value}; }
	return {false, 0};
}

namespace celerity {
namespace detail {
	config::config(int* argc, char** argv[], logger& logger) {
		// TODO: At some point we might want to parse arguments from argv as well

		// ------------------------------- CELERITY_LOG_LEVEL ---------------------------------

		{
#ifdef NDEBUG
			log_lvl = log_level::info;
#else
			log_lvl = log_level::trace;
#endif
			const std::vector<std::pair<log_level, std::string>> possible_values = {
			    {log_level::trace, "trace"},
			    {log_level::debug, "debug"},
			    {log_level::info, "info"},
			    {log_level::warn, "warn"},
			    {log_level::err, "err"},
			    {log_level::critical, "critical"},
			    {log_level::off, "off"},
			};

			const auto result = get_env("CELERITY_LOG_LEVEL");
			if(result.first) {
				bool valid = false;
				for(auto& pv : possible_values) {
					if(result.second == pv.second) {
						log_lvl = pv.first;
						valid = true;
						break;
					}
				}
				if(!valid) {
					std::ostringstream oss;
					oss << "Invalid value \"" << result.second << "\" provided for CELERITY_LOG_LEVEL. ";
					oss << "Possible values are: ";
					for(size_t i = 0; i < possible_values.size(); ++i) {
						oss << possible_values[i].second << (i < possible_values.size() - 1 ? ", " : ".");
					}
					logger.warn(oss.str());
				}
			}
			logger.set_level(log_lvl);
		}

		// --------------------------------- CELERITY_DEVICES ---------------------------------

		{
			const auto result = get_env("CELERITY_DEVICES");
			if(result.first) {
#ifdef OPEN_MPI
#define SPLIT_TYPE OMPI_COMM_TYPE_HOST
#else
#define SPLIT_TYPE MPI_COMM_TYPE_SHARED
#endif
				// Determine our per-node rank by finding all world-ranks that can use a shared-memory transport
				// (If running on OpenMPI, use the per-host split instead)
				// This is a collective call, so make sure we do this before checking the env var
				// TODO: Assert that shared memory is available (i.e. not explicitly disabled)
				MPI_Comm node_comm;
				MPI_Comm_split_type(MPI_COMM_WORLD, SPLIT_TYPE, 0, MPI_INFO_NULL, &node_comm);
				int node_rank = 0;
				MPI_Comm_rank(node_comm, &node_rank);

				std::vector<std::string> values;
				boost::split(values, result.second, [](char c) { return c == ' '; });

				if(node_rank > static_cast<long>(values.size()) - 2) {
					throw std::runtime_error(
					    fmt::format("Process has local rank {}, but CELERITY_DEVICES only includes {} device(s)", node_rank, values.size() - 1));
				}

				int node_size = 0;
				MPI_Comm_size(node_comm, &node_size);
				if(static_cast<long>(values.size()) - 1 > node_size) {
					logger.warn(
					    "CELERITY_DEVICES contains {} device indices, but only {} worker processes were spawned on this node", values.size() - 1, node_size);
				}

				const auto pid_parsed = parse_uint(values[0].c_str());
				const auto did_parsed = parse_uint(values[node_rank + 1].c_str());
				if(!pid_parsed.first || !did_parsed.first) {
					logger.warn("CELERITY_DEVICES contains invalid value(s) - will be ignored");
				} else {
					device_cfg = device_config{pid_parsed.second, did_parsed.second};
				}
			}
		}

		// ------------------------------- CELERITY_PROFILE_OCL -------------------------------

		{
			const auto result = get_env("CELERITY_PROFILE_OCL");
			if(result.first) {
				enable_device_profiling = result.second == "1";
#if WORKAROUND(HIPSYCL, 0)
				if(*enable_device_profiling) {
					logger.warn("Device profiling is currently not supported on hipSYCL");
					enable_device_profiling = false;
				}
#endif
			}
		}

		// -------------------------------- CELERITY_FORCE_WG ---------------------------------

		{
			const auto result = get_env("CELERITY_FORCE_WG");
			if(result.first) {
				const auto parsed = parse_uint(result.second.c_str());
				if(parsed.first) { forced_work_group_size = parsed.second; }
			}
		}
	}

} // namespace detail
} // namespace celerity
