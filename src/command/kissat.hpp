#pragma once

#include <fmt/format.h>

#include <alice/alice.hpp>
#include <mockturtle/mockturtle.hpp>

#include "../core/parallel_cec.hpp"

namespace alice {

/*! \brief Kissat-based equivalence checking command.
 *
 * This command directly invokes the kissat SAT solver on AIG, KLUT, or XMG
 * miter circuits for equivalence checking.
 */
class kissat_command : public command {
 public:
  kissat_command(const environment::ptr& env)
      : command(env,
                "Kissat-based equivalence checking (AIG/KLUT/XMG miters)") {
    add_option("--procs,-p", num_procs,
               "Number of processes (0=auto, 1=sequential)", 0);
    add_option("--split,-s", num_split_vars,
               "Number of split variables (0=auto from procs)", 0);
    add_option(
        "--cnf_method,-C", cnf_method_str,
        "CNF generation method: 'mockturtle' (default), 'vlr' (VLR-based)",
        "mockturtle");
    add_option("--kissat_cfg,-k", kissat_cfg,
           "Kissat configuration: '', 'default', 'sat', 'unsat'", "");
    add_flag("--aig,-a", "Force verification on AIG network");
    add_flag("--xmg,-x", "Force verification on XMG network");
    add_flag("--lut,-l", "Force verification on KLUT network");
    add_flag("--no_kissat_opt", "Disable explicit Kissat optimization options");
    add_flag("--no_cube_prop",
         "Disable assumption propagation and CNF simplification per cube");
    add_flag("--basic_opt",
         "Use basic preprocessing (disable aggressive optimization tuning)");
    add_flag("--solver_stats,-t", "Print detailed SAT solver statistics");
    add_flag("--verbose,-v", "Verbose output");
    add_flag("--show_cex,-c", "Show counterexample if not equivalent");
  }

