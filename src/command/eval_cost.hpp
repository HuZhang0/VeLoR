#pragma once

#include <fmt/format.h>

#include <alice/alice.hpp>
#include <fstream>
#include <mockturtle/mockturtle.hpp>
#include <sstream>
#include <unordered_map>

namespace alice {

/*! \brief Gate cost information from genlib. */
struct gate_cost_info {
  std::string name;
  double area;
  std::string function;
};

/*! \brief Network statistics for cost evaluation. */
struct network_cost_stats {
  std::unordered_map<std::string, uint32_t> gate_counts;
  double total_area{0.0};
  uint32_t total_gates{0};
  uint32_t num_pis{0};
  uint32_t num_pos{0};
};

/*! \brief Command to evaluate circuit verification cost based on genlib. */
class eval_cost_command : public command {
 public:
  eval_cost_command(const environment::ptr& env)
      : command(env, "Evaluate circuit verification cost based on genlib") {
    add_option("--genlib,-g", genlib_file,
               "Genlib file for gate area information", "");
    add_flag("--verbose,-v", "Verbose output with detailed gate distribution");
    add_flag("--aig,-a", "Evaluate AIG network from store");
    add_flag("--xmg,-x", "Evaluate XMG network from store");
    add_flag("--klut,-l", "Evaluate KLUT network from store");
  }

 protected:
  void execute() override {
    // Load genlib if specified
    if (!genlib_file.empty()) {
      if (!load_genlib(genlib_file)) {
        env->out() << "[e] Failed to load genlib file: " << genlib_file << "\n";
        return;
      }
      if (is_set("verbose")) {
        env->out() << fmt::format("[i] Loaded {} gates from genlib\n",
                                  gate_costs_.size());
      }
    } else {
      // Use default costs if no genlib provided
      init_default_costs();
      if (is_set("verbose")) {
        env->out() << "[i] Using default gate costs\n";
      }
    }

    bool evaluated = false;

    // Evaluate networks based on flags
    if (is_set("aig")) {
      evaluated |= evaluate_aig();
    }
    if (is_set("xmg")) {
      evaluated |= evaluate_xmg();
    }
    if (is_set("klut")) {
      evaluated |= evaluate_klut();
    }

    // If no specific flag, try all networks in order
    if (!evaluated) {
      if (!evaluate_klut()) {
        if (!evaluate_xmg()) {
          if (!evaluate_aig()) {
            env->out() << "[e] No network found in store (AIG/XMG/KLUT)\n";
            env->out() << "[i] Tip: Load a network before running eval_cost\n";
          }
        }
      }
    }
  }

 private:
  std::string genlib_file;
  std::unordered_map<std::string, gate_cost_info> gate_costs_;

  /*! \brief Load genlib file and parse gate costs. */
  bool load_genlib(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
      return false;
    }

    std::string line;
    while (std::getline(file, line)) {
      // Skip comments and empty lines
      if (line.empty() || line[0] == '#') continue;

      // Parse GATE lines: GATE name area function
      std::istringstream iss(line);
      std::string keyword;
      iss >> keyword;

      if (keyword == "GATE") {
        gate_cost_info info;
        iss >> info.name >> info.area;

        // Read rest of line as function
        std::string rest;
        std::getline(iss, rest);
        info.function = rest;

        gate_costs_[info.name] = info;
      }
    }

    return !gate_costs_.empty();
  }

  /*! \brief Initialize default gate costs. */
  void init_default_costs() {
    // 2-input gates
    gate_costs_["and2"] = {"and2", 0.112, "O=a*b"};
    gate_costs_["or2"] = {"or2", 0.112, "O=a+b"};
    gate_costs_["nand2"] = {"nand2", 0.112, "O=!(a*b)"};
    gate_costs_["nor2"] = {"nor2", 0.112, "O=!(a+b)"};
    gate_costs_["xor2"] = {"xor2", 0.242, "O=a^b"};
    gate_costs_["xnor2"] = {"xnor2", 0.242, "O=!(a^b)"};

    // 3-input gates
    gate_costs_["and3"] = {"and3", 0.1, "O=a*b*c"};
    gate_costs_["or3"] = {"or3", 0.1, "O=a+b+c"};
    gate_costs_["nand3"] = {"nand3", 0.1, "O=!(a*b*c)"};
    gate_costs_["nor3"] = {"nor3", 0.1, "O=!(a+b+c)"};
    gate_costs_["xor3"] = {"xor3", 0.312, "O=a^b^c"};
    gate_costs_["maj3"] = {"maj3", 0.299, "O=a*b+a*c+b*c"};
    gate_costs_["aoi21"] = {"aoi21", 0.238, "O=!(a*b+c)"};
    gate_costs_["oai21"] = {"oai21", 0.238, "O=!((a+b)*c)"};
    gate_costs_["aoi12"] = {"aoi12", 0.238, "O=!(a+b*c)"};
    gate_costs_["oai12"] = {"oai12", 0.238, "O=!(a*(b+c))"};
    // gate_costs_["mux2"] = {"mux2", 0.711, "O=s?b:a"};

    // Basic gates
    gate_costs_["inv"] = {"inv", 0.015, "O=!a"};
    gate_costs_["buf"] = {"buf", 0.01, "O=a"};
  }

