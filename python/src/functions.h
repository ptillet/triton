#include "triton/ir/builder.h"
#include <iostream>
#include <pybind11/pybind11.h>

namespace ir = triton::ir;
namespace py = pybind11;

static const std::string _builder_doc = R"pbdoc(
  :param builder: IR builder to generate code into, optional, set automatically when called inside a @triton.jit function
  :type builder: triton.ir.builder
)pbdoc";

#define DEF_FUNC(MOD, PY_NAME, C_FUNC, ...)                          \
  MOD.def(PY_NAME, C_FUNC, (C_FUNC##_docstr + _builder_doc).c_str(), \
          ret::reference, __VA_ARGS__, "builder"_a)

void throw_not_implemented(std::string key) {
  throw std::runtime_error("Encountered unimplemented code path in `" + key + "`. This is likely a bug on our side.");
}
/*----------------------------------------------
 definition of triton.cast / triton.ir.value.to
 ----------------------------------------------*/
std::string cast_docstr = R"pbdoc(
  Tries to cast a block to a new data type.

  :param input: The input block.
  :type input: triton.ir.value
)pbdoc";

ir::value *cast(ir::value *input, py::object _dtype, ir::builder *builder) {
  ir::type *dtype = _dtype.attr("make_ir")(builder->get_context()).cast<ir::type *>();
  if (input->get_type()->is_block_ty())
    dtype = ir::block_type::get(dtype, input->get_type()->get_block_shapes());
  // FP Truncation
  ir::type *src_ty = input->get_type()->get_scalar_ty();
  ir::type *dst_ty = dtype->get_scalar_ty();
  bool truncate_fp = src_ty->is_floating_point_ty() &&
                     dst_ty->is_floating_point_ty() &&
                     src_ty->get_fp_mantissa_width() > dst_ty->get_fp_mantissa_width();
  if (truncate_fp)
    return builder->create_fp_trunc(input, dtype);
  throw_not_implemented("cast");
}

/*----------------------------------------------
 definition of triton.broadcast_check
 ----------------------------------------------*/
std::string try_broadcast_docstr = R"pbdoc(
    Tries to broadcast two blocks to a common compatible shape.

    :param input: The first input block.
    :type input: triton.ir.value
    :param other: The second input block.
    :type other: triton.ir.value
)pbdoc";

std::tuple<ir::value *, ir::value *> try_broadcast(ir::value *lhs, ir::value *rhs, ir::builder *builder) {
  ir::type *lhs_ty = lhs->get_type();
  ir::type *rhs_ty = rhs->get_type();
  // make_shape_compatible(block, scalar)
  if (lhs_ty->is_block_ty() && !rhs_ty->is_block_ty())
    rhs = builder->create_splat(rhs, lhs_ty->get_block_shapes());
  // make_shape_compatible(scalar, block)
  else if (!lhs_ty->is_block_ty() && rhs_ty->is_block_ty())
    lhs = builder->create_splat(lhs, rhs_ty->get_block_shapes());
  // make_shape_compatible(block, block)
  else if (lhs_ty->is_block_ty() && rhs_ty->is_block_ty()) {
    auto lhs_shape = lhs_ty->get_block_shapes();
    auto rhs_shape = rhs_ty->get_block_shapes();
    if (lhs_shape.size() != rhs_shape.size())
      throw std::runtime_error("Cannot make_shape_compatible: blocks must have the same rank");
    ir::type::block_shapes_t ret_shape;
    for (size_t i = 0; i < lhs_shape.size(); ++i) {
      unsigned left = lhs_shape[i];
      unsigned right = rhs_shape[i];
      if (left == 1)
        ret_shape.push_back(right);
      else if (right == 1)
        ret_shape.push_back(left);
      else if (left == right)
        ret_shape.push_back(left);
      else
        throw std::runtime_error("Cannot make_shape_compatible: incompatible dimensions at index " + std::to_string(i) +
                                 ": " + std::to_string(left) + " and " + std::to_string(right));
    }
    if (lhs_shape != ret_shape)
      lhs = builder->create_broadcast(lhs, ret_shape);
    if (rhs_shape != ret_shape)
      rhs = builder->create_broadcast(rhs, ret_shape);
  }
  return std::make_tuple(lhs, rhs);
}

/*----------------------------------------------
 definition of triton.broadcast_to
 ----------------------------------------------*/
std::string broadcast_to_docstr = R"pbdoc(
    Tries to broadcast a block to a new shape.

    :param input: The input block.
    :type input: triton.value
    :param shape: The new shape.
    :type shape: tuple of int
)pbdoc";

