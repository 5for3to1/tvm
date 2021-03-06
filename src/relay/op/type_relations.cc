/*!
 *  Copyright (c) 2018 by Contributors
 * \file type_relations.cc
 * \brief A set of utilities and common functionality
 * for type relations.
 */
#include <tvm/relay/expr.h>
#include <tvm/relay/logging.h>
#include <tvm/relay/op.h>
#include <numeric>
#include "../pass/incomplete_type.h"
#include "./type_relations.h"

namespace tvm {
namespace relay {

TensorType ToTensorType(const Type& t) {
  if (auto tt_node = t.as<TensorTypeNode>()) {
    return GetRef<TensorType>(tt_node);
  } else {
    return TensorType(nullptr);
  }
}

// TODO(@jroesch) what size value do we extract, 64bit or 32bit?
int ToInt(const tvm::Expr& e) {
  CHECK(e.defined());
  auto imm = e.as<tvm::ir::IntImm>();
  CHECK(imm) << "TYPE: " << imm << imm->type << std::endl;
  return imm->value;
}

Array<Type> IdentityRel(const Array<Type>& types, int num_args) {
  CHECK_EQ(types.size(), 2);
  auto t1 = ToTensorType(types[0]);
  if (t1 && types[1].as<IncompleteTypeNode>()) {
    return {t1, t1};
  } else {
    return types;
  }
}

static Type ConcreteBroadcast(const TensorType& t1, const TensorType& t2,
                              DataType output_dtype) {
  RELAY_LOG(INFO) << "ConcreteBroadcast: t1=" << t1 << " t2=" << t2
                  << std::endl;
  auto sh1 = t1->shape;
  auto sh2 = t2->shape;
  RELAY_LOG(INFO) << "ConcreteBroadcast: sh1=" << sh1 << " sh2=" << sh2
                  << std::endl;
  if (sh1.size() == 0 && sh2.size() == 0) {
    return TensorTypeNode::make({}, output_dtype);
    // We have non-zero shapes so broadcast rules apply.
  } else {
    auto suffix_len = static_cast<int>(std::min(sh1.size(), sh2.size()));
    auto full_len = static_cast<int>(std::max(sh1.size(), sh2.size()));

    auto rev_sh1 = sh1.rbegin();
    auto rev_sh2 = sh2.rbegin();

    while (rev_sh1 != sh1.rend() && rev_sh2 != sh2.rend()) {
      auto dim1 = ToInt(*rev_sh1);
      auto dim2 = ToInt(*rev_sh2);
      if ((dim1 != dim2) && ((dim1 != 1) && (dim2 != 1))) {
        CHECK(false) << "Dimension mistmatch "
                     << "dim1: " << dim1 << " dim2: " << dim2 << std::endl;
      }
      rev_sh1++;
      rev_sh2++;
    }

    Array<ShapeExpr> larger;
    Array<ShapeExpr> smaller;

    for (int i = 0; i < (full_len - suffix_len); i++) {
      smaller.push_back(tvm::ir::IntImm::make(HalideIR::Int(64), 1));
    }

    if (sh1.size() < sh2.size()) {
      for (auto sh : sh1) {
        smaller.push_back(sh);
      }
      larger = sh2;
    } else if (sh1.size() > sh2.size()) {
      for (auto sh : sh1) {
        larger.push_back(sh);
      }
      smaller = sh2;
    } else {
      larger = sh1;
      smaller = sh2;
    }

    CHECK_EQ(larger.size(), smaller.size());

    Array<HalideIR::Expr> out_shape;
    for (size_t i = 0; i < smaller.size(); i++) {
      auto left = smaller[i].as<tvm::ir::IntImm>();
      auto right = larger[i].as<tvm::ir::IntImm>();
      CHECK(left);
      CHECK(right);
      int64_t dim = std::max(left->value, right->value);
      out_shape.push_back(tvm::ir::IntImm::make(HalideIR::Int(64), dim));
    }

    return TensorTypeNode::make(out_shape, output_dtype);
  }
}

Array<Type> BroadcastRel(const Array<Type>& types, int num_args) {
  CHECK_EQ(types.size(), 3);
  RELAY_LOG(INFO) << "In1: " << types[0] << "In2: " << types[1]
                  << "Out: " << types[2] << std::endl;
  if (auto t1 = ToTensorType(types[0])) {
    if (auto t2 = ToTensorType(types[1])) {
      CHECK_EQ(t1->dtype, t2->dtype);
      return {t1, t2, ConcreteBroadcast(t1, t2, t1->dtype)};
    }
  }

  return types;
}

/* A relation which specifies broadcasting rules for operations which
   compute boolean results.
*/
Array<Type> BroadcastCompRel(const Array<Type>& types, int num_args) {
  CHECK_EQ(types.size(), 3);
  if (auto t1 = ToTensorType(types[0])) {
    if (auto t2 = ToTensorType(types[1])) {
      return {t1, t2, ConcreteBroadcast(t1, t2, HalideIR::Bool())};
    }
  }

  return types;
}

/*! \brief Handle concrete concat case from known input to output. */
inline Type ConcreteConcatRel(const Type& input_type) {
  if (auto tuple_node = input_type.as<TupleTypeNode>()) {
    // NB: For now the axis argument is hardwired to be 0.
    std::vector<int> dims;
    DataType dtype;

    CHECK_LT(1, tuple_node->fields.size());
    bool skip_first = true;

    // Collect the suffix dimensions since axis is zero.
    // TODO(@jroesch): This is a demonstration of how
    // to do varargs. It requires a little more work to
    // fully type the behavior of concat.

    auto first = Downcast<TensorType>(tuple_node->fields[0]);
    dtype = first->dtype;

    for (auto dim_expr : first->shape) {
      if (!skip_first) {
        dims.push_back(ToInt(dim_expr));
      } else {
        skip_first = false;
      }
    }

    std::vector<int> axis_dims;
    for (auto field_ty : tuple_node->fields) {
      auto ttype = Downcast<TensorType>(field_ty);
      for (size_t i = 0; i < ttype->shape.size(); i++) {
        if (i != 0) {
          CHECK_EQ(ToInt(dims[i - 1]), ToInt(ttype->shape[i]));
        } else {
          axis_dims.push_back(ToInt(ttype->shape[i]));
        }
      }
    }

    auto out_axis_dim = std::accumulate(axis_dims.begin(), axis_dims.end(), 0);

    Array<tvm::Expr> out_shape = { tvm::ir::IntImm::make(HalideIR::Int(64), out_axis_dim) };

    for (auto dim : dims) {
      out_shape.push_back(tvm::ir::IntImm::make(HalideIR::Int(64), dim));
    }

    return TensorTypeNode::make(out_shape, dtype);

  } else {
    throw TypeRelationError("concat can only be used with a tuple as its argument");
  }
}

Array<Type> ConcatRel(const Array<Type>& types, int num_args) {
  CHECK_EQ(types.size(), 2);

  if (types[0].as<IncompleteTypeNode>() && types[1].as<IncompleteTypeNode>()) {
    return types;
  } else if (types[1].as<IncompleteTypeNode>()) {
    return { types[0], ConcreteConcatRel(types[0]) };
  } else {
    throw TypeRelationError(
      "can not deduce relationship between the " \
      "type of concat's input and output");
  }
}

}  // namespace relay
}  // namespace tvm
