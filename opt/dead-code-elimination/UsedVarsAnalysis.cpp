/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "UsedVarsAnalysis.h"

#include "ReachableClasses.h"

namespace ptrs = local_pointers;

/*
 * This returns all the pointer-bearing registers whose pointees :insn will
 * read from.
 */
std::vector<uint16_t> object_read_registers(const IRInstruction* insn) {
  switch (insn->opcode()) {
  case OPCODE_AGET:
  case OPCODE_AGET_WIDE:
  case OPCODE_AGET_BOOLEAN:
  case OPCODE_AGET_BYTE:
  case OPCODE_AGET_CHAR:
  case OPCODE_AGET_SHORT:
  case OPCODE_AGET_OBJECT:
  case OPCODE_IGET:
  case OPCODE_IGET_WIDE:
  case OPCODE_IGET_BOOLEAN:
  case OPCODE_IGET_BYTE:
  case OPCODE_IGET_CHAR:
  case OPCODE_IGET_SHORT:
  case OPCODE_IGET_OBJECT:

  case OPCODE_APUT_OBJECT:
  case OPCODE_IPUT_OBJECT:
  case OPCODE_SPUT_OBJECT:

  case OPCODE_RETURN_OBJECT:
    return {insn->src(0)};

  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_INTERFACE: {
    std::vector<uint16_t> regs;
    size_t idx{0};
    if (insn->opcode() != OPCODE_INVOKE_STATIC) {
      // The `this` parameter
      regs.emplace_back(insn->src(idx++));
    }
    auto callee = insn->get_method();
    auto arg_types = callee->get_proto()->get_args()->get_type_list();
    for (DexType* arg_type : arg_types) {
      if (!is_primitive(arg_type)) {
        regs.emplace_back(insn->src(idx));
      }
      ++idx;
    }
    return regs;
  }
  default:
    return {};
  }
}

/*
 * Record the environment before the execution of every instruction. We need
 * this data during the backwards used vars analysis.
 */
static std::unordered_map<const IRInstruction*, ptrs::Environment>
gen_instruction_environment_map(const cfg::ControlFlowGraph& cfg,
                                const ptrs::FixpointIterator& fp_iter) {
  std::unordered_map<const IRInstruction*, ptrs::Environment> result;
  for (auto* block : cfg.blocks()) {
    auto env = fp_iter.get_entry_state_at(block);
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      result.emplace(insn, env);
      fp_iter.analyze_instruction(insn, &env);
    }
  }
  return result;
}

// Print the subset of env that insn references. (Printing out the entire env
// at every instruction makes logging too costly.)
std::string show_subset(const ptrs::Environment& env,
                        const IRInstruction* insn) {
  std::ostringstream o;
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    auto src = insn->src(i);
    const auto& pointers = env.get_pointers(src);
    o << pointers;
    if (!pointers.is_value()) {
      continue;
    }
    o << "(";
    bool first = true;
    for (auto* pointer : pointers.elements()) {
      if (!first) {
        o << ", ";
      }
      o << env.get_pointee(pointer);
      first = false;
    }
    o << ")";
  }
  return o.str();
}

UsedVarsFixpointIterator::UsedVarsFixpointIterator(
    const local_pointers::FixpointIterator& pointers_fp_iter,
    const EffectSummaryMap& effect_summaries,
    const std::unordered_set<const DexMethod*>& non_overridden_virtuals,
    const cfg::ControlFlowGraph& cfg)
    : MonotonicFixpointIterator(cfg, cfg.blocks().size()),
      m_insn_env_map(gen_instruction_environment_map(cfg, pointers_fp_iter)),
      m_effect_summaries(effect_summaries),
      m_non_overridden_virtuals(non_overridden_virtuals) {}

void UsedVarsFixpointIterator::analyze_instruction(
    const IRInstruction* insn, UsedVarsSet* used_vars) const {
  TRACE(DEAD_CODE, 5, "Before %s : %s : %s\n", SHOW(insn), SHOW(*used_vars),
        show_subset(m_insn_env_map.at(insn), insn).c_str());
  bool required = is_required(insn, *used_vars);
  auto op = insn->opcode();
  if (ptrs::is_alloc_opcode(op)) {
    used_vars->remove(insn);
  }
  if (insn->dests_size()) {
    used_vars->remove(insn->dest());
  } else if (insn->has_move_result()) {
    used_vars->remove(RESULT_REGISTER);
  }
  if (required) {
    const auto& env = m_insn_env_map.at(insn);
    if (env.is_bottom()) {
      return;
    }
    for (auto reg : object_read_registers(insn)) {
      auto pointers = env.get_pointers(reg);
      // XXX: We should never encounter this case since we explicitly bind all
      // potential pointer-containing registers to non-Top values in our
      // environment. If we did encounter Top here, however, we should treat
      // all local allocations as potentially used -- a read from
      // PointerSet::top() must be treated like a read from every possible
      // heap location.
      always_assert(!pointers.is_top());
      for (auto pointer : pointers.elements()) {
        used_vars->add(pointer);
      }
    }
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      used_vars->add(insn->src(i));
    }
    if (is_move_result(op) || opcode::is_move_result_pseudo(op)) {
      used_vars->add(RESULT_REGISTER);
    }
  }
  TRACE(DEAD_CODE, 5, "After: %s\n", SHOW(*used_vars));
}