ir::value *broadcast_to(ir::value *input, const ir::type::block_shapes_t &shape, ir::builder *builder) {
  if (!input->get_type()->is_block_ty())
    return builder->create_splat(input, shape);
  auto src_shape = input->get_type()->get_block_shapes();
  if (src_shape.size() != shape.size())
    throw std::runtime_error("Cannot broadcast");
  return builder->create_broadcast(input, shape);
}

/*----------------------------------------------
 definition of triton.load
 ----------------------------------------------*/
std::string load_docstr = R"pbdoc(
    Return a block of data whose values are, elementwise, loaded from memory at location defined by `pointer`.

    :param pointer: Pointer to the data to be loaded.
    :type pointer: Block of triton.pointer
    :param mask: if mask[idx] is false, do not load the data at `pointer[idx]`.
    :type mask: Block of triton.bool, optional
    :param other: if mask[idx] is false, return other[idx] instead of 'pointer[idx]`
    :type other: Block of triton.value, optional
  )pbdoc";

ir::value *load(ir::value *pointer, std::optional<ir::value *> _mask, std::optional<ir::value *> _other, ir::builder *builder) {
  if (!_mask.has_value() && !_other.has_value())
    return builder->create_load(pointer);
  if (!_mask.has_value())
    throw std::runtime_error("`other` cannot be provided without `mask`");
  ir::value *mask = _mask.value();
  ir::type *elt_ty = pointer->get_type()->get_scalar_ty()->get_pointer_element_ty();
  ir::value *other = _other.has_value() ? _other.value() : ir::undef_value::get(elt_ty);
  other = cast(other, py::cast(elt_ty), builder);
  other = broadcast_to(other, pointer->get_type()->get_block_shapes(), builder);
  return builder->create_masked_load(pointer, mask, other);
}

/*----------------------------------------------
 definition of triton.store
 ----------------------------------------------*/
std::string store_docstr = R"pbdoc(
    Stores `value` block of elements in memory, element-wise, at the memory locations specified by `pointer`. 

    :param pointer: The memory locations where the elements of `value` are stored.
    :type pointer: Block of triton.pointer
    :param value: The block of elements to be stored.
    :type value: Block of triton.value
    :param mask: If mask[idx] is false, do not store `value[idx]` at `pointer[idx]`.
    :type mask: Block of triton.bool, optional
  )pbdoc";
ir::value *store(ir::value *ptr, ir::value *val, std::optional<ir::value *> _mask, ir::builder *builder) {
  if (!_mask.has_value())
    return builder->create_store(ptr, val);
  ir::value *mask = _mask.value();
  return builder->create_masked_store(ptr, val, mask);
}

/*----------------------------------------------
 definition of triton.dot
 ----------------------------------------------*/
std::string dot_docstr = R"pbdoc(
    Returns the matrix product of two blocks.
    The two blocks must be two dimensionals and have compatible inner dimensions.

    :param input: The first block to be multiplied.
    :type input: 2D block of scalar-type in {`float16`, `float32`}
    :param other: The second block to be multiplied.
    :type other: 2D block of scalar-type in {`float16`, `float32`}
  )pbdoc";
ir::value *dot(ir::value *lhs, ir::value *rhs, ir::builder *builder) {
  ir::value *_0 = builder->get_float32(0);
  unsigned M = lhs->get_type()->get_block_shapes()[0];
  unsigned N = rhs->get_type()->get_block_shapes()[1];
  _0 = builder->create_splat(_0, {M, N});
  return builder->create_dot(lhs, rhs, _0);
}

/*----------------------------------------------
 definition of triton.where
 ----------------------------------------------*/
std::string where_docstr = R"pbdoc(
    Returns a block of elements from either `x` or `y`, depending on `condition`.
    Note that `x` and `y` are always evaluated regardless of the value of `condition`.
    If you want to avoid unintented memory operations, use the `mask` arguments in `triton.load` and `triton.store` instead.

    :param condition: When True (nonzero), yield x, otherwise yield y.
    :type condition: Block of triton.bool
    :param x: values selected at indices where condition is True.
    :param y: values selected at indices where condition is False.
  )pbdoc";
ir::value *where(ir::value *condition, ir::value *x, ir::value *y, ir::builder *builder) {
  return builder->create_select(condition, x, y);
};

/*----------------------------------------------
 definition of triton.arange
 ----------------------------------------------*/
