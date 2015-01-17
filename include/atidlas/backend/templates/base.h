#ifndef ATIDLAS_TEMPLATES_base_
#define ATIDLAS_TEMPLATES_base_


#include <list>
#include <set>

#include "atidlas/types.h"
#include "atidlas/backend/parse.h"
#include "atidlas/backend/stream.h"
#include "atidlas/cl/cl.hpp"
#include "atidlas/cl/lazy_compiler.h"
#include "atidlas/symbolic/expression.h"

namespace atidlas
{

enum fetching_policy_type
{
  FETCH_FROM_LOCAL,
  FETCH_FROM_GLOBAL_STRIDED,
  FETCH_FROM_GLOBAL_CONTIGUOUS
};

//Error codes
static const int TEMPLATE_VALID = 0;
static const int TEMPLATE_LOCAL_MEMORY_OVERFLOW = -1;
static const int TEMPLATE_WORK_GROUP_SIZE_OVERFLOW = -2;
static const int TEMPLATE_LOCAL_SIZE_0_OVERFLOW = -3;
static const int TEMPLATE_LOCAL_SIZE_1_OVERFLOW = -4;
static const int TEMPLATE_LOCAL_SIZE_2_OVERFLOW = -5;
static const int TEMPLATE_LOCAL_SIZE_NOT_WARP_MULTIPLE = -6;
static const int TEMPLATE_INVALID_SIMD_WIDTH = -7;
static const int TEMPLATE_ALIGNMENT_MUST_BE_BLOCK_SIZE_MULTIPLE = -8;
static const int TEMPLATE_INVALID_FETCHING_POLICY_TYPE= -9;

static const int TEMPLATE_GLOBAL_MEMORY_REQUIRES_ZERO_LOCAL_FETCH = -10;
static const int TEMPLATE_MS_NS_MUST_BE_SIMD_WIDTH_MULTIPLE = -11;
static const int TEMPLATE_KS_MUST_BE_SMALLER_THAN_KL = -12;
static const int TEMPLATE_SIMD_WIDTH_MUST_BE_ONE = -13;
static const int TEMPLATE_LOCAL_FETCH_PRODUCT_MUST_MATCH_LOCAL_SIZE_PRODUCT = -14;
static const int TEMPLATE_LOCAL_FETCH_0_MUST_BE_KL_MULTIPLE = -15;
static const int TEMPLATE_LOCAL_FETCH_0_MUST_BE_NL_MULTIPLE = -16;
static const int TEMPLATE_LOCAL_FETCH_1_MUST_BE_KL_MULTIPLE = -17;
static const int TEMPLATE_LOCAL_FETCH_1_MUST_BE_ML_MULTIPLE = -18;

class base
{
public:
  struct parameters_type
  {
    parameters_type(unsigned int _simd_width, int_t _local_size_1, int_t _local_size_2, int_t _num_kernels);
    unsigned int simd_width;
    int_t local_size_0;
    int_t local_size_1;
    int_t num_kernels;
  };

  class invalid_exception : public std::exception
  {
  public:
    invalid_exception();
    invalid_exception(std::string message);
    virtual const char* what() const throw();
    virtual ~invalid_exception() throw();
  private:
    std::string message_;
  };

protected:

  /** @brief Functor to map the symbolic_expressions to the types defined in mapped_objects.hpp */
  class map_functor : public traversal_functor
  {
    /** @brief Accessor for the numeric type */
    numeric_type get_numeric_type(atidlas::symbolic_expression const * symbolic_expression, int_t root_idx) const;
    /** @brief Creates a binary leaf */
    template<class T> tools::shared_ptr<mapped_object> binary_leaf(atidlas::symbolic_expression const * symbolic_expression, int_t root_idx, mapping_type const * mapping) const;
    /** @brief Creates a value scalar mapping */
    tools::shared_ptr<mapped_object> create(numeric_type dtype, values_holder) const;
    /** @brief Creates a vector mapping */
    tools::shared_ptr<mapped_object> create(array const &) const;
    /** @brief Creates a tuple mapping */
    tools::shared_ptr<mapped_object> create(repeat_infos const &) const;
    /** @brief Creates a mapping */
    tools::shared_ptr<mapped_object> create(lhs_rhs_element const &) const;
  public:
    map_functor(symbolic_binder & binder, mapping_type & mapping);
    /** @brief Functor for traversing the tree */
    void operator()(atidlas::symbolic_expression const & symbolic_expression, int_t root_idx, leaf_t leaf_t) const;
  private:
    symbolic_binder & binder_;
    mapping_type & mapping_;
  };

  /** @brief functor for setting the arguments of a kernel */
  class set_arguments_functor : public traversal_functor
  {
  public:
    typedef void result_type;

    set_arguments_functor(symbolic_binder & binder, unsigned int & current_arg, cl::Kernel & kernel);
    void set_arguments(numeric_type dtype, values_holder const & scal) const;
    void set_arguments(array const & ) const;
    void set_arguments(repeat_infos const & i) const;
    void set_arguments(lhs_rhs_element const & lhs_rhs) const;

