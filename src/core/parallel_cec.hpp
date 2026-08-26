/* VeLoR: Parallel Equivalence Checking Tool
 * Process-level parallel equivalence checking with CNF generation methods
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
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kissat_wrapper.hpp"

namespace velor {

/*! \brief CNF generation method. */
enum class cnf_generation_method {
  MOCKTURTLE = 0,  // Mockturtle original generate_cnf
  VLR = 1          // VLR (Verification Logic Representation) based method
};

/*! \brief Parameters for process parallel CEC. */
struct process_parallel_cec_params {
  uint32_t num_processes{0u};
  uint32_t num_split_vars{0u};
  uint32_t min_fanout{5u};
  std::vector<uint32_t> key_variables;
  cnf_generation_method cnf_method{cnf_generation_method::MOCKTURTLE};
  bool verbose{false};
  uint32_t timeout_seconds{0u};  // 0 = no timeout
  bool print_solver_stats{false};
  bool enable_kissat_optimizations{true};
  bool enable_cube_propagation{true};
  bool aggressive_kissat_preprocess{true};
  std::string kissat_configuration{""};  // "", "default", "sat", "unsat"

  // Learned clauses from functional hashing
  std::vector<std::vector<int>> learned_clauses;
};

/*! \brief Statistics for process parallel CEC. */
struct process_parallel_cec_stats {
  mockturtle::stopwatch<>::duration time_total{};
  mockturtle::stopwatch<>::duration time_cnf{};
  mockturtle::stopwatch<>::duration time_sat{};

  uint32_t num_vars{0u};
  uint32_t num_clauses{0u};
  uint32_t num_processes{0u};
  uint32_t num_killed{0u};
  uint32_t sat_variables{0u};
  uint64_t sat_conflicts{0u};
  uint64_t sat_propagations{0u};
  uint64_t simplified_clauses_removed{0u};
  uint64_t simplified_literals_removed{0u};
  uint64_t trivial_unsat_cubes{0u};

  std::vector<bool> counterexample;

  void report() const {
    std::cout << fmt::format(
        "[i] ===== Process Parallel CEC Statistics =====\n");
    std::cout << fmt::format("[i] Total time    : {:>8.5f} s\n",
                             mockturtle::to_seconds(time_total));
    std::cout << fmt::format("[i] CNF time      : {:>8.5f} s\n",
                             mockturtle::to_seconds(time_cnf));
    std::cout << fmt::format("[i] SAT time      : {:>8.5f} s\n",
                             mockturtle::to_seconds(time_sat));
    std::cout << fmt::format("[i] CNF variables : {}\n", num_vars);
    std::cout << fmt::format("[i] CNF clauses   : {}\n", num_clauses);
    std::cout << fmt::format("[i] SAT variables : {}\n", sat_variables);
    std::cout << fmt::format("[i] SAT conflicts : {}\n", sat_conflicts);
    std::cout << fmt::format("[i] SAT props     : {}\n", sat_propagations);
    std::cout << fmt::format("[i] Processes     : {}\n", num_processes);
    if (simplified_clauses_removed > 0 || simplified_literals_removed > 0) {
      std::cout << fmt::format("[i] Simplify rm cls/lit: {}/{}\n",
                               simplified_clauses_removed,
                               simplified_literals_removed);
    }
    if (trivial_unsat_cubes > 0) {
      std::cout << fmt::format("[i] Trivial UNSAT cubes: {}\n",
                               trivial_unsat_cubes);
    }
    if (num_killed > 0) {
      std::cout << fmt::format("[i] Killed        : {}\n", num_killed);
    }

    if (!counterexample.empty()) {
      std::cout << "[i] Counterexample (first 10 PIs): ";
      for (size_t i = 0; i < std::min(size_t(10), counterexample.size()); ++i) {
        std::cout << (counterexample[i] ? '1' : '0');
      }
      if (counterexample.size() > 10) std::cout << "...";
      std::cout << "\n";
    }
  }
};

