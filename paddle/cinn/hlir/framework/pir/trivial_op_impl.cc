// Copyright (c) 2024 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/cinn/hlir/framework/pir/trivial_op_impl.h"
#include <variant>
#include "paddle/cinn/operator_fusion/cluster_interface.h"
#include "paddle/cinn/operator_fusion/fusion_tracker/expr_utils.h"
#include "paddle/cinn/operator_fusion/pattern.h"

#include "paddle/cinn/hlir/dialect/operator/ir/manual_op.h"
#include "paddle/cinn/hlir/framework/compile_error.h"
#include "paddle/cinn/hlir/framework/pir/op_lowering_util.h"
#include "paddle/cinn/hlir/framework/pir/utils.h"
#include "paddle/cinn/hlir/pe/map_expr_to_ir.h"
#include "paddle/cinn/ir/dim.h"
#include "paddle/cinn/ir/group_schedule/base_group_scheduler.h"
#include "paddle/cinn/ir/schedule/ir_schedule.h"
#include "paddle/cinn/ir/schedule/ir_schedule_util.h"
#include "paddle/cinn/lang/placeholder.h"
#include "paddle/cinn/optim/schedule_block_dce.h"
#include "paddle/cinn/optim/transform_gpu_forloop.h"
#include "paddle/common/ddim.h"
#include "paddle/common/enforce.h"
#include "paddle/fluid/pir/dialect/operator/ir/op_type.h"
#include "paddle/pir/include/dialect/control_flow/ir/cf_op.h"

PD_DECLARE_bool(group_schedule_tiling_first);