    void operator()(atidlas::symbolic_expression const & symbolic_expression, int_t root_idx, leaf_t leaf_t) const;
  private:
    symbolic_binder & binder_;
    unsigned int & current_arg_;
    cl::Kernel & kernel_;
  };

  struct loop_body_base
  {
    virtual void operator()(kernel_generation_stream & stream, unsigned int simd_width) const = 0;
  };

  static void fetching_loop_info(fetching_policy_type policy, std::string const & bound, kernel_generation_stream & stream,
                                 std::string & init, std::string & upper_bound, std::string & inc, std::string const & domain_id, std::string const & domain_size);
  static void element_wise_loop_1D(kernel_generation_stream & stream, loop_body_base const & loop_body,
                                   fetching_policy_type fetch, unsigned int simd_width, std::string const & i, std::string const & bound, std::string const & domain_id, std::string const & domain_size);
  static void compute_reduction(kernel_generation_stream & os, std::string acc, std::string cur, op_element const & op);
  static void compute_index_reduction(kernel_generation_stream & os, std::string acc, std::string cur, std::string const & acc_value, std::string const & cur_value, op_element const & op);
  static void process_all(std::string const & type_key, std::string const & str,
                          kernel_generation_stream & stream, std::vector<mapping_type> const & mappings);
  static void process_all_at(std::string const & type_key, std::string const & str,
                             kernel_generation_stream & stream, std::vector<mapping_type> const & mappings,
                             size_t root_idx, leaf_t leaf);
  static std::string neutral_element(op_element const & op);
  static std::string generate_arguments(std::vector<mapping_type> const & mappings, std::map<std::string, std::string> const & accessors,
                                        symbolic_expressions_container const & symbolic_expressions);
  static std::string generate_arguments(std::string const & data_type, std::vector<mapping_type> const & mappings,
                                        symbolic_expressions_container const & symbolic_expressions);
  static void fill_kernel_name(char * ptr, unsigned int label, const char * suffix);
  static bool is_node_trans(symbolic_expression::container_type const & array, size_t root_idx, leaf_t leaf_type);
  static std::string append_simd_suffix(std::string const & str, unsigned int i);
  static bool is_strided(symbolic_expression_node const & node);
  static int_t vector_size(symbolic_expression_node const & node);
  static std::pair<int_t, int_t> matrix_size(symbolic_expression_node const & node);
  static unsigned int align(unsigned int to_round, unsigned int base);
  static bool is_reduction(symbolic_expression_node const & node);
  static bool is_index_reduction(op_element const & op);

  tools::shared_ptr<symbolic_binder> make_binder();
  static std::string vstore(unsigned int simd_width, std::string const & value, std::string const & offset, std::string const & ptr);
  static std::string vload(unsigned int simd_width, std::string const & offset, std::string const & ptr);
  static std::string append_width(std::string const & str, unsigned int width);
  bool has_strided_access(symbolic_expressions_container const & symbolic_expressions) const;
  void set_arguments(symbolic_expressions_container const & symbolic_expressions, cl::Kernel & kernel, unsigned int & current_arg);


private:
  virtual std::vector<std::string> generate_impl(unsigned int label, symbolic_expressions_container const & symbolic_expressions, std::vector<mapping_type> const & mapping) const = 0;

public:
  base(binding_policy_t binding_policy);
  virtual unsigned int lmem_usage(symbolic_expressions_container const &) const;
  virtual unsigned int registers_usage(symbolic_expressions_container const &) const;
  virtual std::vector<int_t> input_sizes(symbolic_expressions_container const & symbolic_expressions) = 0;
  virtual ~base();
  std::vector<std::string> generate(unsigned int label, symbolic_expressions_container const & symbolic_expressions, cl::Device const & device);
  virtual int check_invalid(symbolic_expressions_container const & symbolic_expressions, cl::Device const & device) const = 0;
  virtual void enqueue(cl::CommandQueue & queue,
                       std::vector<cl::lazy_compiler> & programs,
                       unsigned int label, symbolic_expressions_container const & symbolic_expressions) = 0;
  virtual tools::shared_ptr<base> clone() const = 0;
private:
  binding_policy_t binding_policy_;
};


template<class TemplateType, class ParametersType>
class base_impl : public base
{
private:
  virtual int check_invalid_impl(cl::Device const &, symbolic_expressions_container const &) const;
protected:
  bool has_misaligned_offset(symbolic_expressions_container const & symbolic_expressions);
public:
  typedef ParametersType parameters_type;
  base_impl(parameters_type const & parameters, binding_policy_t binding_policy);
  int_t local_size_0() const;
  int_t local_size_1() const;
  tools::shared_ptr<base> clone() const;
  /** @brief returns whether or not the profile has undefined behavior on particular device */
  int check_invalid(symbolic_expressions_container const & symbolic_expressions, cl::Device const & device) const;
protected:
  parameters_type p_;
  binding_policy_t binding_policy_;
};

}

#endif