namespace detail {

/*! \brief Optimized CNF templates for common gate types. */
struct gate_cnf_template {
  std::string name;
  uint32_t num_inputs;
  std::vector<std::vector<int>>
      clauses;  // Relative clause pattern (0=output, 1-n=inputs)
};

/*! \brief Get optimized CNF template for common gate types. */
inline const gate_cnf_template* get_gate_cnf_template(
    const std::string& gate_type, uint32_t num_inputs) {
  // 2-input gates
  // Note: In clause patterns, 0 represents output variable
  // We need a special encoding: positive output = 100, negative output = -100
  // But for simplicity, we avoid -0 and use explicit mapping in application

  // AND2: o = a ∧ b
  // Tseitin: (¬o ∨ a) ∧ (¬o ∨ b) ∧ (o ∨ ¬a ∨ ¬b)
  static const gate_cnf_template AND2 = {
      "AND2",
      2,
      {
          {-100, 1}, {-100, 2}, {100, -1, -2}  // ¬o∨a, ¬o∨b, o∨¬a∨¬b
      }};

  // OR2: o = a ∨ b
  // Tseitin: (¬o ∨ a ∨ b) ∧ (o ∨ ¬a) ∧ (o ∨ ¬b)
  static const gate_cnf_template OR2 = {
      "OR2",
      2,
      {
          {-100, 1, 2}, {100, -1}, {100, -2}  // ¬o∨a∨b, o∨¬a, o∨¬b
      }};

  // NAND2: o = ¬(a ∧ b)
  // Tseitin: (o ∨ a) ∧ (o ∨ b) ∧ (¬o ∨ ¬a ∨ ¬b)
  static const gate_cnf_template NAND2 = {
      "NAND2",
      2,
      {
          {100, 1}, {100, 2}, {-100, -1, -2}  // o∨a, o∨b, ¬o∨¬a∨¬b
      }};

  // NOR2: o = ¬(a ∨ b)
  // Tseitin: (o ∨ a ∨ b) ∧ (¬o ∨ ¬a) ∧ (¬o ∨ ¬b)
  static const gate_cnf_template NOR2 = {
      "NOR2",
      2,
      {
          {100, 1, 2}, {-100, -1}, {-100, -2}  // o∨a∨b, ¬o∨¬a, ¬o∨¬b
      }};

  // XOR2: o = a ⊕ b
  // Tseitin: (¬o∨¬a∨¬b) ∧ (¬o∨a∨b) ∧ (o∨¬a∨b) ∧ (o∨a∨¬b)
  static const gate_cnf_template XOR2 = {
      "XOR2", 2, {{-100, -1, -2}, {-100, 1, 2}, {100, -1, 2}, {100, 1, -2}}};

  // XNOR2: o = ¬(a ⊕ b) = a ⊙ b
  // Tseitin: (o∨¬a∨¬b) ∧ (o∨a∨b) ∧ (¬o∨¬a∨b) ∧ (¬o∨a∨¬b)
  static const gate_cnf_template XNOR2 = {
      "XNOR2", 2, {{100, -1, -2}, {100, 1, 2}, {-100, -1, 2}, {-100, 1, -2}}};

  // 3-input gates
  // AND3: o = a ∧ b ∧ c
  // Tseitin: (¬o∨a) ∧ (¬o∨b) ∧ (¬o∨c) ∧ (o∨¬a∨¬b∨¬c)
  static const gate_cnf_template AND3 = {
      "AND3", 3, {{-100, 1}, {-100, 2}, {-100, 3}, {100, -1, -2, -3}}};

  // OR3: o = a ∨ b ∨ c
  // Tseitin: (¬o∨a∨b∨c) ∧ (o∨¬a) ∧ (o∨¬b) ∧ (o∨¬c)
  static const gate_cnf_template OR3 = {
      "OR3", 3, {{-100, 1, 2, 3}, {100, -1}, {100, -2}, {100, -3}}};

  // NAND3: o = ¬(a ∧ b ∧ c)
  // Tseitin: (o∨a) ∧ (o∨b) ∧ (o∨c) ∧ (¬o∨¬a∨¬b∨¬c)
  static const gate_cnf_template NAND3 = {
      "NAND3", 3, {{100, 1}, {100, 2}, {100, 3}, {-100, -1, -2, -3}}};

  // NOR3: o = ¬(a ∨ b ∨ c)
  // Tseitin: (o∨a∨b∨c) ∧ (¬o∨¬a) ∧ (¬o∨¬b) ∧ (¬o∨¬c)
  static const gate_cnf_template NOR3 = {
      "NOR3", 3, {{100, 1, 2, 3}, {-100, -1}, {-100, -2}, {-100, -3}}};

  // XOR3: o = a ⊕ b ⊕ c
  static const gate_cnf_template XOR3 = {"XOR3",
                                         3,
                                         {{-100, 1, 2, 3},
                                          {-100, -1, -2, 3},
                                          {-100, -1, 2, -3},
                                          {-100, 1, -2, -3},
                                          {100, -1, 2, 3},
                                          {100, 1, -2, 3},
                                          {100, 1, 2, -3},
                                          {100, -1, -2, -3}}};

  // AOI/OAI gates
  // AOI21: o = ¬(ab + c) = (¬a + ¬b) · ¬c
  // Tseitin: (¬o∨¬a∨¬b) ∧ (¬o∨¬c) ∧ (o∨a∨c) ∧ (o∨b∨c)
  static const gate_cnf_template AOI21 = {
      "AOI21", 3, {{-100, -1, -2}, {-100, -3}, {100, 1, 3}, {100, 2, 3}}};

  // OAI21: o = ¬((a+b)·c) = (¬a·¬b) + ¬c
  // Tseitin: (¬o∨¬a∨¬c) ∧ (¬o∨¬b∨¬c) ∧ (o∨a∨b) ∧ (o∨c)
  static const gate_cnf_template OAI21 = {
      "OAI21", 3, {{-100, -1, -3}, {-100, -2, -3}, {100, 1, 2}, {100, 3}}};

  // AOI12: o = ¬(a + bc) = ¬a · (¬b + ¬c)
  // Tseitin: (¬o∨¬a) ∧ (¬o∨¬b∨¬c) ∧ (o∨a∨b) ∧ (o∨a∨c)
  static const gate_cnf_template AOI12 = {
      "AOI12", 3, {{-100, -1}, {-100, -2, -3}, {100, 1, 2}, {100, 1, 3}}};

  // OAI12: o = ¬(a·(b+c)) = ¬a + (¬b·¬c)
  // Tseitin: (¬o∨¬a∨¬b) ∧ (¬o∨¬a∨¬c) ∧ (o∨a) ∧ (o∨b∨c)
  static const gate_cnf_template OAI12 = {
      "OAI12", 3, {{-100, -1, -2}, {-100, -1, -3}, {100, 1}, {100, 2, 3}}};

  // MAJ3: o = ab + ac + bc (majority)
  // Tseitin: (¬o∨a∨b) ∧ (¬o∨a∨c) ∧ (¬o∨b∨c) ∧ (o∨¬a∨¬b) ∧ (o∨¬a∨¬c) ∧ (o∨¬b∨¬c)
  static const gate_cnf_template MAJ3 = {"MAJ3",
                                         3,
                                         {{-100, 1, 2},
                                          {-100, 1, 3},
                                          {-100, 2, 3},
                                          {100, -1, -2},
                                          {100, -1, -3},
                                          {100, -2, -3}}};

  // Map gate type string to template
  if (num_inputs == 2) {
    if (gate_type == "and2" || gate_type == "AND2") return &AND2;
    if (gate_type == "or2" || gate_type == "OR2") return &OR2;
    if (gate_type == "nand2" || gate_type == "NAND2") return &NAND2;
    if (gate_type == "nor2" || gate_type == "NOR2") return &NOR2;
    if (gate_type == "xor2" || gate_type == "XOR2") return &XOR2;
    if (gate_type == "xnor2" || gate_type == "XNOR2") return &XNOR2;
  } else if (num_inputs == 3) {
    if (gate_type == "and3" || gate_type == "AND3") return &AND3;
    if (gate_type == "or3" || gate_type == "OR3") return &OR3;
    if (gate_type == "nand3" || gate_type == "NAND3") return &NAND3;
    if (gate_type == "nor3" || gate_type == "NOR3") return &NOR3;
    if (gate_type == "xor3" || gate_type == "XOR3") return &XOR3;
    if (gate_type == "aoi21" || gate_type == "AOI21") return &AOI21;
    if (gate_type == "oai21" || gate_type == "OAI21") return &OAI21;
    if (gate_type == "aoi12" || gate_type == "AOI12") return &AOI12;
    if (gate_type == "oai12" || gate_type == "OAI12") return &OAI12;
    if (gate_type == "maj3" || gate_type == "MAJ3") return &MAJ3;
  }

  return nullptr;
}

/*! \brief Shared memory structure for process communication. */
struct shared_result {
  std::atomic<int> status;  // 0=running, 1=SAT, 2=UNSAT
  std::atomic<pid_t> winner_pid;
  uint32_t num_pis;
  bool counterexample[10000];  // Fixed size for simplicity

