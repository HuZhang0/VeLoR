#pragma once

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <alice/alice.hpp>
#include <chrono>
#include <kitty/kitty.hpp>
#include <mockturtle/algorithms/mapper.hpp>
#include <lorina/genlib.hpp>
#include <map>
#include <mockturtle/algorithms/emap.hpp>
#include <mockturtle/io/genlib_reader.hpp>
#include <mockturtle/mockturtle.hpp>
#include <mockturtle/utils/tech_library.hpp>

namespace alice {

/*! \brief Technology library based LUT mapping using emap/map.
 *
 * Maps an AIG to VLR network using a technology library.
 * The library should contain 2-6 input SAT-friendly functions.
 */
class vlr_command : public command {
 public:
  vlr_command(const environment::ptr& env)
      : command(env, "Technology library based LUT mapping to VLR") {
    add_option("--genlib,-g", genlib_file,
               "Genlib technology library file (required)", "");
    add_option("--cut_size,-k", cut_size, "Maximum cut size (default=3)", 3);
    add_option("--engine,-e", mapping_engine,
               "Mapping engine: emap or map (default=emap)", "emap");
    add_flag("--area,-a", "Use area-oriented mapping (default: delay-flow)");
    add_flag("--verbose,-v", "Verbose output");
  }

 protected:
  void execute() override {
    // Check if we have an AIG

    auto aig = store<mockturtle::aig_network>().current();

    if (is_set("verbose")) {
      env->out() << fmt::format("[i] Input AIG: {} gates, {} PIs, {} POs\n",
                                aig.num_gates(), aig.num_pis(), aig.num_pos());
    }

    // Load technology library from genlib file
    if (genlib_file.empty()) {
      env->err() << "[e] Please specify a genlib file with --genlib <file>\n";
      env->err() << "[i] Example genlib file location: lib/simple_sat.genlib\n";
      return;
    }

    std::vector<mockturtle::gate> gates;
    if (lorina::read_genlib(genlib_file, mockturtle::genlib_reader(gates)) !=
        lorina::return_code::success) {
      env->err() << "[e] Failed to read genlib file: " << genlib_file << "\n";
      return;
    }

    // Filter gates by cut_size
    std::vector<mockturtle::gate> filtered_gates;
    for (const auto& gate : gates) {
      if (gate.num_vars <= cut_size) {
        filtered_gates.push_back(gate);
      }
    }

    if (filtered_gates.empty()) {
      env->err() << fmt::format(
          "[e] No gates found with <= {} inputs in library\n", cut_size);
      return;
    }

    if (is_set("verbose")) {
      env->out() << fmt::format(
          "[i] Loaded genlib library: {} gates from {} (filtered {} gates with "
          "> {} inputs)\n",
          filtered_gates.size(), genlib_file,
          gates.size() - filtered_gates.size(), cut_size);
    }

    // Create technology library
    mockturtle::tech_library_params lib_ps;
    lib_ps.verbose = is_set("verbose");

    mockturtle::tech_library<6u,
                             mockturtle::classification_type::np_configurations>
        lib(filtered_gates, lib_ps);

    if (is_set("verbose")) {
      env->out() << "[i] Technology library created successfully\n";
    }

    std::string engine = mapping_engine;
    std::transform(engine.begin(), engine.end(), engine.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (engine != "emap" && engine != "map") {
      env->err() << fmt::format(
          "[e] Unknown mapping engine '{}'. Use 'emap' or 'map'\n", engine);
      return;
    }

    if (is_set("verbose")) {
      env->out() << fmt::format("[i] Mapping engine: {}\n", engine);
    }

    double report_area = 0.0;
    double report_delay = 0.0;

    // Perform mapping
    auto start = std::chrono::high_resolution_clock::now();
    auto klut = [&]() {
      if (engine == "emap") {
        mockturtle::emap_params emap_ps;
        emap_ps.cut_enumeration_ps.cut_size = cut_size;
        emap_ps.area_oriented_mapping = is_set("area");
        emap_ps.matching_mode = mockturtle::emap_params::hybrid;
        emap_ps.verbose = is_set("verbose");

        mockturtle::emap_stats emap_st;
        auto klut_binding = mockturtle::emap_klut<6u>(aig, lib, emap_ps, &emap_st);
        report_area = static_cast<double>(emap_st.area);
        report_delay = static_cast<double>(emap_st.delay);
        return mockturtle::cleanup_dangling<decltype(klut_binding),
                          mockturtle::klut_network>(
          klut_binding);
      }

      mockturtle::map_params map_ps;
      map_ps.cut_enumeration_ps.cut_size = cut_size;
      map_ps.skip_delay_round = is_set("area");
      map_ps.verbose = is_set("verbose");

      mockturtle::map_stats map_st;
      auto klut_binding =
          mockturtle::map<mockturtle::aig_network, 6u>(aig, lib, map_ps, &map_st);
      report_area = static_cast<double>(map_st.area);
      report_delay = static_cast<double>(map_st.delay);
        return mockturtle::cleanup_dangling<decltype(klut_binding),
                          mockturtle::klut_network>(
          klut_binding);
    }();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    // Store result
    store<mockturtle::klut_network>().extend() = klut;

    // Report statistics
    env->out() << fmt::format(
        "[i] === Techlibrary-based LUT Mapping Results ===\n");
    env->out() << fmt::format("[i] Engine: {}\n", engine);
    env->out() << fmt::format("[i] Mapping completed in {:.2f} s\n",
                              elapsed.count());
    env->out() << fmt::format("\n[i] Network Statistics:\n");
    env->out() << fmt::format("    - Total LUTs: {}\n",
                              static_cast<uint32_t>(klut.num_gates()));
    env->out() << fmt::format("    - Primary Inputs: {}\n",
                              static_cast<uint32_t>(klut.num_pis()));
    env->out() << fmt::format("    - Primary Outputs: {}\n",
                              static_cast<uint32_t>(klut.num_pos()));

    // Area and Delay information
    env->out() << fmt::format("\n[i] Area & Delay:\n");
    env->out() << fmt::format("    - Total Area: {:.4f}\n", report_area);
    env->out() << fmt::format("    - Critical Path Delay: {}\n",
                  static_cast<uint32_t>(report_delay));
    env->out() << fmt::format("    - Area-Delay Product: {:.4f}\n",
                  report_area * report_delay);

    if (is_set("verbose")) {
      // Analyze LUT size distribution and function types
      std::array<uint32_t, 10> lut_dist{0};
      std::map<std::string, uint32_t> func_type_dist;
      double total_lut_area = report_area;

      klut.foreach_gate([&](auto const& n) {
        auto func = klut.node_function(n);
        uint32_t num_vars = func.num_vars();
        if (num_vars <= 9) {
          lut_dist[num_vars]++;
        }

        // Get function classification
        auto tt = func;

        // Classify common function types
        std::string func_name = "OTHER";
        if (num_vars == 1) {
          if (kitty::is_const0(tt))
            func_name = "CONST0";
          else if (kitty::is_const0(~tt))
            func_name = "CONST1";
          else {
            kitty::dynamic_truth_table tt_buf(1);
            kitty::create_from_hex_string(tt_buf, "2");  // 0b10
            kitty::dynamic_truth_table tt_inv(1);
            kitty::create_from_hex_string(tt_inv, "1");  // 0b01

            if (tt == tt_buf)
              func_name = "BUF";
            else if (tt == tt_inv)
              func_name = "NOT";
            else
              func_name = fmt::format("1-LUT({})", kitty::to_hex(tt));
          }
        } else if (num_vars == 2) {
          // if (kitty::is_const0(tt))
          //   func_name = "CONST0";
          // else if (kitty::is_const0(~tt))
          //   func_name = "CONST1";
          // else {
          // Create reference truth tables for comparison
          kitty::dynamic_truth_table tt_and2(2);
          kitty::create_from_hex_string(tt_and2, "8");
          kitty::dynamic_truth_table tt_or2(2);
          kitty::create_from_hex_string(tt_or2, "e");
          kitty::dynamic_truth_table tt_xor2(2);
          kitty::create_from_hex_string(tt_xor2, "6");
          kitty::dynamic_truth_table tt_xnor2(2);
          kitty::create_from_hex_string(tt_xnor2, "9");
          kitty::dynamic_truth_table tt_nand2(2);
          kitty::create_from_hex_string(tt_nand2, "7");
          kitty::dynamic_truth_table tt_nor2(2);
          kitty::create_from_hex_string(tt_nor2, "1");

          if (tt == tt_and2)
            func_name = "AND2";
          else if (tt == tt_or2)
            func_name = "OR2";
          else if (tt == tt_xor2)
            func_name = "XOR2";
          else if (tt == tt_xnor2)
            func_name = "XNOR2";
          else if (tt == tt_nand2)
            func_name = "NAND2";
          else if (tt == tt_nor2)
            func_name = "NOR2";
          else
            func_name = fmt::format("2-LUT({})", kitty::to_hex(tt));
          // }
        } else if (num_vars == 3) {
          // Create reference truth tables for 3-input functions
          kitty::dynamic_truth_table tt_and3(3);
          kitty::create_from_hex_string(tt_and3, "80");
          kitty::dynamic_truth_table tt_or3(3);
          kitty::create_from_hex_string(tt_or3, "fe");
          kitty::dynamic_truth_table tt_nand3(3);
          kitty::create_from_hex_string(tt_nand3, "7f");
          kitty::dynamic_truth_table tt_nor3(3);
          kitty::create_from_hex_string(tt_nor3, "01");
          kitty::dynamic_truth_table tt_xor3(3);
          kitty::create_from_hex_string(tt_xor3, "96");
          kitty::dynamic_truth_table tt_maj3(3);
          kitty::create_from_hex_string(tt_maj3, "e8");
          kitty::dynamic_truth_table tt_aoi21(3);
          kitty::create_from_hex_string(tt_aoi21, "d8");
          kitty::dynamic_truth_table tt_oai21(3);
          kitty::create_from_hex_string(tt_oai21, "27");

          if (tt == tt_and3)
            func_name = "AND3";
          else if (tt == tt_or3)
            func_name = "OR3";
          else if (tt == tt_nand3)
            func_name = "NAND3";
          else if (tt == tt_nor3)
            func_name = "NOR3";
          else if (tt == tt_xor3)
            func_name = "XOR3";
          else if (tt == tt_maj3)
            func_name = "MAJ3";
          else if (tt == tt_aoi21)
            func_name = "AOI21";
          else if (tt == tt_oai21)
            func_name = "OAI21";
          else
            func_name = fmt::format("3-LUT({})", kitty::to_hex(tt));
        } else {
          func_name = fmt::format("{}-LUT", num_vars);
        }
        func_type_dist[func_name]++;
      });

      env->out() << fmt::format("\n[i] LUT Size Distribution:\n");
      uint32_t total_luts = 0;
      for (size_t i = 1; i <= 6; ++i) {
        if (lut_dist[i] > 0) {
          total_luts += lut_dist[i];
          double percentage =
              100.0 * lut_dist[i] / static_cast<double>(klut.num_gates());
          env->out() << fmt::format("    - {}-input LUTs: {:4d} ({:5.2f}%)\n",
                                    static_cast<uint32_t>(i), lut_dist[i],
                                    percentage);
        }
      }

      env->out() << fmt::format(
          "\n[i] LUT Function Type Distribution (Top 10):\n");
      // Sort by count
      std::vector<std::pair<std::string, uint32_t>> sorted_funcs(
          func_type_dist.begin(), func_type_dist.end());
      std::sort(
          sorted_funcs.begin(), sorted_funcs.end(),
          [](const auto& a, const auto& b) { return a.second > b.second; });

      int count = 0;
      for (const auto& [func_name, cnt] : sorted_funcs) {
        if (count++ >= 10) break;
        double percentage = 100.0 * cnt / static_cast<double>(klut.num_gates());
        env->out() << fmt::format("    - {:12s}: {:4d} ({:5.2f}%)\n", func_name,
                                  cnt, percentage);
      }

      if (sorted_funcs.size() > 10) {
        env->out() << fmt::format(
            "    - ... and {} more function types\n",
            static_cast<uint32_t>(sorted_funcs.size() - 10));
      }

      // Additional statistics
      env->out() << fmt::format("\n[i] Detailed Mapping Statistics:\n");
      env->out() << fmt::format("    - Total function types: {}\n",
                                static_cast<uint32_t>(func_type_dist.size()));
      env->out() << fmt::format("    - Accumulated LUT area: {:.4f}\n",
                                total_lut_area);

      uint32_t num_gates = klut.num_gates();
      if (num_gates > 0) {
        env->out() << fmt::format(
            "    - Average area per LUT: {:.4f}\n",
            total_lut_area / static_cast<double>(num_gates));
      }
      double avg_fanin = 0.0;
      for (size_t i = 1; i <= 6; ++i) {
        avg_fanin += i * lut_dist[i];
      }
      if (num_gates > 0) {
        avg_fanin /= static_cast<double>(num_gates);
      }
      env->out() << fmt::format("    - Average fanin: {:.2f}\n", avg_fanin);
    }

    env->out() << "[i] =====================================\n";
  }

 private:
  std::string genlib_file;
  std::string mapping_engine{"emap"};
  uint32_t cut_size{3};
};

ALICE_ADD_COMMAND(vlr, "Mapping")

}  // namespace alice