 protected:
  void execute() override {
    auto start = std::chrono::high_resolution_clock::now();

    bool result = false;

    // Get network stores
    auto& kluts = store<mockturtle::klut_network>();
    auto& xmgs = store<mockturtle::xmg_network>();
    auto& aigs = store<mockturtle::aig_network>();

    // Check for explicit network type flags
    bool force_aig = is_set("aig");
    bool force_xmg = is_set("xmg");
    bool force_lut = is_set("lut");

    // Count how many flags are set
    int flag_count =
        (force_aig ? 1 : 0) + (force_xmg ? 1 : 0) + (force_lut ? 1 : 0);

    if (flag_count > 1) {
      env->out()
          << "[e] Only one network type flag can be specified at a time\n";
      env->out() << "[i] Use --aig, --xmg, or --lut (not multiple)\n";
      return;
    }

    // Execute based on explicit flag or auto-detection
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
      // Auto-detect: Try networks in priority order: KLUT → XMG → AIG
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
        env->out() << "[i] Tip: Load a miter circuit before running kissat\n";
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
  /*! \brief Parse CNF generation method from string. */
  velor::cnf_generation_method parse_cnf_method() const {
    if (cnf_method_str == "mockturtle" || cnf_method_str == "m" ||
        cnf_method_str == "mock") {
      return velor::cnf_generation_method::MOCKTURTLE;
    } else if (cnf_method_str == "vlr" || cnf_method_str == "v") {
      return velor::cnf_generation_method::VLR;
    } else {
      env->out() << fmt::format(
          "[w] Unknown CNF method '{}', using 'mockturtle'\n", cnf_method_str);
      return velor::cnf_generation_method::MOCKTURTLE;
    }
  }

  /*! \brief Get CNF method name for display. */
  std::string get_cnf_method_name(velor::cnf_generation_method method) const {
    switch (method) {
      case velor::cnf_generation_method::MOCKTURTLE:
        return "Mockturtle (original)";
      case velor::cnf_generation_method::VLR:
        return "VLR (Verification Logic Representation)";
      default:
        return "Unknown";
    }
  }

  /*! \brief Parse and validate Kissat configuration string. */
  std::string parse_kissat_configuration() const {
    if (kissat_cfg.empty()) {
      return "";
    }
    if (kissat_cfg == "default" || kissat_cfg == "sat" ||
        kissat_cfg == "unsat") {
      return kissat_cfg;
    }
    env->out() << fmt::format(
        "[w] Unknown Kissat configuration '{}', ignored\n", kissat_cfg);
    return "";
  }

  /*! \brief Execute CEC on AIG network. */
  bool execute_aig_cec(const mockturtle::aig_network& aig_miter) {
    env->out() << fmt::format("[i] Input AIG miter: {} gates, {} PIs, {} POs\n",
                              aig_miter.num_gates(), aig_miter.num_pis(),
                              aig_miter.num_pos());

    auto cnf_method = parse_cnf_method();

    if (is_set("verbose")) {
      env->out() << fmt::format("[i] CNF method: {}\n",
                                get_cnf_method_name(cnf_method));
      env->out() << fmt::format(
          "[i] Kissat opts: {}, cube-prop: {}, aggressive: {}, cfg: '{}'\n",
          is_set("no_kissat_opt") ? "off" : "on",
          is_set("no_cube_prop") ? "off" : "on",
          is_set("basic_opt") ? "off" : "on", kissat_cfg);
    }

    velor::process_parallel_cec_params params;
    params.num_processes = num_procs;
    params.num_split_vars = num_split_vars;
    params.verbose = is_set("verbose");
    params.cnf_method = cnf_method;
    params.print_solver_stats = is_set("solver_stats");
    params.enable_kissat_optimizations = !is_set("no_kissat_opt");
    params.enable_cube_propagation = !is_set("no_cube_prop");
    params.aggressive_kissat_preprocess = !is_set("basic_opt");
    params.kissat_configuration = parse_kissat_configuration();

    velor::process_parallel_cec_stats stats;

    auto result =
        velor::process_parallel_equivalence_checking(aig_miter, params, &stats);

    return print_results(result, stats);
  }

  /*! \brief Execute CEC on KLUT network. */
  bool execute_klut_cec(const mockturtle::klut_network& klut_miter) {
    env->out() << fmt::format("[i] Input KLUT miter: {} LUTs, {} PIs, {} POs\n",
                              klut_miter.num_gates(), klut_miter.num_pis(),
                              klut_miter.num_pos());

    auto cnf_method = parse_cnf_method();

    if (is_set("verbose")) {
      env->out() << fmt::format("[i] CNF method: {}\n",
                                get_cnf_method_name(cnf_method));
      env->out() << fmt::format(
          "[i] Kissat opts: {}, cube-prop: {}, aggressive: {}, cfg: '{}'\n",
          is_set("no_kissat_opt") ? "off" : "on",
          is_set("no_cube_prop") ? "off" : "on",
          is_set("basic_opt") ? "off" : "on", kissat_cfg);
    }

    velor::process_parallel_cec_params params;
    params.num_processes = num_procs;
    params.num_split_vars = num_split_vars;
    params.verbose = is_set("verbose");
    params.cnf_method = cnf_method;
    params.print_solver_stats = is_set("solver_stats");
    params.enable_kissat_optimizations = !is_set("no_kissat_opt");
    params.enable_cube_propagation = !is_set("no_cube_prop");
    params.aggressive_kissat_preprocess = !is_set("basic_opt");
    params.kissat_configuration = parse_kissat_configuration();

    velor::process_parallel_cec_stats stats;

    auto result =
        velor::process_parallel_equivalence_checking(klut_miter, params, &stats);

    return print_results(result, stats);
  }

  /*! \brief Execute CEC on XMG network. */
  bool execute_xmg_cec(const mockturtle::xmg_network& xmg_miter) {
    env->out() << fmt::format("[i] Input XMG miter: {} gates, {} PIs, {} POs\n",
                              xmg_miter.num_gates(), xmg_miter.num_pis(),
                              xmg_miter.num_pos());

    auto cnf_method = parse_cnf_method();

    if (is_set("verbose")) {
      env->out() << fmt::format("[i] CNF method: {}\n",
                                get_cnf_method_name(cnf_method));
      env->out() << fmt::format(
          "[i] Kissat opts: {}, cube-prop: {}, aggressive: {}, cfg: '{}'\n",
          is_set("no_kissat_opt") ? "off" : "on",
          is_set("no_cube_prop") ? "off" : "on",
          is_set("basic_opt") ? "off" : "on", kissat_cfg);
    }

    velor::process_parallel_cec_params params;
    params.num_processes = num_procs;
    params.num_split_vars = num_split_vars;
    params.verbose = is_set("verbose");
    params.cnf_method = cnf_method;
    params.print_solver_stats = is_set("solver_stats");
    params.enable_kissat_optimizations = !is_set("no_kissat_opt");
    params.enable_cube_propagation = !is_set("no_cube_prop");
    params.aggressive_kissat_preprocess = !is_set("basic_opt");
    params.kissat_configuration = parse_kissat_configuration();

    velor::process_parallel_cec_stats stats;

    auto result =
        velor::process_parallel_equivalence_checking(xmg_miter, params, &stats);

    return print_results(result, stats);
  }

  /*! \brief Print CEC results. */
  bool print_results(std::optional<bool> result,
                     const velor::process_parallel_cec_stats& stats) {
    // Print statistics
    if (is_set("verbose")) {
      stats.report();
    } else {
      env->out() << fmt::format("[i] CNF: {} vars, {} clauses\n",
                                stats.num_vars, stats.num_clauses);
      if (stats.num_processes > 1) {
        env->out() << fmt::format("[i] Processes: {} (killed: {})\n",
                                  stats.num_processes, stats.num_killed);
        if (stats.trivial_unsat_cubes > 0) {
          env->out() <<
              fmt::format("[i] Trivial UNSAT cubes: {}\n", stats.trivial_unsat_cubes);
        }
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

    // Print result
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
  uint32_t num_procs{0};
  uint32_t num_split_vars{0};
  std::string cnf_method_str{"mockturtle"};
  std::string kissat_cfg{""};
};

ALICE_ADD_COMMAND(kissat, "Verification")

}  // namespace alice