  void init(uint32_t n_pis) {
    status.store(0);
    winner_pid.store(0);
    num_pis = n_pis;
    std::memset(counterexample, 0, sizeof(counterexample));
  }

  bool try_set_sat(pid_t pid, const std::vector<bool>& cex) {
    int expected = 0;
    if (status.compare_exchange_strong(expected, 1)) {
      winner_pid.store(pid);
      size_t n = std::min(cex.size(), size_t(10000));
      for (size_t i = 0; i < n; ++i) {
        counterexample[i] = cex[i];
      }
      return true;
    }
    return false;
  }

  bool is_sat() const { return status.load() == 1; }
};

/*! \brief CNF data structure. */
struct proc_cnf_data {
  std::vector<std::vector<int>> clauses;
  std::vector<int> output_constraint;
  uint32_t num_vars{0};
  uint32_t num_clauses{0};

  void clear() {
    clauses.clear();
    output_constraint.clear();
    num_vars = 0;
    num_clauses = 0;
  }
};

/*! \brief Simplified CNF instance under assumptions. */
struct simplified_cnf_instance {
  bool conflict{false};
  uint32_t num_vars{0};
  std::vector<std::vector<int>> clauses;
  std::vector<int8_t> fixed_values;  // -1=false, 0=unknown, 1=true
  uint64_t removed_clauses{0};
  uint64_t removed_literals{0};
};

/*! \brief Result of creating one Kissat solver instance. */
struct solver_build_result {
  std::unique_ptr<KissatWrapper> solver;
  bool trivially_unsat{false};
  std::vector<int8_t> fixed_values;
};

/*! \brief Process parallel checker implementation. */
template <class Ntk>
class process_parallel_checker {
 public:
  process_parallel_checker(const Ntk& ntk,
                           const process_parallel_cec_params& ps,
                           process_parallel_cec_stats& st)
      : ntk_(ntk), params_(ps), stats_(st), shared_mem_(nullptr) {}

