/* VeLoR: Parallel Equivalence Checking Tool
 * Process-level parallel equivalence checking with Glucose SAT solver
 */

#pragma once

#include <fmt/format.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mockturtle/algorithms/cnf.hpp>
#include <mockturtle/networks/klut.hpp>
#include <mockturtle/utils/stopwatch.hpp>
#include <mockturtle/views/fanout_view.hpp>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "glucose_wrapper.hpp"
#include "parallel_cec.hpp"  // Import base types

namespace velor {
namespace glucose {

// Import types from the main project namespace
typedef velor::cnf_generation_method cnf_generation_method;
typedef velor::process_parallel_cec_params process_parallel_cec_params;
typedef velor::process_parallel_cec_stats process_parallel_cec_stats;

namespace detail {

using velor::detail::proc_cnf_data;
using velor::detail::simplified_cnf_instance;
using velor::detail::shared_result;

struct solver_build_result_glucose {
  std::unique_ptr<GlucoseWrapper> solver;
  bool trivially_unsat{false};
  std::vector<int8_t> fixed_values;
};

/*! \brief Process parallel checker implementation using Glucose. */
template <class Ntk>
class process_parallel_checker_glucose {
 public:
  process_parallel_checker_glucose(const Ntk& ntk,
                                   const process_parallel_cec_params& ps,
                                   process_parallel_cec_stats& st)
      : ntk_(ntk), params_(ps), stats_(st), shared_mem_(nullptr) {}

  ~process_parallel_checker_glucose() {
    if (shared_mem_ != nullptr) {
      munmap(shared_mem_, sizeof(shared_result));
    }
  }

  std::optional<bool> run() {
    mockturtle::stopwatch total_time(stats_.time_total);

    // Step 1: Generate CNF
    if (!generate_cnf_data()) {
      std::cerr << "[e] Failed to generate CNF\n";
      return std::nullopt;
    }

    // Step 2: Solve (parallel or sequential)
    if (params_.num_processes > 1) {
      return solve_parallel();
    } else {
      return solve_sequential();
    }
  }

 private:
  const Ntk& ntk_;
  const process_parallel_cec_params& params_;
  process_parallel_cec_stats& stats_;
  proc_cnf_data cnf_;
  std::unordered_map<uint32_t, uint32_t> node_to_var_;
  shared_result* shared_mem_;

  /*! \brief Generate CNF using selected method. */
  bool generate_cnf_data() {
    mockturtle::stopwatch cnf_time(stats_.time_cnf);

    switch (params_.cnf_method) {
      case cnf_generation_method::VLR:
        return generate_vlr_cnf();
      case cnf_generation_method::MOCKTURTLE:
      default:
        return generate_mockturtle_cnf();
    }
  }

  /*! \brief Generate CNF using Mockturtle default encoding. */
  bool generate_mockturtle_cnf() {

    cnf_.clear();
    node_to_var_.clear();

    uint32_t clause_count = 0;
    std::unordered_set<uint32_t> used_vars;

    auto output_literals =
        generate_cnf(ntk_, [&](std::vector<uint32_t> const& clause) {
          clause_count++;
          std::vector<int> dimacs_clause;

          for (auto lit : clause) {
            const auto var = (lit / 2) + 1;
            const auto pol = lit % 2;
            const int dimacs_lit =
                pol ? -static_cast<int>(var) : static_cast<int>(var);
            dimacs_clause.push_back(dimacs_lit);

            node_to_var_[lit / 2] = var;
            used_vars.insert(var);
          }

          cnf_.clauses.push_back(std::move(dimacs_clause));
        });

    // Add output constraint
    if (!output_literals.empty()) {
      const auto lit = output_literals[0];
      const auto var = (lit / 2) + 1;
      const auto pol = lit % 2;
      cnf_.output_constraint.push_back(pol ? -static_cast<int>(var)
                                           : static_cast<int>(var));
      used_vars.insert(var);
    }

    cnf_.num_vars = used_vars.empty()
                        ? 0
                        : *std::max_element(used_vars.begin(), used_vars.end());
    cnf_.num_clauses = clause_count;
    stats_.num_vars = cnf_.num_vars;
    stats_.num_clauses = clause_count;

    if (params_.verbose) {
      std::cout << fmt::format("[i] CNF: {} vars, {} clauses\n",
                               stats_.num_vars, stats_.num_clauses);
    }

    return true;
  }