bool UsedVarsFixpointIterator::is_used_or_escaping_write(
    const ptrs::Environment& env,
    const UsedVarsSet& used_vars,
    reg_t obj_reg) const {
  auto pointers = env.get_pointers(obj_reg);
  if (!pointers.is_value()) {
    return true;
  }
  const auto& heap = env.get_heap();
  for (auto pointer : pointers.elements()) {
    if (used_vars.contains(pointer)) {
      return true;
    }
    if (heap.get(pointer).equals(EscapeDomain(EscapeState::MAY_ESCAPE))) {
      return true;
    }
  }
  return false;
}

bool UsedVarsFixpointIterator::is_required(const IRInstruction* insn,
                                           const UsedVarsSet& used_vars) const {
  auto op = insn->opcode();
  switch (op) {
  case IOPCODE_LOAD_PARAM:
  case IOPCODE_LOAD_PARAM_OBJECT:
  case IOPCODE_LOAD_PARAM_WIDE:
  // Control-flow opcodes are always required.
  case OPCODE_RETURN_VOID:
  case OPCODE_RETURN:
  case OPCODE_RETURN_WIDE:
  case OPCODE_RETURN_OBJECT:
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_CHECK_CAST:
  case OPCODE_THROW:
  case OPCODE_GOTO:
  case OPCODE_PACKED_SWITCH:
  case OPCODE_SPARSE_SWITCH:
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE:
  case OPCODE_IF_LT:
  case OPCODE_IF_GE:
  case OPCODE_IF_GT:
  case OPCODE_IF_LE:
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ:
  case OPCODE_IF_LTZ:
  case OPCODE_IF_GEZ:
  case OPCODE_IF_GTZ:
  case OPCODE_IF_LEZ: {
    return true;
  }
  case OPCODE_APUT:
  case OPCODE_APUT_WIDE:
  case OPCODE_APUT_OBJECT:
  case OPCODE_APUT_BOOLEAN:
  case OPCODE_APUT_BYTE:
  case OPCODE_APUT_CHAR:
  case OPCODE_APUT_SHORT:
  case OPCODE_IPUT:
  case OPCODE_IPUT_WIDE:
  case OPCODE_IPUT_OBJECT:
  case OPCODE_IPUT_BOOLEAN:
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_SHORT: {
    const auto& env = m_insn_env_map.at(insn);
    return is_used_or_escaping_write(env, used_vars, insn->src(1));
  }
  case OPCODE_FILL_ARRAY_DATA: {
    const auto& env = m_insn_env_map.at(insn);
    return is_used_or_escaping_write(env, used_vars, insn->src(0));
  }
  case OPCODE_SPUT:
  case OPCODE_SPUT_WIDE:
  case OPCODE_SPUT_OBJECT:
  case OPCODE_SPUT_BOOLEAN:
  case OPCODE_SPUT_BYTE:
  case OPCODE_SPUT_CHAR:
  case OPCODE_SPUT_SHORT: {
    return true;
  }
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_VIRTUAL: {
    auto method = resolve_method(insn->get_method(), opcode_to_search(insn));
    if (method == nullptr) {
      return true;
    }
    if (assumenosideeffects(method)) {
      return used_vars.contains(RESULT_REGISTER);
    }
    // We could be smarter about virtual calls that may resolve to one of many
    // callees, but right now we just bail out.
    if (op == OPCODE_INVOKE_VIRTUAL &&
        !m_non_overridden_virtuals.count(method)) {
      return true;
    }
    if (!m_effect_summaries.count(method)) {
      return true;
    }
    // A call is required if it has a side-effect, if its return value is used,
    // or if it mutates an argument that may later be read somewhere up the
    // callstack.
    auto& summary = m_effect_summaries.at(method);
    if (summary.effects != EFF_NONE || used_vars.contains(RESULT_REGISTER)) {
      return true;
    }
    const auto& env = m_insn_env_map.at(insn);
    const auto& mod_params = summary.modified_params;
    return std::any_of(
        mod_params.begin(), mod_params.end(), [&](param_idx_t idx) {
          return is_used_or_escaping_write(env, used_vars, insn->src(idx));
        });
  }
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_INTERFACE: {
    return true;
  }
  default: {
    if (insn->dests_size()) {
      return used_vars.contains(insn->dest());
    } else if (insn->has_move_result()) {
      return used_vars.contains(RESULT_REGISTER);
    }
    return true;
  }
  }
}

std::vector<IRList::iterator> get_dead_instructions(
    const IRCode* const_code, const UsedVarsFixpointIterator& fp_iter) {
  // We aren't mutating the IRCode object, but we want to return non-const
  // IRList::iterators.
  auto* code = const_cast<IRCode*>(const_code);
  auto& cfg = code->cfg();
  std::vector<IRList::iterator> dead_instructions;
  for (auto* block : cfg.blocks()) {
    auto used_vars = fp_iter.get_used_vars_at_exit(block);
    TRACE(DEAD_CODE, 5, "B%u exit : %s\n", block->id(), SHOW(used_vars));
    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      if (it->type != MFLOW_OPCODE) {
        continue;
      }
      auto* insn = it->insn;
      if (!fp_iter.is_required(insn, used_vars)) {
        // move-result-pseudo instructions will be automatically removed
        // when their primary instruction is deleted.
        if (!opcode::is_move_result_pseudo(insn->opcode())) {
          dead_instructions.emplace_back(code->iterator_to(*it));
        }
      }
      fp_iter.analyze_instruction(insn, &used_vars);
    }
    TRACE(DEAD_CODE, 5, "B%u entry : %s\n", block->id(),
          SHOW(fp_iter.get_used_vars_at_entry(block)));
  }
  return dead_instructions;
}
