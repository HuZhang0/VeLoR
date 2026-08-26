#ifndef LUT_MAPPING_HPP
#define LUT_MAPPING_HPP

#include <mockturtle/mockturtle.hpp>
using namespace mockturtle;

namespace alice {

class lut_mapping_command : public command {
 public:
  explicit lut_mapping_command(const environment::ptr& env)
      : command(env, "LUT mapping : using AIG as default") {
    add_option("cut_size, -k", cut_size,
               "set the cut size from 2 to 8, default = 4");
    add_flag("--verbose, -v", "print the information");
  }

 protected:
  void execute() {
    auto start = std::chrono::high_resolution_clock::now();

    lut_mapping_params ps;
    if (store<aig_network>().size() == 0) {
      assert(false && "no AIG in the store");
    }
    /* derive some AIG */
    aig_network aig = store<aig_network>().current();
    mapping_view<aig_network, true> mapped_aig{aig};
    ps.cut_enumeration_ps.cut_size = cut_size;
    lut_mapping<mapping_view<aig_network, true>, true, cut_enumeration_mf_cut>(
        mapped_aig, ps);
    /* collapse into k-LUT network */
    const auto klut = *collapse_mapped_network<klut_network>(mapped_aig);
    store<klut_network>().extend();
    store<klut_network>().current() = klut;
     auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    env->out() << fmt::format("[i] Mapping completed in {:.2f} s\n",
                              elapsed.count());
  }

 private:
  unsigned cut_size = 4u;
};

ALICE_ADD_COMMAND(lut_mapping, "Mapping")
}  // namespace alice

#endif