  std::string identify_gate_type(const kitty::dynamic_truth_table& tt,
                                 uint32_t num_inputs) {
    if (num_inputs > 3) return "UNKNOWN";

    uint64_t bits = 0;
    if (num_inputs <= 6) {
      bits = tt._bits[0];
    } else {
      return "UNKNOWN";
    }

    if (num_inputs == 2) {
      switch (bits & 0xF) {
        case 0x8:
          return "AND2";
        case 0xE:
          return "OR2";
        case 0x7:
          return "NAND2";
        case 0x1:
          return "NOR2";
        case 0x6:
          return "XOR2";
        case 0x9:
          return "XNOR2";
        default:
          return "UNKNOWN";
      }
    }

    if (num_inputs == 3) {
      switch (bits & 0xFF) {
        case 0x80:
          return "AND3";
        case 0xFE:
          return "OR3";
        case 0x7F:
          return "NAND3";
        case 0x01:
          return "NOR3";
        case 0x96:
          return "XOR3";
        case 0xE8:
          return "MAJ3";
        case 0x07:
          return "AOI21";
        case 0x1F:
          return "OAI21";
        case 0x15:
          return "AOI12";
        case 0x57:
          return "OAI12";
        default:
          return "UNKNOWN";
      }
    }

    return "UNKNOWN";
  }

  /*! \brief Generate CNF using VLR template-accelerated encoding. */
  bool generate_vlr_cnf() {
    cnf_.clear();
    node_to_var_.clear();

    uint32_t clause_count = 0;

    uint32_t max_var = 0;
    ntk_.foreach_node([&](auto const& n) {
      if (ntk_.is_constant(n)) return;
      uint32_t node_idx = ntk_.node_to_index(n);
      uint32_t var = node_idx + 1;
      node_to_var_[node_idx] = var;
      max_var = std::max(max_var, var);
    });

    uint32_t template_hits = 0;
    uint32_t template_misses = 0;

    ntk_.foreach_gate([&](auto const& n) {
      uint32_t node_idx = ntk_.node_to_index(n);
      int output_var = static_cast<int>(node_to_var_[node_idx]);

      auto tt = ntk_.node_function(n);
      uint32_t num_inputs = tt.num_vars();

      std::vector<int> input_vars;
      ntk_.foreach_fanin(n, [&](auto const& f) {
        uint32_t fanin_idx = ntk_.node_to_index(ntk_.get_node(f));
        bool is_compl = ntk_.is_complemented(f);
        auto it = node_to_var_.find(fanin_idx);
        if (it != node_to_var_.end()) {
          int var = static_cast<int>(it->second);
          input_vars.push_back(is_compl ? -var : var);
        }
      });

      std::string gate_type = identify_gate_type(tt, num_inputs);
      const auto* tmpl =
          velor::detail::get_gate_cnf_template(gate_type, num_inputs);

      if (tmpl != nullptr && input_vars.size() == num_inputs) {
        template_hits++;

        for (const auto& clause_pattern : tmpl->clauses) {
          std::vector<int> clause;
          for (int lit_pattern : clause_pattern) {
            if (lit_pattern == 100) {
              clause.push_back(output_var);
            } else if (lit_pattern == -100) {
              clause.push_back(-output_var);
            } else if (lit_pattern > 0) {
              int input_idx = lit_pattern - 1;
              clause.push_back(input_vars[input_idx]);
            } else {
              int input_idx = (-lit_pattern) - 1;
              clause.push_back(-input_vars[input_idx]);
            }
          }
          cnf_.clauses.push_back(clause);
          clause_count++;
        }
      } else {
        template_misses++;

        const auto cnf_cubes = kitty::cnf_characteristic(tt);

        std::vector<int> lits = input_vars;
        lits.push_back(output_var);

        for (const auto& cube : cnf_cubes) {
          std::vector<int> clause;
          for (size_t i = 0; i < lits.size(); ++i) {
            if (cube.get_mask(i)) {
              int lit = lits[i];
              clause.push_back(cube.get_bit(i) ? lit : -lit);
            }
          }
          cnf_.clauses.push_back(clause);
          clause_count++;
        }
      }
    });

    ntk_.foreach_po([&](auto const& f) {
      uint32_t node_idx = ntk_.node_to_index(ntk_.get_node(f));
      bool is_compl = ntk_.is_complemented(f);
      auto it = node_to_var_.find(node_idx);
      if (it != node_to_var_.end()) {
        int var = static_cast<int>(it->second);
        cnf_.output_constraint.push_back(is_compl ? -var : var);
      }
    });

    cnf_.num_vars = max_var;
    cnf_.num_clauses = clause_count;
    stats_.num_vars = cnf_.num_vars;
    stats_.num_clauses = clause_count;

    if (params_.verbose) {
      std::cout << fmt::format(
          "[i] Standard CNF with templates: {} vars, {} clauses\n",
          stats_.num_vars, stats_.num_clauses);
      std::cout << fmt::format(
          "[i] Template hits: {}, misses: {} ({:.1f}% coverage)\n",
          template_hits, template_misses,
          100.0 * template_hits /
              std::max(1u, template_hits + template_misses));
    }

    return true;
  }