namespace cinn {
namespace hlir {
namespace framework {
namespace pir {
namespace trivial_fusion_detail {

TrivialOp::TrivialOp(const ir::Expr& origin_func_body) {
  func_body = ir::ir_utils::IRCopy(origin_func_body);
}

TrivialOp::TrivialOp(const TrivialOp& trivial_op) {
  func_body = trivial_op.GetFuncBody();
}

void TrivialOp::_SetFuncBody(ir::Expr new_body) { func_body = new_body; }

ir::Expr* TrivialOp::_GetFuncBodyPointer() { return &func_body; }

ir::Expr TrivialOp::GetFuncBody() const { return func_body; }

ReduceOp::ReduceOp(const ir::Expr& origin_func_body) {
  func_body = ir::ir_utils::IRCopy(origin_func_body);
}

ReduceOp::ReduceOp(const ReduceOp& reduce_op) {
  func_body = reduce_op.GetFuncBody();
}

void ReduceOp::_SetFuncBody(ir::Expr new_body) { func_body = new_body; }

ir::Expr ReduceOp::GetFuncBody() const { return func_body; }

ir::Expr* ReduceOp::_GetFuncBodyPointer() { return &func_body; }

using FusibleOp = std::variant<ReduceOp, TrivialOp>;

ir::Expr _GetRootExpr(const FusibleOp& op) {
  return std::visit([](auto&& arg) { return arg.GetFuncBody(); }, op);
}

void _SetFuncBody(FusibleOp& op, ir::Expr new_body) {  // NOLINT
  std::visit([&](auto&& arg) { arg._SetFuncBody(new_body); }, op);
}

ir::Expr GetComputeBody(const FusibleOp& op) {
  struct Visitor {
    ir::Expr operator()(const ReduceOp& op) {
      const auto& compute_realize =
          (ExprSetFinderUtils::ChildScheduleBlockRealizes *
           ExprSetFinderUtils::ScheduleBlockRealizeIsNotInit)
              .GetSingle(_GetRootExpr(op));
      const auto& compute_body =
          (ExprSetFinderUtils::ChildStores * ExprSetFinderUtils::Store2Value)
              .GetSingle(compute_realize);
      return ExprTransformerUtils::SubstitudeByScheduleBlockRealize(
          compute_realize)(compute_body);
    }
    ir::Expr operator()(const TrivialOp& op) {
      const auto& compute_realize =
          (ExprSetFinderUtils::ChildScheduleBlockRealizes)
              .GetSingle(_GetRootExpr(op));
      const auto& compute_body =
          (ExprSetFinderUtils::ChildStores * ExprSetFinderUtils::Store2Value)
              .GetSingle(compute_realize);
      return ExprTransformerUtils::SubstitudeByScheduleBlockRealize(
          compute_realize)(compute_body);
    }
  };
  VLOG(4) << "GetComputeBody";
  return std::visit(Visitor(), op);
}

ir::Tensor GetOutputTensor(const FusibleOp& op) {
  struct Visitor {
    ir::Tensor operator()(const ReduceOp& op) {
      const auto& compute_body =
          (ExprSetFinderUtils::ChildScheduleBlockRealizes *
           ExprSetFinderUtils::ScheduleBlockRealizeIsNotInit *
           ExprSetFinderUtils::ChildStores)
              .GetSingle(_GetRootExpr(op));
      return compute_body.As<ir::Store>()->tensor.as_tensor_ref();
    }
    ir::Tensor operator()(const TrivialOp& op) {
      const auto& compute_body =
          (ExprSetFinderUtils::ChildScheduleBlockRealizes *
           ExprSetFinderUtils::ChildStores)
              .GetSingle(_GetRootExpr(op));
      return compute_body.As<ir::Store>()->tensor.as_tensor_ref();
    }
  };
  VLOG(4) << "GetOutputTensor";
  return std::visit(Visitor(), op);
}

std::vector<ir::Var> AppendBound(const std::vector<ir::Var> vars,
                                 const ir::Expr& root) {
  return ExprSetFinderUtils::MapVector<ir::Var>(
      vars, [&](const auto& v) -> ir::Var {
        VLOG(4) << "Start Append Bound for " << v;
        VLOG(4) << "AppendBound for " << v << ", lower: "
                << (ExprSetFinderUtils::ChildFors *
                    ExprSetFinderUtils::IsForIterVar(v) *
                    ExprSetFinderUtils::For2Min)
                       .GetSingle(root)
                << ", upper: "
                << (ExprSetFinderUtils::ChildFors *
                    ExprSetFinderUtils::IsForIterVar(v) *
                    ExprSetFinderUtils::For2Max)
                       .GetSingle(root);
        return ir::Var(
            (ExprSetFinderUtils::ChildFors *
             ExprSetFinderUtils::IsForIterVar(v) * ExprSetFinderUtils::For2Min)
                .GetSingle(root),
            (ExprSetFinderUtils::ChildFors *
             ExprSetFinderUtils::IsForIterVar(v) * ExprSetFinderUtils::For2Max)
                .GetSingle(root),
            v->name,
            v->is_reduce_axis);
      });
}

std::vector<ir::Var> GetOutputIters(const FusibleOp& op) {
  struct Visitor {
    std::vector<ir::Var> operator()(const ReduceOp& op) {
      ir::Expr init_block_realize =
          (ExprSetFinderUtils::ChildScheduleBlockRealizes *
           ExprSetFinderUtils::ScheduleBlockRealizeIsInit)
              .GetSingle(_GetRootExpr(op));
      const std::vector<Expr>& outer_iter_expr =
          init_block_realize.As<ir::ScheduleBlockRealize>()->iter_values;
      return trivial_fusion_detail::ComposeUtils::ExprVec2VarVec(
          outer_iter_expr);
    }
    std::vector<ir::Var> operator()(const TrivialOp& op) {
      const auto& compute_realize =
          (ExprSetFinderUtils::ChildScheduleBlockRealizes)
              .GetSingle(_GetRootExpr(op));
      const std::vector<Expr>& outer_iter_expr =
          compute_realize.As<ir::ScheduleBlockRealize>()->iter_values;
      return trivial_fusion_detail::ComposeUtils::ExprVec2VarVec(
          outer_iter_expr);
    }
  };
  VLOG(4) << "GetOutputIters";
  VLOG(4) << "Before AppendBound:" << _GetRootExpr(op);
  return AppendBound(std::visit(Visitor(), op), _GetRootExpr(op));
}

std::vector<ir::Var> GetAllIterVars(const ir::Expr& expr) {
  ir::Expr compute_schedule_block_realize =
      (ExprSetFinderUtils::ChildScheduleBlockRealizes *
       ExprSetFinderUtils::ScheduleBlockRealizeIsNotInit)
          .GetSingle(expr);

  const std::vector<Expr>& all_iter_expr =
      compute_schedule_block_realize.As<ir::ScheduleBlockRealize>()
          ->iter_values;
  return ComposeUtils::ExprVec2VarVec(all_iter_expr);
}

std::vector<ir::Var> GetReduceIters(const ReduceOp& op) {
  auto GetUnorderedAllIterVars = [](const ReduceOp& op) {
    return GetAllIterVars(_GetRootExpr(op));
  };

  // Iter Vars not appearing in outer_iter_vars are pushed into
  // reduce_iter_vars
  std::vector<ir::Var> all_iter_vars = GetUnorderedAllIterVars(op);
  std::vector<ir::Var> outer_iter_vars = GetOutputIters(op);
  std::vector<ir::Var> reduce_iter_vars;

  for (auto& iter_var : all_iter_vars) {
    if (!(std::find(outer_iter_vars.begin(), outer_iter_vars.end(), iter_var) !=
          outer_iter_vars.end())) {
      iter_var->is_reduce_axis = true;
      reduce_iter_vars.push_back(iter_var);
    }
  }
  VLOG(4) << "GetReduceIters";
  return AppendBound(reduce_iter_vars, _GetRootExpr(op));
}

std::vector<int> GetExpandVarPos(const ReduceOp& op) {
  std::vector<ir::Var> all_iter_vars = GetAllForIters(_GetRootExpr(op));
  VLOG(4) << "all_iter_vars: " << cinn::utils::Join(all_iter_vars, ", ");
  std::vector<int> expand_pos;

  for (int i = 0; i < all_iter_vars.size(); i++) {
    if (all_iter_vars[i]->name.find("expand_var") == 0) {
      expand_pos.push_back(i);
    }
  }
  return expand_pos;
}

ir::Expr GetInitExpr(const ReduceOp& op) {
  const auto result =
      (ExprSetFinderUtils::ChildScheduleBlockRealizes *
       ExprSetFinderUtils::ScheduleBlockRealizeIsInit *
       ExprSetFinderUtils::ChildStores * ExprSetFinderUtils::Store2Value)
          .GetSingle(op.GetFuncBody());
  VLOG(4) << "GetInitExpr: " << result;
  return result;
}

ir::Expr* _GetFuncBodyPointer(FusibleOp op) {
  return std::visit([&](auto&& arg) { return arg._GetFuncBodyPointer(); }, op);
}

ir::Expr CopyReduceBody(const FusibleOp& downstream, const ReduceOp& upstream) {
  struct Visitor {
    ir::Expr operator()(const ReduceOp& op) {
      return ir::ir_utils::IRCopy(op.GetFuncBody());
    }
    ir::Expr operator()(const TrivialOp& op) {
      PADDLE_THROW("TrivialOp cannot be copied.");
    }
  };
  return std::visit(Visitor(), downstream);
}

ir::Expr CreateReduceExpr(
    const std::vector<ir::Var>& output_iters,
    const std::vector<ir::Var>& reduce_iters,
    const ir::Expr& init_body,    // relay on output_iters
    const ir::Expr& reduce_body,  // relay on output_iters + reduce_iters
    const ir::Tensor& new_write_tensor,
    const ir::Tensor& origin_write_tensor) {
  VLOG(4) << "CreateReduceExpr Start.";
  const std::vector<ir::Expr> indice_expr(output_iters.begin(),
                                          output_iters.end());
  auto new_init_tensor = ir::Tensor(new_write_tensor->name + "__reduce_init",
                                    new_write_tensor->type(),
                                    new_write_tensor->shape,
                                    new_write_tensor->domain,
                                    new_write_tensor->operation,
                                    reduce_iters);
  new_init_tensor->WithBuffer();

  const auto& init_schedule_block =
      (ExprTransformerUtils::WrapStoreTransformer(new_init_tensor,
                                                  indice_expr) *
       ExprTransformerUtils::WrapScheduleRealizer(
           output_iters, new_init_tensor->name))(init_body);

  const auto& reduce_schedule_block =
      (ExprTransformerUtils::ChangeTensorLoadTransformer(
           origin_write_tensor, new_write_tensor(indice_expr)) *
       ExprTransformerUtils::WrapStoreTransformer(new_write_tensor,
                                                  indice_expr) *
       ExprTransformerUtils::WrapScheduleRealizer(
           ComposeUtils::ConcatVector(output_iters, reduce_iters),
           new_write_tensor->name) *
       ExprTransformerUtils::WrapForsTransformer(reduce_iters))(reduce_body);

  const auto& gather_body = ir::Block::Make(
      std::vector<ir::Expr>({init_schedule_block, reduce_schedule_block}));
  return ir::Block::Make(
      {(ExprTransformerUtils::WrapForsTransformer(output_iters) *
        ExprTransformerUtils::WrapScheduleRealizer({}, "root"))(gather_body)});
}

ir::Expr CreateTrivialExpr(const std::vector<ir::Var>& output_iters,
                           const ir::Expr& function_body,
                           const ir::Tensor& new_write_tensor) {
  const auto& RemoveReduceAxisFromVar =
      [](const std::vector<ir::Var>& vars) -> std::vector<ir::Var> {
    std::vector<ir::Var> result;
    for (auto& var : vars) {
      auto new_var = ir::ir_utils::IRCopy(var).as_var_ref();
      new_var->is_reduce_axis = false;
      result.push_back(new_var);
    }
    return result;
  };
  auto trivial_iters = RemoveReduceAxisFromVar(output_iters);
  const std::vector<ir::Expr> indice_expr =
      std::vector<ir::Expr>(trivial_iters.begin(), trivial_iters.end());
  const auto& compute_body_schedule_block =
      (ExprTransformerUtils::WrapStoreTransformer(new_write_tensor,
                                                  indice_expr) *
       ExprTransformerUtils::WrapScheduleRealizer(
           trivial_iters, new_write_tensor->name))(function_body);
  return ir::Block::Make(
      {(ExprTransformerUtils::WrapForsTransformer(trivial_iters) *
        ExprTransformerUtils::WrapScheduleRealizer({}, "root"))(
          ir::Block::Make({compute_body_schedule_block}))});
}

ir::Expr CreateExprWithNewComputeBody(const FusibleOp& fusible_op,
                                      const ir::Expr& new_compute_body) {
  struct Visitor {
    ir::Expr operator()(const ReduceOp& op) {
      return CreateReduceExpr(GetOutputIters(op),
                              GetReduceIters(op),
                              GetInitExpr(op),
                              compute_body_,
                              GetOutputTensor(op),
                              GetOutputTensor(op));
    }
    ir::Expr operator()(const TrivialOp& op) {
      return CreateTrivialExpr(
          GetOutputIters(op), compute_body_, GetOutputTensor(op));
    }

    ir::Expr compute_body_;
    explicit Visitor(ir::Expr compute_body) { compute_body_ = compute_body; }
  };
  VLOG(4) << "CreateExprWithNewComputeBody";
  return std::visit(Visitor(new_compute_body), fusible_op);
}

int GetTensorCounter() {
  static int counter = 1;
  return counter++;
}

std::vector<FusibleOp> TransformReduceLoopRange(
    const ReduceOp& upstream,
    FusibleOp* downstream,
    const std::vector<size_t>& fake_reduce_iter_idx) {
  // downstream will be mutated by this transform.
  VLOG(4) << "RRTransform begin";
  VLOG(4) << "RRTransform Upstream is \n" << _GetRootExpr(upstream);
  VLOG(4) << "RRTransform Downstream is \n" << _GetRootExpr(*downstream);
  ir::Expr modified_downstream_compute_body = GetComputeBody(*downstream);
  const auto& load_upstream_expr = ComposeUtils::GetEachTensorLoadExpr(
      modified_downstream_compute_body, GetOutputTensor(upstream));
  std::vector<FusibleOp> results;
  ir::Tensor downstream_output_tensor = GetOutputTensor(*downstream);

  bool is_trivial_downstream = std::holds_alternative<TrivialOp>(*downstream);

  const auto create_new_tensor = [&](const ir::Tensor& downstream_load_tensor) {
    VLOG(4) << "Create New Tensor Start";
    const auto shape =
        is_trivial_downstream
            ? FilterWithFakeReduceIter(downstream_output_tensor->shape,
                                       fake_reduce_iter_idx)
            : downstream_output_tensor->shape;
    ir::Tensor result = ir::Tensor(
        downstream_load_tensor->name + "_loopalign_" +
            std::to_string(GetTensorCounter()),
        downstream_load_tensor->type(),
        shape,
        is_trivial_downstream
            ? FilterWithFakeReduceIter(downstream_output_tensor->domain,
                                       fake_reduce_iter_idx)
            : downstream_output_tensor->domain,
        GetOutputTensor(upstream)->operation,
        GetReduceIters(upstream));
    result->WithBuffer();
    VLOG(4) << "Create New Tensor Result: " << result;
    return result;
  };

  for (const auto& load_tensor : load_upstream_expr) {
    const auto& new_tensor =
        create_new_tensor(load_tensor.As<ir::Load>()->tensor.as_tensor_ref());
    ir::Expr new_reduce = CreateReduceExpr(
        is_trivial_downstream
            ? FilterWithFakeReduceIter(GetOutputIters(*downstream),
                                       fake_reduce_iter_idx)
            : GetOutputIters(*downstream),
        GetReduceIters(upstream),
        GetInitExpr(upstream),
        ComposeUtils::CopyedReplaceExpr(GetComputeBody(upstream),
                                        GetOutputIters(upstream),
                                        load_tensor.As<ir::Load>()->indices),
        new_tensor,
        GetOutputTensor(upstream));
    results.emplace_back(ReduceOp(new_reduce));
    VLOG(4) << "After Tmp Transform, upstream is : \n"
            << _GetRootExpr(results.back());
    ExprTransformerUtils::ReplaceTarget(
        &modified_downstream_compute_body,
        load_tensor,
        new_tensor(ComposeUtils::VarVec2ExprVec(
            is_trivial_downstream
                ? FilterWithFakeReduceIter(GetOutputIters(*downstream),
                                           fake_reduce_iter_idx)
                : GetOutputIters(*downstream))));
  }
  _SetFuncBody(*downstream,
               CreateExprWithNewComputeBody(*downstream,
                                            modified_downstream_compute_body));
  VLOG(4) << "RRTransform After Replace Downstream Load: \n"
          << _GetRootExpr(*downstream);
  return results;
}

FusibleOp SinkTrivialLoopAlign(TrivialOp trivial_op,
                               ReduceOp reduce_op,
                               std::vector<size_t> fake_reduce_iter_idx) {
  VLOG(4) << "SinkTrivialLoopAlign";
  ir::Expr new_trivial_body = ir::ir_utils::IRCopy(trivial_op.GetFuncBody());
  std::vector<ir::Var> all_out_iter_vars = GetOutputIters(trivial_op);
  std::vector<ir::Var> non_reduce_iter_vars =
      FilterWithFakeReduceIter(all_out_iter_vars, fake_reduce_iter_idx);
  std::vector<ir::Var> fake_reduce_iter_vars;
  for (const auto& idx : fake_reduce_iter_idx) {
    fake_reduce_iter_vars.emplace_back(
        all_out_iter_vars.at(static_cast<int>(idx)));
  }

  VLOG(4) << "all_out_iter_vars: "
          << cinn::utils::Join(all_out_iter_vars, ", ");
  VLOG(4) << "non_reduce_iter_vars: "
          << cinn::utils::Join(non_reduce_iter_vars, ", ");
  VLOG(4) << "fake_reduce_iter_vars: "
          << cinn::utils::Join(fake_reduce_iter_vars, ", ");

  ir::Expr trivial_last_for =
      (ExprSetFinderUtils::ChildFors *
       ExprSetFinderUtils::IsForIterVar(all_out_iter_vars.back()))
          .GetSingle(new_trivial_body);
  ir::Expr new_for_body = trivial_last_for.As<ir::For>()->body;

  const auto ExpandIterVars = [&]() {
    std::vector<ir::Var> result =
        ComposeUtils::ConcatVector(non_reduce_iter_vars, fake_reduce_iter_vars);
    auto upstream_reduce_iters = GetReduceIters(reduce_op);
    if (fake_reduce_iter_vars.size() != upstream_reduce_iters.size()) {
      result.insert(result.end(),
                    upstream_reduce_iters.begin(),
                    upstream_reduce_iters.end());
    }
    VLOG(4) << "ExpandIterVars: " << cinn::utils::Join(result, ", ");
    return result;
  };

  ir::Expr new_schedule_realizer =
      (ExprTransformerUtils::WrapForsTransformer(ExpandIterVars()) *
       ExprTransformerUtils::WrapScheduleRealizer({}, "root"))(new_for_body);

  VLOG(4) << "new_schedule_realizer\n" << new_schedule_realizer;
  return TrivialOp(new_schedule_realizer);
}

FusibleOp CreateFusibleOp(ir::Expr compute_body, OpPatternKind op_pattern) {
  if (IsTrivialKind(op_pattern)) {
    return TrivialOp(compute_body);
  } else {
    return ReduceOp(compute_body);
  }
}

template <typename T, typename F>
std::vector<T> FilterVector(const std::vector<T>& ops, const F& f) {
  std::vector<T> res;
  for (const auto& op : ops) {
    if (f(op)) {
      res.push_back(op);
    }
  }
  return res;
}

std::vector<ir::Expr> GetShapeFromVars(const std::vector<ir::Var>& vars) {
  std::vector<ir::Expr> res;
  for (const auto& v : vars) {
    res.emplace_back(v->upper_bound);
  }
  return res;
}

void DebugPrintReduceVar(const FusibleOp& op) {
  VLOG(4) << "DebugPrint Op: " << GetOutputTensor(op);
  VLOG(4) << "DebugPrint Op: " << GetComputeBody(op);
  const auto& block = (ExprSetFinderUtils::ChildScheduleBlockRealizes *
                       ExprSetFinderUtils::ScheduleBlockRealizeIsNotInit *
                       ExprSetFinderUtils::Realizer2ScheduleBlock)
                          .GetSingle(_GetRootExpr(op));
  const std::vector<ir::Var>& iter_vars =
      block.As<ir::ScheduleBlock>()->iter_vars;
  for (const auto& v : iter_vars) {
    VLOG(4) << "Var: " << v << "  is_reduce_axis=" << v->is_reduce_axis;
  }
}

std::pair<TrivialOp, ReduceOp> SplitReduceOp(const ReduceOp& reduce_op) {
  VLOG(4) << "Start SplitReduceOp";
  VLOG(4) << "DebugPrint Op Origin: " << _GetRootExpr(reduce_op);
  ir::Tensor reduce_out_tensor = GetOutputTensor(reduce_op);
  // substitude compute_body with a new init value.
  ir::Expr trivial_compute_body =
      ExprTransformerUtils::ChangeTensorLoadTransformer(
          GetOutputTensor(reduce_op),
          GetInitExpr(reduce_op))(GetComputeBody(reduce_op));

  const std::vector<ir::Var>& all_iters = ComposeUtils::ConcatVector(
      GetOutputIters(reduce_op), GetReduceIters(reduce_op));
  VLOG(4) << "Trivial Compute Body is " << trivial_compute_body;
  ir::Tensor new_trivial_tensor =
      ir::Tensor(reduce_out_tensor->name + "_split_transform",
                 reduce_out_tensor->type(),
                 GetShapeFromVars(all_iters),
                 GetShapeFromVars(all_iters),
                 ir::ComputeOp::Make(
                     reduce_out_tensor->name + "_split_transform",
                     [body = trivial_compute_body](
                         const std::vector<Expr>& indices) { return body; },
                     GetShapeFromVars(all_iters),
                     GetShapeFromVars(all_iters),
                     {}),
                 {});
  new_trivial_tensor->WithBuffer();
  VLOG(4) << "Created Tensor is: " << new_trivial_tensor;
  VLOG(4) << "Load Expr is: "
          << new_trivial_tensor(ComposeUtils::VarVec2ExprVec(all_iters));

  // push trivial op
  VLOG(4) << "Splited TrivialOp is "
          << CreateTrivialExpr(
                 all_iters, trivial_compute_body, new_trivial_tensor);

  const auto& result_trivial = TrivialOp(
      CreateTrivialExpr(all_iters, trivial_compute_body, new_trivial_tensor));

  // push reduce op, change compute_body to
  VLOG(4) << "WrapReduceOperation start: with reduce_type: "
          << GetOutputTensor(reduce_op)->body().As<ir::Reduce>()->reduce_type;
  VLOG(4) << "WrapReduceOperation new_trivial_tensor: "
          << new_trivial_tensor(ComposeUtils::VarVec2ExprVec(all_iters));
  const ir::Expr& new_reduce_body = ExprTransformerUtils::WrapReduceOperation(
      GetOutputTensor(reduce_op)->body().As<ir::Reduce>()->reduce_type,
      GetOutputTensor(reduce_op),
      ComposeUtils::VarVec2ExprVec(GetOutputIters(reduce_op)))(
      new_trivial_tensor(ComposeUtils::VarVec2ExprVec(all_iters)));
  VLOG(4) << "Splited ReduceOp body is " << new_reduce_body;
  VLOG(4) << "Splited ReduceOp is "
          << CreateExprWithNewComputeBody(
                 reduce_op,
                 ExprSetFinderUtils::Store2Value.GetSingle(new_reduce_body));
  const auto& result_reduce = ReduceOp(CreateExprWithNewComputeBody(
      reduce_op, ExprSetFinderUtils::Store2Value.GetSingle(new_reduce_body)));

  auto expand_pos = GetExpandVarPos(reduce_op);
  if (!expand_pos.empty()) {
    auto pad_result_trivial = std::get<TrivialOp>(
        cinn::fusion::DoPadding(result_trivial, expand_pos).back());
    auto pad_result_reduce = std::get<ReduceOp>(
        cinn::fusion::DoPadding(result_reduce, expand_pos).back());
    return std::make_pair(pad_result_trivial, pad_result_reduce);
  }
  VLOG(4) << "SplitReduceTransform End~";
  return std::make_pair(result_trivial, result_reduce);
}

std::vector<ir::Var> GetAllForIters(const ir::Expr& expr) {
  using cinn::hlir::framework::pir::trivial_fusion_detail::ExprSetFinderUtils::
      ChildFors;
  using cinn::hlir::framework::pir::trivial_fusion_detail::ExprSetFinderUtils::
      ChildScheduleBlockRealizes;
  using cinn::hlir::framework::pir::trivial_fusion_detail::ExprSetFinderUtils::
      FindFather;
  using cinn::hlir::framework::pir::trivial_fusion_detail::ExprSetFinderUtils::
      IsFor;
  using cinn::hlir::framework::pir::trivial_fusion_detail::ExprSetFinderUtils::
      ScheduleBlockRealizeIsNotInit;
  const auto& all_father_fors =
      (ChildScheduleBlockRealizes * ScheduleBlockRealizeIsNotInit *
       FindFather(expr) * IsFor)(expr);
  std::vector<ir::Var> vars;
  for (const auto& for_expr : all_father_fors) {
    vars.push_back(for_expr.As<ir::For>()->loop_var);
  }
  VLOG(4) << "GetAllForIters : " << expr
          << "\n var is : " << utils::Join(vars, ",");
  return vars;
}

}  // namespace trivial_fusion_detail

void InitLoopTransformMap(FusionGroupInfo* group_info, const ir::Expr& body) {
  using trivial_fusion_detail::ExprSetFinderUtils::ChildScheduleBlockRealizes;
  using trivial_fusion_detail::ExprSetFinderUtils::ChildTensorLoads;
  using trivial_fusion_detail::ExprSetFinderUtils::
      ScheduleBlockRealizeIsSplitTransform;

  std::vector<ir::Expr> split_transform_block =
      (ChildScheduleBlockRealizes * ScheduleBlockRealizeIsSplitTransform)(body);
  if (split_transform_block.empty()) {
    return;
  }
  CHECK_EQ(split_transform_block.size(), 1);
  VLOG(4) << "found split_transform block: " << body;

  auto* block = split_transform_block[0].As<ir::ScheduleBlockRealize>();
  auto& iter_values = block->iter_values;
  auto& iter_vars = block->schedule_block.As<ir::ScheduleBlock>()->iter_vars;
  CHECK_EQ(iter_values.size(), iter_vars.size());

  // Map iter_vars (e.g. inner_block_0) to loops.
  std::unordered_map<ir::Var, size_t> iter_var_map;
  std::vector<ir::Var> for_iters = trivial_fusion_detail::GetAllForIters(body);
  for (size_t i = 0; i < iter_vars.size(); i++) {
    auto iter = std::find(
        for_iters.begin(), for_iters.end(), iter_values[i].as_var_ref());
    CHECK(iter != for_iters.end());
    iter_var_map[iter_vars[i]] = std::distance(for_iters.begin(), iter);
  }

  // Map loops to the load indices of the first input.
  // TODO(liangshuhao): Choose the most appropriate input when there are
  // multiple inputs.
  const auto& all_loads = ChildTensorLoads(split_transform_block[0]);
  group_info->loop_transform_map.assign(for_iters.size(), -1);
  for (const auto& load : all_loads) {
    const auto& indices = load.As<ir::Load>()->indices;
    VLOG(4) << "load indices: " << utils::Join(indices, ", ");
    for (size_t i = 0; i < indices.size(); i++) {
      if (indices[i].as_var()) {
        size_t loop_idx = iter_var_map[indices[i].as_var_ref()];
        group_info->loop_transform_map[loop_idx] = i;
      }
    }
    VLOG(4) << "loop_transform_map: "
            << utils::Join(group_info->loop_transform_map, ", ");
    break;
  }
}

FusionGroupInfo GetFusionGroupInfo(
    const std::vector<ir::Expr>& op_compute_bodies) {
  using trivial_fusion_detail::AppendBound;
  using trivial_fusion_detail::GetAllForIters;
  using trivial_fusion_detail::IsReduceBody;
  using trivial_fusion_detail::ReduceOp;
  using trivial_fusion_detail::ComposeUtils::ConcatVector;

  FusionGroupInfo group_info = FusionGroupInfo();

  for (const auto& body : op_compute_bodies) {
    InitLoopTransformMap(&group_info, body);

    if (IsReduceBody(body)) {
      ReduceOp op = ReduceOp(body);
      if (group_info.reduce_var_name.empty()) {
        std::vector<ir::Var> all_iters =
            AppendBound(GetAllForIters(body), body);
        std::transform(all_iters.begin(),
                       all_iters.end(),
                       std::back_inserter(group_info.loop_ranges),
                       [](const ir::Var var) {
                         VLOG(4) << "Var is : : " << var;
                         VLOG(4) << "Var->upper_bound: " << var->upper_bound;
                         if (var->upper_bound.is_constant()) {
                           return var->upper_bound.as_int64();
                         } else {
                           return (int64_t)-1;
                         }
                       });
        std::vector<ir::Var> reduce_iters = fusion::FilterVector(
            all_iters, [](const ir::Var& var) { return var->is_reduce_axis; });
        for (int64_t i = all_iters.size() - reduce_iters.size();
             i < all_iters.size();
             i++) {
          group_info.reduce_axis.emplace_back(i);
        }
      }
      group_info.reduce_var_name.emplace_back(GetOutputTensor(op)->name);
    }
  }

  if (group_info.reduce_var_name.empty()) {
    trivial_fusion_detail::TrivialOp op =
        trivial_fusion_detail::TrivialOp(*(op_compute_bodies.begin()));
    std::vector<ir::Var> iters = GetOutputIters(op);
    std::transform(iters.begin(),
                   iters.end(),
                   std::back_inserter(group_info.loop_ranges),
                   [](const ir::Var var) {
                     if (var->upper_bound.is_constant()) {
                       return var->upper_bound.as_int64();
                     } else {
                       return (int64_t)-1;
                     }
                   });
  }
  VLOG(4) << group_info.DebugPrint();
  return group_info;
}

}  // namespace pir
}  // namespace framework
}  // namespace hlir
}  // namespace cinn
