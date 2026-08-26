/* VeLoR: Parallel Equivalence Checking Tool
 * Native Glucose-Syrup (thread-level) parallel CEC
 */

#pragma once

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mockturtle/algorithms/cnf.hpp>
#include <mockturtle/networks/klut.hpp>
#include <mockturtle/utils/stopwatch.hpp>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/SolverTypes.h"
#include "parallel/MultiSolvers.h"
#include "parallel_cec.hpp"

namespace velor {
namespace glucosep {

using velor::cnf_generation_method;
using velor::process_parallel_cec_params;
using velor::process_parallel_cec_stats;

namespace detail {

using velor::detail::proc_cnf_data;

template <class Ntk>
class native_parallel_checker_glucose {
 public:
  native_parallel_checker_glucose(const Ntk& ntk,
                                  const process_parallel_cec_params& ps,
                                  process_parallel_cec_stats& st)
      : ntk_(ntk), params_(ps), stats_(st) {}

  std::optional<bool> run() {
    mockturtle::stopwatch total_time(stats_.time_total);

    if (!generate_cnf_data()) {
      std::cerr << "[e] Failed to generate CNF\n";
      return std::nullopt;
    }

    return solve_native_parallel();
  }

 private:
  const Ntk& ntk_;
  const process_parallel_cec_params& params_;
  process_parallel_cec_stats& stats_;
  proc_cnf_data cnf_;
  std::unordered_map<uint32_t, uint32_t> node_to_var_;

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
      std::cout << fmt::format("[i] CNF: {} vars, {} clauses\n", stats_.num_vars,
                               stats_.num_clauses);
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

  std::optional<bool> solve_native_parallel() {
    mockturtle::stopwatch sat_time(stats_.time_sat);

    Glucose::MultiSolvers solver;
    solver.setVerbosity(params_.verbose ? 1 : 0);

    int req_threads = static_cast<int>(params_.num_processes);
    if (req_threads == 1) {
      req_threads = 2;
    }
    solver.set_num_threads(req_threads);

    for (uint32_t i = 0; i < cnf_.num_vars; ++i) {
      solver.newVar();
    }

    auto add_clause = [&](const std::vector<int>& clause) {
      Glucose::vec<Glucose::Lit> lits;
      for (int lit : clause) {
        int var = std::abs(lit);
        lits.push(Glucose::mkLit(var - 1, lit < 0));
      }
      return solver.addClause(lits);
    };

    for (const auto& clause : cnf_.clauses) {
      if (!add_clause(clause)) {
        stats_.num_processes = 1;
        stats_.sat_variables = solver.nVars();
        stats_.sat_conflicts = 0;
        stats_.sat_propagations = 0;
        return false;
      }
    }

    if (!cnf_.output_constraint.empty()) {
      if (!add_clause({cnf_.output_constraint[0]})) {
        stats_.num_processes = 1;
        stats_.sat_variables = solver.nVars();
        stats_.sat_conflicts = 0;
        stats_.sat_propagations = 0;
        return false;
      }
    }

    for (const auto& clause : params_.learned_clauses) {
      if (!add_clause(clause)) {
        stats_.num_processes = 1;
        stats_.sat_variables = solver.nVars();
        stats_.sat_conflicts = 0;
        stats_.sat_propagations = 0;
        return false;
      }
    }

    const auto result = solver.solve();

    stats_.num_processes = static_cast<uint32_t>(std::max(1, solver.num_threads()));
    stats_.sat_variables = static_cast<uint32_t>(solver.nVars());
    stats_.sat_conflicts = solver.total_conflicts();
    stats_.sat_propagations = solver.total_propagations();

    if (result == Glucose::l_True) {
      stats_.counterexample = extract_counterexample(solver);
      return true;
    }
    if (result == Glucose::l_False) {
      return false;
    }
    return std::nullopt;
  }

  std::vector<bool> extract_counterexample(const Glucose::MultiSolvers& solver) {
    std::vector<bool> cex(ntk_.num_pis(), false);

    uint32_t pi_index = 0;
    ntk_.foreach_pi([&](auto const& n) {
      uint32_t node_idx = ntk_.node_to_index(n);
      auto it = node_to_var_.find(node_idx);
      if (it != node_to_var_.end()) {
        const uint32_t var = it->second;
        if (var > 0 && (var - 1) < static_cast<uint32_t>(solver.model.size())) {
          cex[pi_index] = (solver.model[var - 1] == Glucose::l_True);
        }
      }
      ++pi_index;
    });

    return cex;
  }
};

}  // namespace detail

template <class Ntk>
std::optional<bool> native_parallel_equivalence_checking_glucose(
    const Ntk& ntk, const process_parallel_cec_params& ps = {},
    process_parallel_cec_stats* pst = nullptr) {
  process_parallel_cec_stats local_stats;
  process_parallel_cec_stats& stats = pst ? *pst : local_stats;

  detail::native_parallel_checker_glucose<Ntk> checker(ntk, ps, stats);
  return checker.run();
}

}  // namespace glucosep
}  // namespace velor