  /*! \brief Sequential solving with Glucose. */
  std::optional<bool> solve_sequential() {
    if (params_.verbose) {
      std::cout << "[i] Solving with Glucose SAT solver\n";
    }

    mockturtle::stopwatch sat_time(stats_.time_sat);

    auto build = create_solver(std::vector<int>{});
    if (build.trivially_unsat) {
      return false;
    }
    if (!build.solver) {
      std::cerr << "[e] Failed to create Glucose solver\n";
      return std::nullopt;
    }

    auto result = build.solver->solve();
    stats_.sat_variables = build.solver->get_num_variables();
    stats_.sat_conflicts = build.solver->get_num_conflicts();
    stats_.sat_propagations = build.solver->get_num_propagations();

    if (result == GlucoseWrapper::Result::SATISFIABLE) {
      stats_.counterexample =
          extract_counterexample(*build.solver, build.fixed_values);
      return true;
    } else if (result == GlucoseWrapper::Result::UNSATISFIABLE) {
      return false;
    }

    return std::nullopt;
  }

  /*! \brief Parallel solving with Glucose. */
  std::optional<bool> solve_parallel() {
    if (params_.verbose) {
      std::cout << fmt::format(
          "[i] Solving with Glucose (parallel/cube partitioning)\n");
    }

    // Use RAII stopwatch for automatic timing (same as satmorph/Kissat)
    mockturtle::stopwatch sat_time(stats_.time_sat);

    // Determine number of processes
    uint32_t num_procs = params_.num_processes;
    if (num_procs == 0) {
      num_procs = std::thread::hardware_concurrency();
      if (num_procs == 0) num_procs = 1;
    }

    // Determine number of split variables
    uint32_t num_split_vars = params_.num_split_vars;
    if (num_split_vars == 0) {
      num_split_vars = static_cast<uint32_t>(std::ceil(std::log2(num_procs)));
    }

    auto split_vars = select_split_variables(num_split_vars);
    if (split_vars.empty()) {
      if (params_.verbose) {
        std::cout << "[i] No split variables, solving sequentially\n";
      }
      return solve_sequential();
    }

    auto cubes = generate_cubes(split_vars);
    stats_.num_processes = cubes.size();

    // Create shared memory for result
    shared_mem_ = static_cast<shared_result*>(
        mmap(NULL, sizeof(shared_result), PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (shared_mem_ == MAP_FAILED) {
      std::cerr << "[e] Failed to create shared memory\n";
      return std::nullopt;
    }
    shared_mem_->init(ntk_.num_pis());

    if (params_.verbose) {
      std::cout << fmt::format("[i] Launching {} processes\n", cubes.size());
    }

    // Fork worker processes
    std::vector<pid_t> child_pids;
    child_pids.reserve(cubes.size());

    for (size_t i = 0; i < cubes.size(); ++i) {
      pid_t pid = fork();
      if (pid < 0) {
        std::cerr << "[e] Fork failed\n";
        break;
      } else if (pid == 0) {
        // Child process
        child_process_worker(cubes[i], i);
        std::exit(3);  // Should not reach
      } else {
        // Parent
        child_pids.push_back(pid);
      }
    }

    if (child_pids.empty()) {
      std::cerr << "[e] No child processes created\n";
      return std::nullopt;
    }

    // Monitor children - SAT: kill all and return immediately
    //                     UNSAT: wait for all to complete
    size_t num_unsat = 0;
    size_t num_sat = 0;
    std::unordered_set<pid_t> alive_pids(child_pids.begin(), child_pids.end());

    while (!alive_pids.empty()) {
      // Wait for any child (non-blocking)
      int status;
      pid_t pid = waitpid(-1, &status, WNOHANG);

      if (pid > 0) {
        alive_pids.erase(pid);

        if (WIFEXITED(status)) {
          int exit_code = WEXITSTATUS(status);
          if (exit_code == 0) {
            num_unsat++;
          } else if (exit_code == 1) {
            num_sat++;

            // SAT found!
            std::vector<bool> cex(ntk_.num_pis());
            for (size_t i = 0; i < ntk_.num_pis() && i < 10000; ++i) {
              cex[i] = shared_mem_->counterexample[i];
            }
            stats_.counterexample = cex;

            // Kill remaining processes
            if (!alive_pids.empty()) {
              stats_.num_killed = alive_pids.size();
              for (auto cpid : alive_pids) {
                kill(cpid, SIGKILL);
              }

              while (!alive_pids.empty()) {
                pid_t killed_pid = waitpid(-1, nullptr, 0);
                if (killed_pid > 0) {
                  alive_pids.erase(killed_pid);
                } else {
                  break;
                }
              }
            }

            return true;
          }
        }
      } else if (pid == 0) {
        // No child ready, check shared memory for early SAT detection
        if (shared_mem_->is_sat()) {
          std::vector<bool> cex(ntk_.num_pis());
          for (size_t i = 0; i < ntk_.num_pis() && i < 10000; ++i) {
            cex[i] = shared_mem_->counterexample[i];
          }
          stats_.counterexample = cex;

          pid_t winner = shared_mem_->winner_pid.load();
          size_t num_to_kill = alive_pids.size();
          if (winner > 0 && alive_pids.count(winner)) {
            num_to_kill--;
          }
          stats_.num_killed = num_to_kill;

          for (auto cpid : alive_pids) {
            kill(cpid, SIGKILL);
          }

          while (!alive_pids.empty()) {
            pid_t killed_pid = waitpid(-1, nullptr, 0);
            if (killed_pid > 0) {
              alive_pids.erase(killed_pid);
            } else {
              break;
            }
          }

          return true;
        }

        // No child ready, sleep briefly
        usleep(1000);  // 1ms
      } else {
        // Error
        break;
      }
    }

    // All processes completed
    if (num_sat > 0) {
      return true;  // SAT (shouldn't reach here normally)
    } else if (num_unsat == child_pids.size()) {
      return false;  // All UNSAT
    }

    return std::nullopt;  // UNKNOWN
  }

  /*! \brief Child worker function for a given cube (assumptions). */
  void child_process_worker(const std::vector<int>& assumptions,
                            size_t worker_id) {
    // Check if already solved before starting
    if (shared_mem_->is_sat()) {
      std::exit(2);  // Another process found SAT
    }

    auto build = create_solver(assumptions);
    if (build.trivially_unsat) {
      std::exit(0);
    }
    if (!build.solver) {
      std::exit(3);  // Error
    }

    // Set different random seed for each process to diversify search
    build.solver->set_random_seed(static_cast<double>(worker_id + 1));

    // Set up termination callback to check shared memory periodically
    // This allows early exit when another process finds SAT
    build.solver->set_terminate_callback(
        [this]() -> bool { return shared_mem_->is_sat(); });

    auto result = build.solver->solve();

    if (result == GlucoseWrapper::Result::SATISFIABLE) {
      // Try to set SAT result
      auto cex = extract_counterexample(*build.solver, build.fixed_values);
      if (shared_mem_->try_set_sat(getpid(), cex)) {
        std::exit(1);  // SAT (winner)
      } else {
        std::exit(2);  // SAT but lost race
      }
    } else if (result == GlucoseWrapper::Result::UNSATISFIABLE) {
      std::exit(0);  // UNSAT
    } else if (result == GlucoseWrapper::Result::INTERRUPTED) {
      // Interrupted due to another process finding SAT
      std::exit(2);  // Terminated early
    }

    std::exit(3);  // UNKNOWN
  }

  /*! \brief Wait for any child process to finish. */
  std::optional<bool> wait_for_result(const std::vector<pid_t>& pids) {
    while (true) {
      int status = shared_mem_->status.load(std::memory_order_acquire);
      if (status == 1) {
        return true;  // SAT
      } else if (status == 2) {
        return false;  // UNSAT
      }
      usleep(1000);  // Sleep 1ms
    }
  }

  /*! \brief Cleanup child processes. */
  void cleanup_processes(const std::vector<pid_t>& pids) {
    // Kill all child processes
    for (pid_t pid : pids) {
      kill(pid, SIGKILL);
    }

    // Wait for all children to terminate
    for (size_t i = 0; i < pids.size(); ++i) {
      wait(nullptr);
    }
  }

  simplified_cnf_instance simplify_instance(
      const std::vector<int>& assumptions) const {
    simplified_cnf_instance inst;
    inst.num_vars = cnf_.num_vars;
    inst.fixed_values.assign(cnf_.num_vars + 1, 0);

    std::vector<std::vector<int>> all_clauses;
    all_clauses.reserve(cnf_.clauses.size() + params_.learned_clauses.size() +
                        (cnf_.output_constraint.empty() ? 0u : 1u));

    for (const auto& clause : cnf_.clauses) all_clauses.push_back(clause);
    for (const auto& clause : params_.learned_clauses)
      all_clauses.push_back(clause);
    if (!cnf_.output_constraint.empty()) {
      all_clauses.push_back({cnf_.output_constraint[0]});
    }

    auto assign_literal = [&](int lit) -> bool {
      uint32_t var = static_cast<uint32_t>(std::abs(lit));
      int8_t value = (lit > 0) ? 1 : -1;
      if (var >= inst.fixed_values.size()) return false;
      if (inst.fixed_values[var] == 0) {
        inst.fixed_values[var] = value;
        return true;
      }
      return inst.fixed_values[var] == value;
    };

    for (int lit : assumptions) {
      if (!assign_literal(lit)) {
        inst.conflict = true;
        return inst;
      }
    }

    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto& clause : all_clauses) {
        bool satisfied = false;
        int unassigned_count = 0;
        int last_unassigned = 0;

        for (int lit : clause) {
          uint32_t var = static_cast<uint32_t>(std::abs(lit));
          if (var >= inst.fixed_values.size()) continue;
          int8_t val = inst.fixed_values[var];
          if (val == 0) {
            ++unassigned_count;
            last_unassigned = lit;
          } else if ((val > 0 && lit > 0) || (val < 0 && lit < 0)) {
            satisfied = true;
            break;
          }
        }

        if (satisfied) continue;
        if (unassigned_count == 0) {
          inst.conflict = true;
          return inst;
        }
        if (unassigned_count == 1) {
          if (!assign_literal(last_unassigned)) {
            inst.conflict = true;
            return inst;
          }
          changed = true;
        }
      }
    }

    inst.clauses.reserve(all_clauses.size() + assumptions.size());
    for (const auto& clause : all_clauses) {
      bool satisfied = false;
      std::vector<int> reduced;
      reduced.reserve(clause.size());

      for (int lit : clause) {
        uint32_t var = static_cast<uint32_t>(std::abs(lit));
        if (var >= inst.fixed_values.size()) {
          reduced.push_back(lit);
          continue;
        }
        int8_t val = inst.fixed_values[var];
        if (val == 0) {
          reduced.push_back(lit);
        } else if ((val > 0 && lit > 0) || (val < 0 && lit < 0)) {
          satisfied = true;
          break;
        }
      }

      if (satisfied) {
        inst.removed_clauses++;
        inst.removed_literals += clause.size();
        continue;
      }
      if (reduced.empty()) {
        inst.conflict = true;
        return inst;
      }
      if (reduced.size() < clause.size()) {
        inst.removed_literals += clause.size() - reduced.size();
      }
      inst.clauses.push_back(std::move(reduced));
    }

    for (int lit : assumptions) {
      inst.clauses.push_back({lit});
    }
    return inst;
  }

