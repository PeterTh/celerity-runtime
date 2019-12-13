#include "graph_serializer.h"

#include <cassert>

#include "command.h"
#include "command_graph.h"

namespace celerity {
namespace detail {

	void graph_serializer::flush(task_id tid) {
		const auto cmd_range = cdag.task_commands<task_command>(tid);
		const std::vector<task_command*> cmds(cmd_range.begin(), cmd_range.end());
		flush(cmds);
	}

	void graph_serializer::flush(const std::vector<task_command*>& cmds) {
#ifndef NDEBUG
		task_id check_tid = task_id(-1);
#endif

		const auto flush_dependency = [this](abstract_command* dep) {
			std::vector<command_id> dep_deps;
			// Iterate over second level of dependencies. All of these must already have been flushed.
			// TODO: We could probably do some pruning here (e.g. omit tasks we know are already finished)
			for(auto dd : dep->get_dependencies()) {
				assert(dd.node->is_flushed());
				dep_deps.push_back(dd.node->get_cid());
			}
			serialize_and_flush(dep, dep_deps);
		};

		std::vector<horizon_command*> horizon_cmds;
		horizon_cmds.reserve(cmds.size());

		std::vector<std::pair<task_command*, std::vector<command_id>>> cmds_and_deps;
		cmds_and_deps.reserve(cmds.size());
		for(auto cmd : cmds) {
#ifndef NDEBUG
			// Verify that all commands belong to the same task
			assert(check_tid == task_id(-1) || check_tid == cmd->get_tid());
			check_tid = cmd->get_tid();
#endif

			cmds_and_deps.emplace_back();
			auto& cad = *cmds_and_deps.rbegin();
			cad.first = cmd;

			// Iterate over first level of dependencies.
			// These might either be data transfer commands, or task commands from other tasks.
			for(auto d : cmd->get_dependencies()) {
				if(isa<nop_command>(d.node)) continue;
				cad.second.push_back(d.node->get_cid());

				// Sanity check: All dependencies must be on the same node.
				assert(d.node->get_nid() == cmd->get_nid());

				if(isa<task_command>(d.node)) {
					// Task command dependencies must be from a different task and have already been flushed.
					assert(static_cast<task_command*>(d.node)->get_tid() != cmd->get_tid());
					assert(static_cast<task_command*>(d.node)->is_flushed());
					continue;
				}

				// Flush dependency right away
				if(!d.node->is_flushed()) flush_dependency(d.node);

				// Special casing for AWAIT_PUSH commands: Also flush the corresponding PUSH.
				// This is necessary as we would otherwise not reach it when starting from task commands alone
				// (unless there exists an anti-dependency, which is not true in most cases).
				if(isa<await_push_command>(d.node)) {
					const auto pcmd = static_cast<await_push_command*>(d.node)->get_source();
					if(!pcmd->is_flushed()) flush_dependency(pcmd);
				}
			}

			for(auto d : cmd->get_dependents()) {
				assert(isa<horizon_command>(d.node));
				horizon_cmds.emplace_back(static_cast<horizon_command*>(d.node));
			}
		}

		// Finally, flush all the task commands.
		for(auto& cad : cmds_and_deps) {
			serialize_and_flush(cad.first, cad.second);
		}

		for(auto& horizon_cmd : horizon_cmds) {
			flush_dependency(horizon_cmd);
		}
	}

	void graph_serializer::serialize_and_flush(abstract_command* cmd, const std::vector<command_id>& dependencies) const {
		assert(!cmd->is_flushed() && "Command has already been flushed.");

		if(isa<nop_command>(cmd)) return;
		command_pkg pkg{cmd->get_cid(), command_type::NOP, command_data{nop_data{}}};

		if(isa<compute_command>(cmd)) {
			pkg.cmd = command_type::COMPUTE;
			const auto compute = static_cast<compute_command*>(cmd);
			pkg.data = compute_data{compute->get_tid(), compute->get_execution_range()};
		} else if(isa<master_access_command>(cmd)) {
			pkg.cmd = command_type::MASTER_ACCESS;
			const auto ma = static_cast<master_access_command*>(cmd);
			pkg.data = master_access_data{ma->get_tid()};
		} else if(isa<push_command>(cmd)) {
			pkg.cmd = command_type::PUSH;
			const auto push = static_cast<push_command*>(cmd);
			pkg.data = push_data{push->get_bid(), push->get_target(), push->get_range()};
		} else if(isa<await_push_command>(cmd)) {
			pkg.cmd = command_type::AWAIT_PUSH;
			const auto await_push = static_cast<await_push_command*>(cmd);
			pkg.data = await_push_data{await_push->get_source()->get_bid(), await_push->get_source()->get_nid(), await_push->get_source()->get_cid(),
			    await_push->get_source()->get_range()};
		} else if(isa<horizon_command>(cmd)) {
			pkg.cmd = command_type::HORIZON;
		} else {
			assert(false && "Unknown command");
		}

		flush_cb(cmd->get_nid(), pkg, dependencies);
		cmd->mark_as_flushed();
	}

} // namespace detail
} // namespace celerity