  /*! \brief Identify gate type from truth table. */
  std::string identify_gate_type(const kitty::dynamic_truth_table& tt) {
    uint32_t num_inputs = tt.num_vars();
    if (num_inputs > 3) return "unknown";

    uint64_t bits = 0;
    if (num_inputs <= 6) {
      bits = tt._bits[0];
    } else {
      return "unknown";
    }

    // 1-input gates (INV, BUF)
    if (num_inputs == 1) {
      switch (bits & 0x3) {
        case 0x1:
          return "inv";  // 01 = NOT a
        case 0x2:
          return "buf";  // 10 = a
        default:
          return "unknown1";
      }
    }

    // 2-input gates
    if (num_inputs == 2) {
      switch (bits & 0xF) {
        case 0x8:
          return "and2";
        case 0xE:
          return "or2";
        case 0x7:
          return "nand2";
        case 0x1:
          return "nor2";
        case 0x6:
          return "xor2";
        case 0x9:
          return "xnor2";
        case 0x2:
          return "buf";  // a and !b (sometimes used as buf)
        case 0x4:
          return "buf";  // !a and b
        default:
          return "unknown2";
      }
    }

    // 3-input gates
    if (num_inputs == 3) {
      switch (bits & 0xFF) {
        case 0x80:
          return "and3";
        case 0xFE:
          return "or3";
        case 0x7F:
          return "nand3";
        case 0x01:
          return "nor3";
        case 0x96:
          return "xor3";
        case 0xE8:
          return "maj3";
        case 0x07:
          return "aoi21";
        case 0x1F:
          return "oai21";
        case 0x15:
          return "aoi12";
        case 0x57:
          return "oai12";
        case 0xD8:
          return "mux2";  // s?b:a = ab+sc
        default:
          return "unknown3";
      }
    }

    return "unknown";
  }

  /*! \brief Get gate cost. */
  double get_gate_cost(const std::string& gate_type) {
    auto it = gate_costs_.find(gate_type);
    if (it != gate_costs_.end()) {
      return it->second.area;
    }
    // Default cost for unknown gates
    return 1.0;
  }

  /*! \brief Evaluate AIG network. */
  bool evaluate_aig() {
    auto& aigs = store<mockturtle::aig_network>();
    if (aigs.empty()) {
      return false;
    }

    auto aig = aigs.current();
    network_cost_stats stats;

    env->out() << "\n[i] ========== AIG Network Cost Evaluation ==========\n";
    env->out() << fmt::format("[i] Network: {} gates, {} PIs, {} POs\n",
                              aig.num_gates(), aig.num_pis(), aig.num_pos());

    stats.num_pis = aig.num_pis();
    stats.num_pos = aig.num_pos();

    // Count AND gates and inverters
    uint32_t num_and = 0;
    uint32_t num_inv = 0;

    // Count AND gates
    num_and = aig.num_gates();

    // Count inverter usage (complemented literals)
    // Note: This counts each use of a complemented signal
    aig.foreach_gate([&](auto const& n) {
      aig.foreach_fanin(n, [&](auto const& f) {
        if (aig.is_complemented(f)) {
          num_inv++;
        }
      });
    });

    // Also count complemented POs
    aig.foreach_po([&](auto const& f) {
      if (aig.is_complemented(f)) {
        num_inv++;
      }
    });

    stats.gate_counts["and2"] = num_and;
    stats.gate_counts["inv"] = num_inv;
    stats.total_gates = num_and + num_inv;

    // Calculate total area
    stats.total_area =
        num_and * get_gate_cost("and2") + num_inv * get_gate_cost("inv");

    print_stats(stats, "AIG");
    return true;
  }