  /*! \brief Create Glucose solver with optional split assumptions. */
  solver_build_result_glucose create_solver(const std::vector<int>& assumptions) {
    solver_build_result_glucose result;
    try {
      auto solver = std::make_unique<GlucoseWrapper>();

      // NOTE:
      // Enabling Glucose internal progress printing (`verbosity=1`) can trigger
      // floating-point exceptions on some instances/toolchain combinations
      // (observed as SIGFPE while printing "Search Statistics").
      // Keep solver verbosity disabled and rely on our own stats reporting.
      solver->set_verbosity(0);

      solver->reserve(cnf_.num_vars);

      if (params_.enable_cube_propagation && !assumptions.empty()) {
        auto inst = simplify_instance(assumptions);
        result.fixed_values = std::move(inst.fixed_values);

        if (inst.conflict) {
          result.trivially_unsat = true;
          stats_.trivial_unsat_cubes++;
          return result;
        }

        stats_.simplified_clauses_removed += inst.removed_clauses;
        stats_.simplified_literals_removed += inst.removed_literals;

        for (const auto& clause : inst.clauses) {
          solver->add_clause(clause);
        }
      } else {
        result.fixed_values.assign(cnf_.num_vars + 1, 0);

        for (const auto& clause : cnf_.clauses) {
          solver->add_clause(clause);
        }

        if (!cnf_.output_constraint.empty()) {
          solver->add_unit_clause(cnf_.output_constraint[0]);
        }

        for (auto lit : assumptions) {
          solver->add_unit_clause(lit);
          uint32_t var = static_cast<uint32_t>(std::abs(lit));
          if (var < result.fixed_values.size()) {
            result.fixed_values[var] = (lit > 0) ? 1 : -1;
          }
        }

        if (!params_.learned_clauses.empty()) {
          for (const auto& clause : params_.learned_clauses) {
            solver->add_clause(clause);
          }
        }
      }

      result.solver = std::move(solver);
      return result;
    } catch (const std::exception& e) {
      std::cerr << "[e] Glucose solver creation failed: " << e.what() << "\n";
      return result;
    }
  }