  ~process_parallel_checker() {
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

    // Step 2: Determine execution strategy
    uint32_t num_procs = params_.num_processes;
    if (num_procs == 0) {
      num_procs = std::thread::hardware_concurrency();
      if (num_procs == 0) num_procs = 1;
    }

    if (num_procs == 1) {
      return solve_sequential();
    }

    // Step 3: Select split variables and generate cubes
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

    if (params_.verbose) {
      std::cout << fmt::format("[i] Split variables: {} → {} processes\n",
                               split_vars.size(), cubes.size());
    }

    // Step 4: Solve in parallel
    return solve_parallel_processes(cubes);
  }

 private:
  const Ntk& ntk_;
  const process_parallel_cec_params& params_;
  process_parallel_cec_stats& stats_;
  proc_cnf_data cnf_;
  std::unordered_map<uint32_t, uint32_t> node_to_var_;
  shared_result* shared_mem_;

  /*! \brief Generate CNF. */
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

  /*! \brief Mockturtle original CNF generation. */
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
      std::cout << fmt::format("[i] Mockturtle CNF: {} vars, {} clauses\n",
                               stats_.num_vars, stats_.num_clauses);
    }

    return true;
  }

  /*! \brief Identify gate type from truth table. */
  std::string identify_gate_type(const kitty::dynamic_truth_table& tt,
                                 uint32_t num_inputs) {
    if (num_inputs > 3) return "UNKNOWN";

    // Get canonical bits representation
    uint64_t bits = 0;
    if (num_inputs <= 6) {
      bits = tt._bits[0];
    } else {
      return "UNKNOWN";
    }

    // 2-input gate identification
    if (num_inputs == 2) {
      switch (bits & 0xF) {
        case 0x8:
          return "AND2";  // 1000
        case 0xE:
          return "OR2";  // 1110
        case 0x7:
          return "NAND2";  // 0111
        case 0x1:
          return "NOR2";  // 0001
        case 0x6:
          return "XOR2";  // 0110
        case 0x9:
          return "XNOR2";  // 1001
        default:
          return "UNKNOWN";
      }
    }

    // 3-input gate identification
    if (num_inputs == 3) {
      switch (bits & 0xFF) {
        case 0x80:
          return "AND3";  // 10000000
        case 0xFE:
          return "OR3";  // 11111110
        case 0x7F:
          return "NAND3";  // 01111111
        case 0x01:
          return "NOR3";  // 00000001
        case 0x96:
          return "XOR3";  // 10010110
        case 0xE8:
          return "MAJ3";  // 11101000 (ab+ac+bc)
        case 0x07:
          return "AOI21";  // 00000111 (!(ab+c))
        case 0x1F:
          return "OAI21";  // 00011111 (!((a+b)c))
        case 0x15:
          return "AOI12";  // 00010101 (!(a+bc))
        case 0x57:
          return "OAI12";  // 01010111 (!(a(b+c)))
        default:
          return "UNKNOWN";
      }
    }

    return "UNKNOWN";
  }

  /*! \brief VLR (Verification Logic Representation) based CNF generation. */
  bool generate_vlr_cnf() {
    cnf_.clear();
    node_to_var_.clear();

    uint32_t clause_count = 0;

    // Step 1: Assign variables to all nodes first
    uint32_t max_var = 0;
    ntk_.foreach_node([&](auto const& n) {
      if (ntk_.is_constant(n)) return;
      uint32_t node_idx = ntk_.node_to_index(n);
      uint32_t var = node_idx + 1;
      node_to_var_[node_idx] = var;
      max_var = std::max(max_var, var);
    });

    // Step 2: Try to use optimized CNF templates for known gate types
    uint32_t template_hits = 0;
    uint32_t template_misses = 0;

    ntk_.foreach_gate([&](auto const& n) {
      uint32_t node_idx = ntk_.node_to_index(n);
      int output_var = static_cast<int>(node_to_var_[node_idx]);

      // Get gate info
      auto tt = ntk_.node_function(n);
      uint32_t num_inputs = tt.num_vars();

      // Collect fanin variables
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

      // Try to identify gate type and use template
      std::string gate_type = identify_gate_type(tt, num_inputs);
      const auto* tmpl = get_gate_cnf_template(gate_type, num_inputs);

      if (tmpl != nullptr && input_vars.size() == num_inputs) {
        // Use optimized CNF template
        template_hits++;

        for (const auto& clause_pattern : tmpl->clauses) {
          std::vector<int> clause;
          for (int lit_pattern : clause_pattern) {
            if (lit_pattern == 100) {
              // Positive output
              clause.push_back(output_var);
            } else if (lit_pattern == -100) {
              // Negative output
              clause.push_back(-output_var);
            } else if (lit_pattern > 0) {
              // Positive input: use input literal as-is
              int input_idx = lit_pattern - 1;
              clause.push_back(input_vars[input_idx]);
            } else {
              // Negative input: negate input literal
              int input_idx = (-lit_pattern) - 1;
              clause.push_back(-input_vars[input_idx]);
            }
          }
          cnf_.clauses.push_back(clause);
          clause_count++;
        }
      } else {
        // Fall back to cnf_characteristic for this gate
        template_misses++;

        // Use kitty::cnf_characteristic (same as mockturtle's generate_cnf)
        const auto cnf_cubes = kitty::cnf_characteristic(tt);

        // Build literal list: inputs followed by output
        std::vector<int> lits = input_vars;
        lits.push_back(output_var);

        for (const auto& cube : cnf_cubes) {
          std::vector<int> clause;
          for (size_t i = 0; i < lits.size(); ++i) {
            if (cube.get_mask(i)) {
              // If bit is 1, use positive literal; if 0, use negative
              int lit = lits[i];
              clause.push_back(cube.get_bit(i) ? lit : -lit);
            }
          }
          cnf_.clauses.push_back(clause);
          clause_count++;
        }
      }
    });

    // Step 3: Add output constraint
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

  /*! \brief Sequential solving. */
  std::optional<bool> solve_sequential() {
    if (params_.verbose) {
      std::cout << "[i] Solving sequentially\n";
    }

    mockturtle::stopwatch sat_time(stats_.time_sat);

    auto build = create_solver({});
    if (build.trivially_unsat) {
      return false;
    }
    if (!build.solver) {
      std::cerr << "[e] Failed to create solver\n";
      return std::nullopt;
    }

    auto result = build.solver->solve();
    if (params_.print_solver_stats) {
      build.solver->print_statistics();
    }
    if (result == KissatWrapper::Result::SATISFIABLE) {
      stats_.counterexample =
          extract_counterexample(*build.solver, build.fixed_values);
      return true;
    } else if (result == KissatWrapper::Result::UNSATISFIABLE) {
      return false;
    }

    return std::nullopt;
  }

  /*! \brief Parallel solving with processes. */
  std::optional<bool> solve_parallel_processes(
      const std::vector<std::vector<int>>& cubes) {
    mockturtle::stopwatch sat_time(stats_.time_sat);

    // Create shared memory
    shared_mem_ = static_cast<shared_result*>(
        mmap(nullptr, sizeof(shared_result), PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0));

    if (shared_mem_ == MAP_FAILED) {
      std::cerr << "[e] Failed to create shared memory\n";
      return std::nullopt;
    }

    shared_mem_->init(ntk_.num_pis());

    if (params_.verbose) {
      std::cout << fmt::format("[i] Launching {} processes\n", cubes.size());
    }

    // Fork child processes
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
        std::exit(3);
      } else {
        // Parent
        child_pids.push_back(pid);
      }
    }

    if (child_pids.empty()) {
      std::cerr << "[e] No child processes created\n";
      return std::nullopt;
    }

    // Monitor children
    size_t num_unsat = 0;
    size_t num_sat = 0;
    std::unordered_set<pid_t> alive_pids(child_pids.begin(), child_pids.end());

    while (!alive_pids.empty()) {
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

        usleep(1000);
      } else {
        break;
      }
    }

    if (num_sat > 0) {
      return true;
    } else if (num_unsat == child_pids.size()) {
      return false;
    }

    return std::nullopt;
  }

  /*! \brief Child worker function. */
  void child_process_worker(const std::vector<int>& assumptions,
                            size_t worker_id) {
    if (shared_mem_->is_sat()) {
      std::exit(2);
    }

    (void)worker_id;

    auto build = create_solver(assumptions);
    if (build.trivially_unsat) {
      std::exit(0);
    }
    if (!build.solver) {
      std::exit(3);
    }

    build.solver->set_terminate_callback(
        [this]() -> bool { return shared_mem_->is_sat(); });

    auto result = build.solver->solve();
    if (params_.print_solver_stats) {
      build.solver->print_statistics();
    }

    if (result == KissatWrapper::Result::SATISFIABLE) {
      auto cex = extract_counterexample(*build.solver, build.fixed_values);
      if (shared_mem_->try_set_sat(getpid(), cex)) {
        std::exit(1);
      } else {
        std::exit(2);
      }
    } else if (result == KissatWrapper::Result::UNSATISFIABLE) {
      std::exit(0);
    } else if (result == KissatWrapper::Result::INTERRUPTED) {
      std::exit(2);
    }

    std::exit(3);
  }

  /*! \brief Configure Kissat options for CEC workload. */
  void configure_solver(KissatWrapper& solver, bool parallel_mode) {
    if (!params_.print_solver_stats) {
      solver.set_option("quiet", 1);
    }

    if (!params_.enable_kissat_optimizations) {
      return;
    }

    if (!params_.kissat_configuration.empty()) {
      solver.set_configuration(params_.kissat_configuration);
    }

    // Enable strong simplification/preprocessing for CEC CNFs.
    solver.set_option("preprocess", 1);
    solver.set_option("simplify", 1);
    solver.set_option("eliminate", 1);
    solver.set_option("substitute", 1);
    solver.set_option("probe", 1);
    solver.set_option("vivify", 1);
    solver.set_option("reduce", 1);
    solver.set_option("congruence", 1);
    solver.set_option("ands", 1);
    solver.set_option("equivalences", 1);
    solver.set_option("ifthenelse", 1);
    solver.set_option("transitive", 1);
    solver.set_option("warmup", 1);
    solver.set_option("fastel", 1);

    if (params_.aggressive_kissat_preprocess) {
      solver.set_option("preprocessrounds", parallel_mode ? 2 : 3);
      solver.set_option("eliminaterounds", parallel_mode ? 2 : 3);
      solver.set_option("substituterounds", 3);
      solver.set_option("proberounds", 3);
      solver.set_option("vivifyeffort", parallel_mode ? 120 : 150);
      solver.set_option("transitiveeffort", 50);
    }

    if (parallel_mode) {
      // Parallel cubes are smaller; bias to stronger simplification early.
      solver.set_option("luckyearly", 1);
      solver.set_option("luckylate", 1);
      solver.set_option("restart", 1);
      solver.set_option("phasesaving", 1);
    }
  }

  /*! \brief Simplify CNF by propagating assumptions before invoking Kissat. */
  simplified_cnf_instance simplify_instance(
      const std::vector<int>& assumptions) const {
    simplified_cnf_instance inst;
    inst.num_vars = cnf_.num_vars;
    inst.fixed_values.assign(cnf_.num_vars + 1, 0);

    std::vector<std::vector<int>> all_clauses;
    all_clauses.reserve(cnf_.clauses.size() + params_.learned_clauses.size() +
                        (cnf_.output_constraint.empty() ? 0u : 1u));

    for (const auto& clause : cnf_.clauses) {
      all_clauses.push_back(clause);
    }
    for (const auto& clause : params_.learned_clauses) {
      all_clauses.push_back(clause);
    }
    if (!cnf_.output_constraint.empty()) {
      all_clauses.push_back({cnf_.output_constraint[0]});
    }

    auto assign_literal = [&](int lit) -> bool {
      uint32_t var = static_cast<uint32_t>(std::abs(lit));
      int8_t value = (lit > 0) ? 1 : -1;
      if (var >= inst.fixed_values.size()) {
        return false;
      }
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

    // Keep assumptions as explicit units to preserve exact sub-problem semantics.
    for (int lit : assumptions) {
      inst.clauses.push_back({lit});
    }

    return inst;
  }

  /*! \brief Create solver with assumptions. */
  solver_build_result create_solver(const std::vector<int>& assumptions) {
    solver_build_result result;

    try {
      auto solver = std::make_unique<KissatWrapper>();
      configure_solver(*solver, !assumptions.empty());
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

        // Add base CNF clauses
        for (const auto& clause : cnf_.clauses) {
          solver->add_clause(clause);
        }

        // Add output constraint
        if (!cnf_.output_constraint.empty()) {
          solver->add_unit_clause(cnf_.output_constraint[0]);
        }

        // Add assumptions as unit clauses
        for (auto lit : assumptions) {
          solver->add_unit_clause(lit);
          uint32_t var = static_cast<uint32_t>(std::abs(lit));
          if (var < result.fixed_values.size()) {
            result.fixed_values[var] = (lit > 0) ? 1 : -1;
          }
        }

        // Add learned clauses
        if (!params_.learned_clauses.empty()) {
          for (const auto& clause : params_.learned_clauses) {
            solver->add_clause(clause);
          }
        }
      }

      result.solver = std::move(solver);
      return result;
    } catch (const std::exception& e) {
      std::cerr << "[e] Solver creation failed: " << e.what() << "\n";
      return result;
    }
  }

  /*! \brief Extract counterexample. */
  std::vector<bool> extract_counterexample(const KissatWrapper& solver,
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
std::optional<bool> process_parallel_equivalence_checking(
    const Ntk& ntk, const process_parallel_cec_params& ps = {},
    process_parallel_cec_stats* pst = nullptr) {
  process_parallel_cec_stats local_stats;
  process_parallel_cec_stats& stats = pst ? *pst : local_stats;

  detail::process_parallel_checker<Ntk> checker(ntk, ps, stats);
  return checker.run();
}

}  // namespace velor