  /*! \brief Evaluate XMG network. */
  bool evaluate_xmg() {
    auto& xmgs = store<mockturtle::xmg_network>();
    if (xmgs.empty()) {
      return false;
    }

    auto xmg = xmgs.current();
    network_cost_stats stats;

    env->out() << "\n[i] ========== XMG Network Cost Evaluation ==========\n";
    env->out() << fmt::format("[i] Network: {} gates, {} PIs, {} POs\n",
                              xmg.num_gates(), xmg.num_pis(), xmg.num_pos());

    stats.num_pis = xmg.num_pis();
    stats.num_pos = xmg.num_pos();

    uint32_t num_maj = 0;
    uint32_t num_xor = 0;
    uint32_t num_inv = 0;

    // Count MAJ and XOR gates
    xmg.foreach_gate([&](auto const& n) {
      if (xmg.is_maj(n)) {
        num_maj++;
      } else if (xmg.is_xor3(n)) {
        num_xor++;
      }

      // Count inverter usage (complemented fanins)
      xmg.foreach_fanin(n, [&](auto const& f) {
        if (xmg.is_complemented(f)) {
          num_inv++;
        }
      });
    });

    // Also count complemented POs
    xmg.foreach_po([&](auto const& f) {
      if (xmg.is_complemented(f)) {
        num_inv++;
      }
    });

    stats.gate_counts["maj3"] = num_maj;
    stats.gate_counts["xor3"] = num_xor;
    stats.gate_counts["inv"] = num_inv;
    stats.total_gates = num_maj + num_xor + num_inv;

    // Calculate total area
    stats.total_area = num_maj * get_gate_cost("maj3") +
                       num_xor * get_gate_cost("xor3") +
                       num_inv * get_gate_cost("inv");

    print_stats(stats, "XMG");
    return true;
  }

  /*! \brief Evaluate KLUT network. */
  bool evaluate_klut() {
    auto& kluts = store<mockturtle::klut_network>();
    if (kluts.empty()) {
      return false;
    }

    auto klut = kluts.current();
    network_cost_stats stats;

    env->out() << "\n[i] ========== KLUT Network Cost Evaluation ==========\n";
    env->out() << fmt::format("[i] Network: {} LUTs, {} PIs, {} POs\n",
                              klut.num_gates(), klut.num_pis(), klut.num_pos());

    stats.num_pis = klut.num_pis();
    stats.num_pos = klut.num_pos();

    uint32_t num_inv = 0;

    // Analyze each LUT
    klut.foreach_gate([&](auto const& n) {
      auto tt = klut.node_function(n);
      uint32_t num_inputs = tt.num_vars();

      // Identify gate type
      std::string gate_type = identify_gate_type(tt);

      // Count gates by type
      stats.gate_counts[gate_type]++;
      stats.total_gates++;

      // Check for complemented fanins
      klut.foreach_fanin(n, [&](auto const& f) {
        if (klut.is_complemented(f)) {
          num_inv++;
        }
      });
    });

    // Check for complemented POs
    klut.foreach_po([&](auto const& f) {
      if (klut.is_complemented(f)) {
        num_inv++;
      }
    });

    if (num_inv > 0) {
      stats.gate_counts["inv"] = num_inv;
      stats.total_gates += num_inv;
    }

    // Calculate total area
    for (const auto& [gate_type, count] : stats.gate_counts) {
      stats.total_area += count * get_gate_cost(gate_type);
    }

    print_stats(stats, "KLUT");
    return true;
  }

  /*! \brief Print network statistics. */
  void print_stats(const network_cost_stats& stats,
                   const std::string& network_type) {
    env->out() << "\n[i] === Gate Distribution ===\n";

    // Sort gates by count (descending)
    std::vector<std::pair<std::string, uint32_t>> sorted_gates(
        stats.gate_counts.begin(), stats.gate_counts.end());
    std::sort(sorted_gates.begin(), sorted_gates.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    double total_area_check = 0.0;
    for (const auto& [gate_type, count] : sorted_gates) {
      double gate_cost = get_gate_cost(gate_type);
      double gate_area = count * gate_cost;
      total_area_check += gate_area;

      if (is_set("verbose") || count > 0) {
        env->out() << fmt::format(
            "  {:12s}: {:6d} gates × {:6.3f} area = {:8.2f}\n", gate_type,
            count, gate_cost, gate_area);
      }
    }

    env->out() << "\n[i] === Summary ===\n";
    env->out() << fmt::format("  Total gates  : {}\n", stats.total_gates);
    env->out() << fmt::format("  Total area   : {:.2f}\n", stats.total_area);
    env->out() << fmt::format("  PIs          : {}\n", stats.num_pis);
    env->out() << fmt::format("  POs          : {}\n", stats.num_pos);
    env->out() << fmt::format(
        "  Avg gate cost: {:.3f}\n",
        stats.total_area / std::max(1u, stats.total_gates));

    // Estimate verification complexity
    double verification_score = estimate_verification_complexity(stats);
    env->out() << fmt::format("  Verification score: {:.2f}\n",
                              verification_score);
    env->out() << "\n";
  }

  /*! \brief Estimate verification complexity based on gate distribution. */
  double estimate_verification_complexity(const network_cost_stats& stats) {
    double area_score = stats.total_area;
    double size_score = stats.total_gates * 0.1;
    return area_score + size_score;
  }
};

ALICE_ADD_COMMAND(eval_cost, "Analysis")

}  // namespace alice
