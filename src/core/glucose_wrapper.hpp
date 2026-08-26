#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Glucose SAT Solver
#include "core/Solver.h"

namespace velor {

/**
 * @brief C++ wrapper for Glucose SAT solver
 *
 * This class provides a RAII wrapper around the Glucose solver,
 * making it easier to use in C++ code with automatic memory management.
 */
class GlucoseWrapper {
 public:
  enum class Result { SATISFIABLE, UNSATISFIABLE, UNKNOWN, INTERRUPTED };

  /**
   * @brief Constructor - initializes Glucose solver
   */
  GlucoseWrapper()
      : solver_(std::make_unique<Glucose::Solver>()), max_var_(0) {}

  /**
   * @brief Destructor
   */
  ~GlucoseWrapper() = default;

  // Delete copy constructor and assignment operator
  GlucoseWrapper(const GlucoseWrapper&) = delete;
  GlucoseWrapper& operator=(const GlucoseWrapper&) = delete;

  // Allow move constructor and assignment
  GlucoseWrapper(GlucoseWrapper&& other) noexcept
      : solver_(std::move(other.solver_)), max_var_(other.max_var_) {
    other.max_var_ = 0;
  }

  GlucoseWrapper& operator=(GlucoseWrapper&& other) noexcept {
    if (this != &other) {
      solver_ = std::move(other.solver_);
      max_var_ = other.max_var_;
      other.max_var_ = 0;
    }
    return *this;
  }

  /**
   * @brief Add a literal to the current clause
   */
  void add_literal(int lit) {
    int var = abs(lit);

    // Ensure the variable exists in the solver
    while (var > max_var_) {
      solver_->newVar();
      max_var_++;
    }

    // Convert to Glucose literal format
    // Glucose uses: var*2 for positive, var*2+1 for negative
    Glucose::Lit glucose_lit = Glucose::mkLit(var - 1, lit < 0);
    clause_buffer_.push(glucose_lit);
  }

  /**
   * @brief Finish the current clause
   */
  void finish_clause() {
    if (clause_buffer_.size() > 0) {
      solver_->addClause(clause_buffer_);
      clause_buffer_.clear();
    }
  }

  /**
   * @brief Add a complete clause
   */
  void add_clause(const std::vector<int>& clause) {
    for (int lit : clause) {
      add_literal(lit);
    }
    finish_clause();
  }

  /**
   * @brief Add a unit clause
   */
  void add_unit_clause(int lit) {
    add_literal(lit);
    finish_clause();
  }

  /**
   * @brief Reserve space for variables
   */
  void reserve(int max_var) {
    while (max_var_ < max_var) {
      solver_->newVar();
      max_var_++;
    }
  }

  /**
   * @brief Solve the SAT instance
   */
  Result solve() {
    // Fast path: no cooperative termination required.
    // Use Glucose native full solve once, avoiding repeated budgeted loops
    // that can keep generating learnt clauses and appear as memory leak.
    if (!terminate_callback_) {
      bool sat = solver_->solve();
      return sat ? Result::SATISFIABLE : Result::UNSATISFIABLE;
    }

    // Use a budgeted solve loop so we can be interrupted cooperatively by
    // a terminate callback (similar to how Kissat's callback is used).
    Glucose::vec<Glucose::Lit> empty_assumps;
    while (true) {
      // Set a small conflict budget so that we periodically return to the
      // caller and check the terminate callback.
      solver_->setConfBudget(10000);
      Glucose::lbool r = solver_->solveLimited(empty_assumps);
      // Clear the budget so subsequent full solves work as expected.
      solver_->budgetOff();

      if (r == Glucose::l_True) {
        return Result::SATISFIABLE;
      } else if (r == Glucose::l_False) {
        return Result::UNSATISFIABLE;
      } else { /* l_Undef */
        // Solver ran out of budget or was interrupted. If a terminate
        // callback is registered and returns true, treat this as an
        // interrupted run.
        if (terminate_callback_()) {
          // Request an interrupt and report INTERRUPTED to the caller.
          solver_->interrupt();
          return Result::INTERRUPTED;
        }
        // Otherwise continue the loop and give the solver more budget.
      }
    }
  }

  /**
   * @brief Get the value of a variable
   */
  std::optional<bool> get_value(int var) const {
    if (var < 1 || var > max_var_) {
      return std::nullopt;
    }

    Glucose::lbool val = solver_->modelValue(var - 1);
    if (val == Glucose::l_True) {
      return true;
    } else if (val == Glucose::l_False) {
      return false;
    }
    return std::nullopt;
  }

  /**
   * @brief Get the model
   */
  std::vector<std::optional<bool>> get_model(int max_var) const {
    std::vector<std::optional<bool>> model(max_var + 1);
    for (int i = 1; i <= max_var; ++i) {
      model[i] = get_value(i);
    }
    return model;
  }

  /**
   * @brief Get the maximum variable number
   */
  uint32_t get_max_variable() const { return max_var_; }

  uint32_t get_num_variables() const {
    return static_cast<uint32_t>(solver_->nVars());
  }

  uint64_t get_num_conflicts() const { return solver_->conflicts; }

  uint64_t get_num_propagations() const { return solver_->propagations; }

  /**
   * @brief Set verbosity
   */
  void set_verbosity(int level) { solver_->verbosity = level; }

  /**
   * @brief Set random seed for solver
   */
  void set_random_seed(double seed) { solver_->random_seed = seed; }

  /**
   * @brief Set a terminate callback. The solver will periodically call this
   * function during a budgeted solve and stop if it returns true.
   */
  void set_terminate_callback(std::function<bool()> cb) {
    terminate_callback_ = std::move(cb);
  }

  /**
   * @brief Print solver statistics
   */
  void print_statistics() const {
    // Glucose doesn't have a direct print_statistics method
    // You can access stats through solver_->stats if needed
  }

  /**
   * @brief Get the raw Glucose solver pointer
   */
  Glucose::Solver* raw_solver() const { return solver_.get(); }

 private:
  std::unique_ptr<Glucose::Solver> solver_;
  int max_var_;
  Glucose::vec<Glucose::Lit> clause_buffer_;
  std::function<bool()> terminate_callback_{};
};

}  // namespace velor