  /*! \brief Select split variables. */
  std::vector<uint32_t> select_split_variables(uint32_t count) {
    if (!params_.key_variables.empty()) {
      std::vector<uint32_t> cnf_vars;
      for (auto node_idx : params_.key_variables) {
        auto it = node_to_var_.find(node_idx);
        if (it != node_to_var_.end()) {
          cnf_vars.push_back(it->second);
        }
      }
      if (cnf_vars.size() > count) {
        cnf_vars.resize(count);
      }
      return cnf_vars;
    }

    auto fanout_ntk = mockturtle::fanout_view(ntk_);
    std::vector<std::pair<uint32_t, uint32_t>> candidates;

    ntk_.foreach_pi([&](auto const& n) {
      uint32_t node_idx = ntk_.node_to_index(n);
      uint32_t fanout = fanout_ntk.fanout_size(n);
      if (fanout >= params_.min_fanout) {
        auto it = node_to_var_.find(node_idx);
        if (it != node_to_var_.end()) {
          candidates.emplace_back(it->second, fanout);
        }
      }
    });

    if (candidates.empty()) {
      ntk_.foreach_pi([&](auto const& n) {
        uint32_t node_idx = ntk_.node_to_index(n);
        auto it = node_to_var_.find(node_idx);
        if (it != node_to_var_.end()) {
          uint32_t fanout = fanout_ntk.fanout_size(n);
          candidates.emplace_back(it->second, fanout);
        }
      });
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<uint32_t> selected;
    size_t k = std::min(static_cast<size_t>(count), candidates.size());
    for (size_t i = 0; i < k; ++i) {
      selected.push_back(candidates[i].first);
    }

    return selected;
  }

  /*! \brief Generate all cubes. */
  std::vector<std::vector<int>> generate_cubes(
      const std::vector<uint32_t>& split_vars) {
    std::vector<std::vector<int>> cubes;
    size_t num_cubes = 1ULL << split_vars.size();

    for (size_t i = 0; i < num_cubes; ++i) {
      std::vector<int> cube;
      for (size_t j = 0; j < split_vars.size(); ++j) {
        bool value = (i >> j) & 1;
        cube.push_back(value ? static_cast<int>(split_vars[j])
                             : -static_cast<int>(split_vars[j]));
      }
      cubes.push_back(std::move(cube));
    }

    return cubes;
  }

  /*! \brief Extract counterexample. */
  std::vector<bool> extract_counterexample(const GlucoseWrapper& solver,
                                           const std::vector<int8_t>& fixed) {
    std::vector<bool> cex(ntk_.num_pis());

    uint32_t pi_index = 0;
    ntk_.foreach_pi([&](auto const& n) {
      uint32_t node_idx = ntk_.node_to_index(n);
      auto it = node_to_var_.find(node_idx);
      if (it != node_to_var_.end()) {
        uint32_t var = it->second;
        if (var < fixed.size() && fixed[var] != 0) {
          cex[pi_index] = fixed[var] > 0;
        } else {
          auto value = solver.get_value(var);
          cex[pi_index] = value.value_or(false);
        }
      }
      pi_index++;
    });

    return cex;
  }
};

}  // namespace detail

template <class Ntk>
std::optional<bool> process_parallel_equivalence_checking_glucose(
    const Ntk& ntk, const process_parallel_cec_params& ps = {},
    process_parallel_cec_stats* pst = nullptr) {
  process_parallel_cec_stats local_stats;
  process_parallel_cec_stats& stats = pst ? *pst : local_stats;

  detail::process_parallel_checker_glucose<Ntk> checker(ntk, ps, stats);
  return checker.run();
}

}  // namespace glucose
}  // namespace velor
