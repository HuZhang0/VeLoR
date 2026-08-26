#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Kissat C API
extern "C" {
#include <kissat.h>
}

namespace velor {

/**
 * @brief C++ wrapper for Kissat SAT solver
 *
 * This class provides a RAII wrapper around the Kissat C API,
 * making it easier to use in C++ code with automatic memory management.
 */
class KissatWrapper {
 public:
  enum class Result {
    SATISFIABLE = 10,
    UNSATISFIABLE = 20,
    UNKNOWN = 0,
    INTERRUPTED = -1
  };

  /**
   * @brief Constructor - initializes Kissat solver
   */
  KissatWrapper()
      : solver_(kissat_init()), max_var_(0), terminate_callback_(nullptr) {
    if (!solver_) {
      throw std::runtime_error("Failed to initialize Kissat solver");
    }
  }

  /**
   * @brief Destructor - releases Kissat solver
   */
  ~KissatWrapper() {
    if (solver_) {
      kissat_release(solver_);
    }
  }

  // Delete copy constructor and assignment operator
  KissatWrapper(const KissatWrapper&) = delete;
  KissatWrapper& operator=(const KissatWrapper&) = delete;

  // Allow move constructor and assignment
  KissatWrapper(KissatWrapper&& other) noexcept
      : solver_(other.solver_),
        max_var_(other.max_var_),
        terminate_callback_(std::move(other.terminate_callback_)) {
    other.solver_ = nullptr;
    other.max_var_ = 0;
    other.terminate_callback_ = nullptr;
  }

  KissatWrapper& operator=(KissatWrapper&& other) noexcept {
    if (this != &other) {
      if (solver_) {
        kissat_release(solver_);
      }
      solver_ = other.solver_;
      max_var_ = other.max_var_;
      terminate_callback_ = std::move(other.terminate_callback_);
      other.solver_ = nullptr;
      other.max_var_ = 0;
      other.terminate_callback_ = nullptr;
    }
    return *this;
  }

  /**
   * @brief Add a literal to the current clause
   * @param lit Literal (positive for variable, negative for negated variable)
   */
  void add_literal(int lit) {
    kissat_add(solver_, lit);
    // Update max variable if necessary
    int var = abs(lit);
    if (var > max_var_) {
      max_var_ = var;
    }
  }

  /**
   * @brief Finish the current clause (add literal 0)
   */
  void finish_clause() { kissat_add(solver_, 0); }

  /**
   * @brief Add a complete clause at once
   * @param clause Vector of literals
   */
  void add_clause(const std::vector<int>& clause) {
    for (int lit : clause) {
      kissat_add(solver_, lit);
    }
    kissat_add(solver_, 0);
  }

  /**
   * @brief Add a unit clause (single literal)
   * @param lit The literal
   */
  void add_unit_clause(int lit) {
    kissat_add(solver_, lit);
    kissat_add(solver_, 0);
  }

  /**
   * @brief Set a termination callback
   * @param callback Function that returns true when solver should terminate
   */
  void set_terminate_callback(std::function<bool()> callback) {
    terminate_callback_ = std::make_unique<std::function<bool()>>(callback);

    // Set up the C-style callback for Kissat
    kissat_set_terminate(
        solver_, terminate_callback_.get(), [](void* state) -> int {
          auto* cb = static_cast<std::function<bool()>*>(state);
          return (*cb)() ? 1 : 0;
        });
  }

  /**
   * @brief Solve the SAT instance
   * @return Result of the solving process
   */
  Result solve() {
    int result = kissat_solve(solver_);
    switch (result) {
      case 10:
        return Result::SATISFIABLE;
      case 20:
        return Result::UNSATISFIABLE;
      case 0:
        // Check if it was interrupted
        if (terminate_callback_ && (*terminate_callback_)()) {
          return Result::INTERRUPTED;
        }
        return Result::UNKNOWN;
      default:
        return Result::UNKNOWN;
    }
  }

  /**
   * @brief Get the value of a variable in the satisfying assignment
   * @param var Variable number (1-indexed)
   * @return Value of the variable (true/false), or nullopt if unknown
   */
  std::optional<bool> get_value(int var) const {
    int val = kissat_value(solver_, var);
    if (val == var) return true;
    if (val == -var) return false;
    return std::nullopt;
  }

  /**
   * @brief Get the satisfying assignment for all variables up to max_var
   * @param max_var Maximum variable number
   * @return Vector of boolean values (index 0 is unused, variables start at
   * index 1)
   */
  std::vector<std::optional<bool>> get_model(int max_var) const {
    std::vector<std::optional<bool>> model(max_var + 1);
    for (int i = 1; i <= max_var; ++i) {
      model[i] = get_value(i);
    }
    return model;
  }

  /**
   * @brief Reserve space for variables (optimization)
   * @param max_var Maximum variable number expected
   */
  void reserve(int max_var) { kissat_reserve(solver_, max_var); }

  /**
   * @brief Get the maximum variable number that has been added
   * @return Maximum variable number
   */
  uint32_t get_max_variable() const { return max_var_; }

  /**
   * @brief Set conflict limit for solving
   * @param limit Maximum number of conflicts
   */
  void set_conflict_limit(unsigned limit) {
    kissat_set_conflict_limit(solver_, limit);
  }

  /**
   * @brief Set decision limit for solving
   * @param limit Maximum number of decisions
   */
  void set_decision_limit(unsigned limit) {
    kissat_set_decision_limit(solver_, limit);
  }

  /**
   * @brief Set a configuration
   * @param name Configuration name
   * @return true if successful
   */
  bool set_configuration(const std::string& name) {
    return kissat_set_configuration(solver_, name.c_str()) != 0;
  }

  /**
   * @brief Set an option value
   * @param name Option name
   * @param value Option value
   * @return true if successful
   */
  bool set_option(const std::string& name, int value) {
    return kissat_set_option(solver_, name.c_str(), value) != 0;
  }

  /**
   * @brief Get an option value
   * @param name Option name
   * @return Option value
   */
  int get_option(const std::string& name) const {
    return kissat_get_option(solver_, name.c_str());
  }

  /**
   * @brief Print solver statistics
   */
  void print_statistics() const { kissat_print_statistics(solver_); }

  /**
   * @brief Get the raw Kissat solver pointer (use with caution)
   * @return Raw solver pointer
   */
  kissat* raw_solver() const { return solver_; }

 private:
  kissat* solver_;
  int max_var_;
  std::unique_ptr<std::function<bool()>> terminate_callback_;
};

}  // namespace velor