std::string arange_docstr = R"pbdoc(
    Returns contiguous values within the open interval [start, end).

    :param start: Start of the interval.
    :type start: int
    :param stop: End of the interval.
    :type stop: int
  )pbdoc";
ir::value *arange(int start, int end, ir::builder *builder) {
  return builder->get_range(start, end);
};

/*----------------------------------------------
 definition of triton.program_id
 ----------------------------------------------*/
std::string program_id_docstr = R"pbdoc(
    Returns the id of the current program instance. 
    Triton uses an SPMD model in which different @triton.jit functions run in parallel with different `program_id`s.

    :param axis: The axis of the program id. Has to be either 0, 1 or 2.
    :type axis: int
  )pbdoc";
ir::value *program_id(int axis, ir::builder *builder) {
  return builder->create_get_program_id(axis);
};

/*----------------------------------------------
 definition of triton.zeros
 ----------------------------------------------*/
std::string zeros_docstr = R"pbdoc(
    Returns a block filled with the scalar value 0 and the given shape.

    :param shape: Shape of the new array, e.g., (8, 16) or (8, )
    :type shape: tuple of ints
    :param dtype: Data-type of the new array, e.g., triton.float16
    :type dtype: triton.ir.dtype
  )pbdoc";
ir::value *zeros(ir::type::block_shapes_t shape, py::object _dtype, ir::builder *builder) {
  ir::type *dtype = _dtype.attr("make_ir")(builder->get_context()).cast<ir::type *>();
  ir::value *_0 = ir::constant::get_null_value(dtype);
  return builder->create_splat(_0, shape);
};

/*----------------------------------------------
 definition of self + other
 ----------------------------------------------*/
std::string add_docstr = R"pbdoc(
    Returns self + other, element-wise.
)pbdoc";
ir::value *add(ir::value *self, ir::value *other, ir::builder *builder) {
  ir::type *scalar_ty = self->get_type()->get_scalar_ty();
  // ptr + offset
  if (scalar_ty->is_pointer_ty())
    return builder->create_gep(self, {other});
  // float + float
  else if (scalar_ty->is_floating_point_ty())
    return builder->create_fadd(self, other);
  // int + int
  else if (scalar_ty->is_integer_ty())
    return builder->create_add(self, other);
  throw_not_implemented("add");
}

/*----------------------------------------------
 definition of self - other
 ----------------------------------------------*/
std::string sub_docstr = R"pbdoc(
    Returns self - other, element-wise.
)pbdoc";
ir::value *sub(ir::value *self, ir::value *other, ir::builder *builder) {
  ir::type *scalar_ty = self->get_type()->get_scalar_ty();
  // ptr + offset
  if (scalar_ty->is_pointer_ty())
    return builder->create_gep(self, {other});
  // float + float
  if (scalar_ty->is_floating_point_ty())
    return builder->create_fsub(self, other);
  // int + int
  else if (scalar_ty->is_integer_ty())
    return builder->create_sub(self, other);
  throw_not_implemented("sub");
}

/*----------------------------------------------
 definition of self * other
 ----------------------------------------------*/
std::string mul_docstr = R"pbdoc(
    Returns self * other, element-wise.
)pbdoc";
ir::value *mul(ir::value *self, ir::value *other, ir::builder *builder) {
  ir::type *scalar_ty = self->get_type()->get_scalar_ty();
  // float * float
  if (scalar_ty->is_floating_point_ty())
    return builder->create_fmul(self, other);
  // int * int
  else if (scalar_ty->is_integer_ty())
    return builder->create_mul(self, other);
  throw_not_implemented("mul");
}

/*----------------------------------------------
 definition of self > other
 ----------------------------------------------*/
std::string greater_than_docstr = R"pbdoc(
    Returns self > other, element-wise.
)pbdoc";
ir::value *greater_than(ir::value *self, ir::value *other, ir::builder *builder) {
  ir::type *scalar_ty = self->get_type()->get_scalar_ty();
  // float > float
  if (scalar_ty->is_floating_point_ty())
    return builder->create_fcmpOGT(self, other);
  // int > int
  else if (scalar_ty->is_integer_ty())
    return builder->create_icmpSGT(self, other);
  throw_not_implemented("greater_than");
}

/*----------------------------------------------
 definition of self >= other
 ----------------------------------------------*/
std::string greater_equal_docstr = R"pbdoc(
    Returns self >= other, element-wise.
)pbdoc";
ir::value *greater_equal(ir::value *self, ir::value *other, ir::builder *builder) {
  ir::type *scalar_ty = self->get_type()->get_scalar_ty();
  // float >= float
  if (scalar_ty->is_floating_point_ty())
    return builder->create_fcmpOGE(self, other);
  // int >= int
  else if (scalar_ty->is_integer_ty())
    return builder->create_icmpSGE(self, other);
  throw_not_implemented("greater_equal");
}

