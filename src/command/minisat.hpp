#pragma once

#include <fmt/format.h>

#include <alice/alice.hpp>
#include <mockturtle/mockturtle.hpp>

#include "../core/parallel_cec_minisat.hpp"

namespace alice {

class minisat_command : public command {
 public:
  minisat_command(const environment::ptr& env)
      : command(env,
                "MiniSat-based equivalence checking (AIG/KLUT/XMG miters)") {
    add_option(
      "--cnf_method,-C", cnf_method_str,
      "CNF generation method: 'mockturtle' (default), 'vlr' (VLR-based)",
      "mockturtle");
    add_flag("--aig,-a", "Force verification on AIG network");
    add_flag("--xmg,-x", "Force verification on XMG network");
    add_flag("--lut,-l", "Force verification on KLUT network");
    add_option("--procs,-p", procs_,
               "Number of parallel processes (default: 1)", 1);
    add_flag("--no_cube_prop",
         "Disable assumption propagation and CNF simplification per cube");
    add_flag("--solver_stats,-t", "Print detailed SAT solver statistics");
    add_flag("--verbose,-v", "Verbose output");
    add_flag("--show_cex,-c", "Show counterexample if not equivalent");
  }

 protected:
  void execute() override {
    auto start = std::chrono::high_resolution_clock::now();

    bool result = false;

    auto& kluts = store<mockturtle::klut_network>();
    auto& xmgs = store<mockturtle::xmg_network>();
    auto& aigs = store<mockturtle::aig_network>();

    bool force_aig = is_set("aig");
    bool force_xmg = is_set("xmg");
    bool force_lut = is_set("lut");

    int flag_count =
        (force_aig ? 1 : 0) + (force_xmg ? 1 : 0) + (force_lut ? 1 : 0);

    if (flag_count > 1) {
      env->out()
          << "[e] Only one network type flag can be specified at a time\n";
      env->out() << "[i] Use --aig, --xmg, or --lut (not multiple)\n";
      return;
    }

    if (force_lut) {
      if (kluts.empty()) {
        env->out() << "[e] No KLUT network in store\n";
        env->out() << "[i] Load a KLUT network first (e.g., read_blif)\n";
        return;
      }
      result = execute_klut_cec(kluts.current());
    } else if (force_xmg) {
      if (xmgs.empty()) {
        env->out() << "[e] No XMG network in store\n";
        env->out() << "[i] Load an XMG network first (e.g., read_verilog)\n";
        return;
      }
      result = execute_xmg_cec(xmgs.current());
    } else if (force_aig) {
      if (aigs.empty()) {
        env->out() << "[e] No AIG network in store\n";
        env->out() << "[i] Load an AIG network first (e.g., read_aiger)\n";
        return;
      }
      result = execute_aig_cec(aigs.current());
    } else {
      if (!kluts.empty()) {
        env->out() << "[i] Auto-detected KLUT network\n";
        result = execute_klut_cec(kluts.current());
      } else if (!xmgs.empty()) {
        env->out() << "[i] Auto-detected XMG network\n";
        result = execute_xmg_cec(xmgs.current());
      } else if (!aigs.empty()) {
        env->out() << "[i] Auto-detected AIG network\n";
        result = execute_aig_cec(aigs.current());
      } else {
        env->out() << "[e] No miter circuit in store (AIG/KLUT/XMG)\n";
        env->out() << "[i] Tip: Load a miter circuit before running minisat\n";
        env->out() << "[i] Example: read_aiger <file.aig>\n";
        return;
      }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    if (!result) {
      env->out() << "[e] Equivalence checking failed\n";
    }

    env->out() << fmt::format("[i] Total wall time: {:.3f} s\n",
                              duration.count() / 1000.0);
  }

 private:
  velor::minisat::cnf_generation_method parse_cnf_method() const {
    if (cnf_method_str == "mockturtle" || cnf_method_str == "m" ||
        cnf_method_str == "mock") {
      return velor::minisat::cnf_generation_method::MOCKTURTLE;
    } else if (cnf_method_str == "vlr" || cnf_method_str == "v") {
      return velor::minisat::cnf_generation_method::VLR;
    } else {
      env->out() << fmt::format(
          "[w] Unknown CNF method '{}', using 'mockturtle'\n", cnf_method_str);
      return velor::minisat::cnf_generation_method::MOCKTURTLE;
    }
  }

  std::string get_cnf_method_name(
      velor::minisat::cnf_generation_method method) const {
    switch (method) {
      case velor::minisat::cnf_generation_method::MOCKTURTLE:
        return "Mockturtle (original)";
      case velor::minisat::cnf_generation_method::VLR:
        return "VLR (Verification Logic Representation)";
      default:
        return "Unknown";
    }
  }

  bool execute_aig_cec(const mockturtle::aig_network& aig_miter) {
    env->out() << fmt::format("[i] Input AIG miter: {} gates, {} PIs, {} POs\n",
                              aig_miter.num_gates(), aig_miter.num_pis(),
                              aig_miter.num_pos());

    auto cnf_method = parse_cnf_method();

    if (is_set("verbose")) {
      env->out() << fmt::format("[i] CNF method: {}\n",
                                get_cnf_method_name(cnf_method));
      env->out() << fmt::format("[i] Cube propagation: {}\n",
                                is_set("no_cube_prop") ? "off" : "on");
    }

    velor::minisat::process_parallel_cec_params params;
    params.verbose = is_set("verbose");
    params.print_solver_stats = is_set("solver_stats");
    params.num_processes = procs_;
    params.cnf_method = cnf_method;
    params.enable_cube_propagation = !is_set("no_cube_prop");

    velor::minisat::process_parallel_cec_stats stats;

    auto result = velor::minisat::process_parallel_equivalence_checking_minisat(
        aig_miter, params, &stats);

    return print_results(result, stats);
  }

  bool execute_klut_cec(const mockturtle::klut_network& klut_miter) {
    env->out() << fmt::format("[i] Input KLUT miter: {} LUTs, {} PIs, {} POs\n",
                              klut_miter.num_gates(), klut_miter.num_pis(),
                              klut_miter.num_pos());

    auto cnf_method = parse_cnf_method();

    if (is_set("verbose")) {
      env->out() << fmt::format("[i] CNF method: {}\n",
                                get_cnf_method_name(cnf_method));
      env->out() << fmt::format("[i] Cube propagation: {}\n",
                                is_set("no_cube_prop") ? "off" : "on");
    }

    velor::minisat::process_parallel_cec_params params;
    params.verbose = is_set("verbose");
    params.print_solver_stats = is_set("solver_stats");
    params.num_processes = procs_;
    params.cnf_method = cnf_method;
    params.enable_cube_propagation = !is_set("no_cube_prop");

    velor::minisat::process_parallel_cec_stats stats;

    auto result = velor::minisat::process_parallel_equivalence_checking_minisat(
        klut_miter, params, &stats);

    return print_results(result, stats);
  }

  bool execute_xmg_cec(const mockturtle::xmg_network& xmg_miter) {
    env->out() << fmt::format("[i] Input XMG miter: {} gates, {} PIs, {} POs\n",
                              xmg_miter.num_gates(), xmg_miter.num_pis(),
                              xmg_miter.num_pos());

    auto cnf_method = parse_cnf_method();

    if (is_set("verbose")) {
      env->out() << fmt::format("[i] CNF method: {}\n",
                                get_cnf_method_name(cnf_method));
      env->out() << fmt::format("[i] Cube propagation: {}\n",
                                is_set("no_cube_prop") ? "off" : "on");
    }

    velor::minisat::process_parallel_cec_params params;
    params.verbose = is_set("verbose");
    params.print_solver_stats = is_set("solver_stats");
    params.num_processes = procs_;
    params.cnf_method = cnf_method;
    params.enable_cube_propagation = !is_set("no_cube_prop");

    velor::minisat::process_parallel_cec_stats stats;

    auto result = velor::minisat::process_parallel_equivalence_checking_minisat(
        xmg_miter, params, &stats);

    return print_results(result, stats);
  }

  bool print_results(std::optional<bool> result,
                     const velor::minisat::process_parallel_cec_stats& stats) {
    if (is_set("verbose")) {
      stats.report();
    } else {
      env->out() << fmt::format("[i] CNF: {} vars, {} clauses\n",
                                stats.num_vars, stats.num_clauses);
      env->out() << fmt::format(
          "[i] SAT stats: variables={}, conflicts={}, propagations={}\n",
          stats.sat_variables, stats.sat_conflicts, stats.sat_propagations);
      if (stats.trivial_unsat_cubes > 0) {
        env->out() <<
            fmt::format("[i] Trivial UNSAT cubes: {}\n", stats.trivial_unsat_cubes);
      }
      if (stats.simplified_clauses_removed > 0 ||
          stats.simplified_literals_removed > 0) {
        env->out() << fmt::format("[i] Simplify removed cls/lit: {}/{}\n",
                                  stats.simplified_clauses_removed,
                                  stats.simplified_literals_removed);
      }
      env->out() << fmt::format("[i] SAT time: {:.3f} s\n",
                                mockturtle::to_seconds(stats.time_sat));
    }

    if (!result.has_value()) {
      env->out() << "[?] Result: UNKNOWN\n";
      return false;
    }

    if (*result) {
      env->out() << "[✗] Circuits are NOT equivalent\n";
      if (is_set("show_cex") && !stats.counterexample.empty()) {
        env->out() << "[i] Counterexample: ";
        for (size_t i = 0; i < std::min(stats.counterexample.size(), 10UL);
             ++i) {
          env->out() << stats.counterexample[i];
        }
        if (stats.counterexample.size() > 10) {
          env->out() << "...";
        }
        env->out() << "\n";
      }
    } else {
      env->out() << "[✓] Circuits are EQUIVALENT\n";
    }

    return true;
  }

 private:
  int procs_ = 1;
  std::string cnf_method_str{"mockturtle"};
};

ALICE_ADD_COMMAND(minisat, "Verification")

}  // namespace alice
