#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Some third-party headers (e.g., bill/maple) predefine MiniSat include-guard
// macros, which can accidentally suppress the real MiniSat headers.
#ifdef Minisat_IntTypes_h
#undef Minisat_IntTypes_h
#endif
#ifdef Minisat_XAlloc_h
#undef Minisat_XAlloc_h
#endif
#ifdef Minisat_Vec_h
#undef Minisat_Vec_h
#endif
#ifdef Minisat_Alg_h
#undef Minisat_Alg_h
#endif
#ifdef Minisat_Alloc_h
#undef Minisat_Alloc_h
#endif
#ifdef Minisat_Heap_h
#undef Minisat_Heap_h
#endif
#ifdef Minisat_Map_h
#undef Minisat_Map_h
#endif
#ifdef Minisat_Queue_h
#undef Minisat_Queue_h
#endif
#ifdef Minisat_Sort_h
#undef Minisat_Sort_h
#endif
#ifdef Minisat_ParseUtils_h
#undef Minisat_ParseUtils_h
#endif
#ifdef Minisat_Options_h
#undef Minisat_Options_h
#endif
#ifdef Minisat_System_h
#undef Minisat_System_h
#endif
#ifdef Minisat_SolverTypes_h
#undef Minisat_SolverTypes_h
#endif
#ifdef Minisat_IntMap_h
#undef Minisat_IntMap_h
#endif
#ifdef Minisat_Solver_h
#undef Minisat_Solver_h
#endif

// ABC SAT headers may define this macro and break MiniSat identifiers.
#ifdef var_Undef
#undef var_Undef
#endif

#include "minisat/core/Solver.h"

namespace velor {

class MinisatWrapper {
 public:
  enum class Result { SATISFIABLE, UNSATISFIABLE, UNKNOWN, INTERRUPTED };

  MinisatWrapper() : solver_(std::make_unique<Minisat::Solver>()), max_var_(0) {}
  ~MinisatWrapper() = default;

  MinisatWrapper(const MinisatWrapper&) = delete;
  MinisatWrapper& operator=(const MinisatWrapper&) = delete;

  MinisatWrapper(MinisatWrapper&& other) noexcept
      : solver_(std::move(other.solver_)), max_var_(other.max_var_) {
    other.max_var_ = 0;
  }

  MinisatWrapper& operator=(MinisatWrapper&& other) noexcept {
    if (this != &other) {
      solver_ = std::move(other.solver_);
      max_var_ = other.max_var_;
      terminate_callback_ = std::move(other.terminate_callback_);
      other.max_var_ = 0;
    }
    return *this;
  }

  void add_literal(int lit) {
    int var = lit < 0 ? -lit : lit;

    while (var > max_var_) {
      solver_->newVar();
      max_var_++;
    }

    Minisat::Lit minisat_lit = Minisat::mkLit(var - 1, lit < 0);
    clause_buffer_.push(minisat_lit);
  }

  void finish_clause() {
    if (clause_buffer_.size() > 0) {
      solver_->addClause(clause_buffer_);
      clause_buffer_.clear();
    }
  }

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
    while (max_var_ < max_var) {
      solver_->newVar();
      max_var_++;
    }
  }

  Result solve() {
    Minisat::vec<Minisat::Lit> empty_assumps;
    while (true) {
      solver_->setConfBudget(10000);
      Minisat::lbool r = solver_->solveLimited(empty_assumps);
      solver_->budgetOff();

      if (r == Minisat::l_True) {
        return Result::SATISFIABLE;
      } else if (r == Minisat::l_False) {
        return Result::UNSATISFIABLE;
      } else {
        if (terminate_callback_ && terminate_callback_()) {
          solver_->interrupt();
          return Result::INTERRUPTED;
        }
      }
    }
  }

  std::optional<bool> get_value(int var) const {
    if (var < 1 || var > max_var_) {
      return std::nullopt;
    }

    Minisat::lbool val = solver_->modelValue(var - 1);
    if (val == Minisat::l_True) {
      return true;
    } else if (val == Minisat::l_False) {
      return false;
    }
    return std::nullopt;
  }

  std::vector<std::optional<bool>> get_model(int max_var) const {
    std::vector<std::optional<bool>> model(max_var + 1);
    for (int i = 1; i <= max_var; ++i) {
      model[i] = get_value(i);
    }
    return model;
  }

  uint32_t get_max_variable() const { return max_var_; }

  uint32_t get_num_variables() const {
    return static_cast<uint32_t>(solver_->nVars());
  }

  uint64_t get_num_conflicts() const { return solver_->conflicts; }

  uint64_t get_num_propagations() const { return solver_->propagations; }

  void set_verbosity(int level) { solver_->verbosity = level; }

  void set_random_seed(double seed) { solver_->random_seed = seed; }

  void set_terminate_callback(std::function<bool()> cb) {
    terminate_callback_ = std::move(cb);
  }

  void print_statistics() const {}

  Minisat::Solver* raw_solver() const { return solver_.get(); }

 private:
  std::unique_ptr<Minisat::Solver> solver_;
  int max_var_;
  Minisat::vec<Minisat::Lit> clause_buffer_;
  std::function<bool()> terminate_callback_{};
};

}  // namespace velor