/*----------------------------------------------
 definition of self < other
 ----------------------------------------------*/
std::string less_than_docstr = R"pbdoc(
    Returns self < other, element-wise.
)pbdoc";
ir::value *less_than(ir::value *self, ir::value *other, ir::builder *builder) {
  ir::type *scalar_ty = self->get_type()->get_scalar_ty();
  // float < float
  if (scalar_ty->is_floating_point_ty())
    return builder->create_fcmpOLT(self, other);
  // int < int
  else if (scalar_ty->is_integer_ty())
    return builder->create_icmpSLT(self, other);
  throw_not_implemented("less_than");
}

/*----------------------------------------------
 definition of self <= other
 ----------------------------------------------*/
std::string less_equal_docstr = R"pbdoc(
    Returns self <= other, element-wise.
)pbdoc";
ir::value *less_equal(ir::value *self, ir::value *other, ir::builder *builder) {
  ir::type *scalar_ty = self->get_type()->get_scalar_ty();
  // float < float
  if (scalar_ty->is_floating_point_ty())
    return builder->create_fcmpOLE(self, other);
  // int < int
  else if (scalar_ty->is_integer_ty())
    return builder->create_icmpSLE(self, other);
  throw_not_implemented("less_equal");
}

/*----------------------------------------------
 definition of self / other
 ----------------------------------------------*/
std::string _div_docstr = R"pbdoc(
    Returns self / other, element-wise.
)pbdoc";
ir::value *_div(ir::value *self, ir::value *other, ir::builder *builder) {
  ir::type *scalar_ty = self->get_type()->get_scalar_ty();
  // float / float
  if (scalar_ty->is_floating_point_ty())
    return builder->create_fdiv(self, other);
  // int / int
  else if (scalar_ty->is_integer_ty())
    return builder->create_sdiv(self, other);
  throw_not_implemented("div");
}

/*----------------------------------------------
 definition of self % other
 ----------------------------------------------*/
std::string mod_docstr = R"pbdoc(
    Returns self % other, element-wise.
)pbdoc";
ir::value *mod(ir::value *self, ir::value *other, ir::builder *builder) {
  ir::type *scalar_ty = self->get_type()->get_scalar_ty();
  // float % int
  if (scalar_ty->is_floating_point_ty())
    return builder->create_frem(self, other);
  // int % int
  else if (scalar_ty->is_integer_ty())
    return builder->create_srem(self, other);
  throw_not_implemented("mod");
}

/*----------------------------------------------
 definition of self & other
 ----------------------------------------------*/
std::string _and_docstr = R"pbdoc(
    Returns self & other, element-wise.
)pbdoc";
ir::value *_and(ir::value *self, ir::value *other, ir::builder *builder) {
  return builder->create_and(self, other);
}

/*----------------------------------------------
 definition of minimum(self, other)
 ----------------------------------------------*/
std::string minimum_docstr = R"pbdoc(
    Returns element-wise minimum of self and other
)pbdoc";
ir::value *minimum(ir::value *self, ir::value *other, ir::builder *builder) {
  return where(less_than(self, other, builder), self, other, builder);
}

/*----------------------------------------------
 definition of self[slices]
 ----------------------------------------------*/

enum slice_mode_t {
  NEWAXIS,
  ALL
};

std::string subscript_docstr = R"pbdoc(
    returns self[slices].

    :param slices: The slices to subscript with.
    :type slices: List of `None` or `:` slices.
)pbdoc";
ir::value *subscript(ir::value *self, std::vector<py::object> slices, ir::builder *builder) {
  std::vector<slice_mode_t> modes;
  for (py::object slice : slices) {
    py::object none = py::none();
    py::object all = py::make_tuple(none, none, none);
    if (slice.is(none))
      modes.push_back(NEWAXIS);
    else if (all.attr("__eq__")(slice))
      modes.push_back(ALL);
    else
      throw std::runtime_error("slice must be None or (None, None, None)");
  }

  ir::type::block_shapes_t shape;
  size_t curr = 0;
  for (slice_mode_t mode : modes) {
    if (mode == NEWAXIS)
      shape.push_back(1);
    else {
      assert(mode == ALL);
      shape.push_back(self->get_type()->get_block_shapes()[curr++]);
    }
  }
  return builder->create_reshape(self, shape);
}