#pragma once

#include "../../lib/cadical/src/cadical.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace velor {

class CadicalTerminatorAdapter : public CaDiCaL::Terminator {
 public:
  explicit CadicalTerminatorAdapter(std::function<bool()>* cb) : cb_(cb) {}

  bool terminate() override {
    if (!cb_ || !(*cb_)) {
      return false;
    }
    return (*cb_)();
  }

 private:
  std::function<bool()>* cb_;
};

class CadicalWrapper {
 public:
  enum class Result { SATISFIABLE, UNSATISFIABLE, UNKNOWN, INTERRUPTED };

  CadicalWrapper()
      : solver_(std::make_unique<CaDiCaL::Solver>()), max_var_(0),
        terminator_(&terminate_callback_) {
    solver_->set("quiet", 1);
  }

  ~CadicalWrapper() = default;

  CadicalWrapper(const CadicalWrapper&) = delete;
  CadicalWrapper& operator=(const CadicalWrapper&) = delete;

  CadicalWrapper(CadicalWrapper&& other) noexcept
      : solver_(std::move(other.solver_)),
        max_var_(other.max_var_),
        terminate_callback_(std::move(other.terminate_callback_)),
        terminator_(&terminate_callback_) {
    if (solver_) {
      solver_->disconnect_terminator();
      solver_->connect_terminator(&terminator_);
    }
    other.max_var_ = 0;
  }

  CadicalWrapper& operator=(CadicalWrapper&& other) noexcept {
    if (this != &other) {
      solver_ = std::move(other.solver_);
      max_var_ = other.max_var_;
      terminate_callback_ = std::move(other.terminate_callback_);
      if (solver_) {
        solver_->disconnect_terminator();
        solver_->connect_terminator(&terminator_);
      }
      other.max_var_ = 0;
    }
    return *this;
  }

  void add_literal(int lit) {
    if (lit == 0) {
      throw std::invalid_argument("Cannot add literal 0");
    }
    solver_->add(lit);
    int var = std::abs(lit);
    if (var > max_var_) {
      max_var_ = var;
    }
  }

  void finish_clause() { solver_->add(0); }

  void add_clause(const std::vector<int>& clause) {
    for (int lit : clause) {
      add_literal(lit);
    }
    finish_clause();
  }

  void add_unit_clause(int lit) {
    add_literal(lit);
    finish_clause();
  }

  void reserve(int max_var) {
    if (max_var > max_var_) {
      solver_->resize(max_var);
      max_var_ = max_var;
    }
  }

  Result solve() {
    int res = solver_->solve();
    if (res == 10) return Result::SATISFIABLE;
    if (res == 20) return Result::UNSATISFIABLE;
    if (terminate_callback_ && terminate_callback_()) return Result::INTERRUPTED;
    return Result::UNKNOWN;
  }

  std::optional<bool> get_value(int var) const {
    if (var < 1 || var > max_var_) return std::nullopt;
    int val = solver_->val(var);
    if (val > 0) return true;
    if (val < 0) return false;
    return std::nullopt;
  }

  std::vector<std::optional<bool>> get_model(int max_var) const {
    std::vector<std::optional<bool>> model(max_var + 1);
    for (int i = 1; i <= max_var; ++i) {
      model[i] = get_value(i);
    }
    return model;
  }

  uint32_t get_max_variable() const { return static_cast<uint32_t>(max_var_); }

  uint32_t get_num_variables() const {
    return static_cast<uint32_t>(std::max(0, solver_->active()));
  }

  uint64_t get_num_conflicts() const {
    auto v = solver_->get_statistic_value("conflicts");
    return v < 0 ? 0 : static_cast<uint64_t>(v);
  }

  uint64_t get_num_propagations() const {
    auto v = solver_->get_statistic_value("propagations");
    return v < 0 ? 0 : static_cast<uint64_t>(v);
  }

  bool set_option(const std::string& name, int value) {
    return solver_->set(name.c_str(), value);
  }

  int get_option(const std::string& name) const {
    return solver_->get(name.c_str());
  }

  bool set_configuration(const std::string& cfg) {
    return solver_->configure(cfg.c_str());
  }

  void set_conflict_limit(unsigned limit) {
    solver_->limit("conflicts", static_cast<int>(limit));
  }

  void set_decision_limit(unsigned limit) {
    solver_->limit("decisions", static_cast<int>(limit));
  }

  void set_verbosity(int level) {
    if (level <= 0) {
      solver_->set("quiet", 1);
      solver_->set("verbose", 0);
    } else {
      solver_->set("quiet", 0);
      solver_->set("verbose", level);
    }
  }

  void set_random_seed(double seed) {
    solver_->set("seed", static_cast<int>(seed));
  }

  void set_terminate_callback(std::function<bool()> cb) {
    terminate_callback_ = std::move(cb);
    solver_->connect_terminator(&terminator_);
  }

  void print_statistics() const { solver_->statistics(); }

  CaDiCaL::Solver* raw_solver() const { return solver_.get(); }

 private:
  std::unique_ptr<CaDiCaL::Solver> solver_;
  int max_var_;
  std::function<bool()> terminate_callback_{};
  CadicalTerminatorAdapter terminator_;
};

}  // namespace velor
