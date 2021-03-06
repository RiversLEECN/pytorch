#include <torch/csrc/jit/script/compiler.h>
#include <torch/csrc/jit/passes/lower_tuples.h>
#include <torch/csrc/jit/passes/constant_pooling.h>
#include <torch/csrc/jit/operator.h>
#include <torch/csrc/jit/interpreter.h>
#include <torch/csrc/jit/ir.h>
#include <torch/csrc/jit/script/parser.h>
#include <torch/csrc/jit/assertions.h>
#include <torch/csrc/utils/object_ptr.h>
#include <torch/csrc/jit/operator.h>
#include <torch/csrc/jit/script/builtin_functions.h>
#include <torch/csrc/jit/hooks_for_testing.h>

#include <torch/csrc/jit/constants.h>

#include <c10/util/Optional.h>

#include <climits>
#include <set>

namespace torch {
namespace jit {
namespace script {

using SugaredValuePtr = std::shared_ptr<SugaredValue>;
using FunctionTable = std::unordered_map<std::string, Method&>;
using ValueTable = std::unordered_map<std::string, SugaredValuePtr>;
using AttributeMap = std::unordered_map<std::string, Const>;
using ListAttributeMap = std::unordered_map<std::string, std::vector<Const>>;

struct NoneValue : SugaredValue {
  NoneValue() = default;
  std::string kind() const override {
    return "None";
  }
};

struct PrintValue : public SugaredValue {
  std::string kind() const override {
    return "print";
  }
  std::shared_ptr<SugaredValue> call(
    const SourceRange& loc,
    Method & m,
    at::ArrayRef<NamedValue> inputs,
    at::ArrayRef<NamedValue> attributes,
    size_t n_binders) override {
      auto& g = *m.graph();
      if (!attributes.empty())
        throw ErrorReport(loc) << "print doesn't accept any keyword arguments";

      //temporary hack to allow print statements to work in python 2, where
      //print(a, b) is treated as a (a, b) tuple input.

      std::vector<Value*> lowered_inputs = toValues(*m.graph(), inputs);
      if(lowered_inputs.size() == 1 && lowered_inputs.at(0)->node()->kind() == prim::TupleConstruct) {
        auto input = lowered_inputs[0];
        for(size_t j = 0; j < input->node()->inputs().size(); ++j) {
          lowered_inputs.insert(lowered_inputs.begin() + 1 + j, input->node()->inputs().at(j));
        }
        lowered_inputs.erase(lowered_inputs.begin());
      }
      g.insertNode(g.create(prim::Print, lowered_inputs, 0)
                       ->setSourceLocation(std::make_shared<SourceRange>(loc)));
      return std::make_shared<NoneValue>();
  }
};

// expressions like int(x)
// these are the same as call prim::Int or equivalent except it
// is a noop when the input is a subtype of 'type'
struct CastValue : public BuiltinFunction {
  CastValue(TypePtr type, c10::Symbol method)
  : BuiltinFunction(method, c10::nullopt)
  , type_(std::move(type)) {}
  std::shared_ptr<SugaredValue> call(
    const SourceRange& loc,
    Method & m,
    at::ArrayRef<NamedValue> inputs,
    at::ArrayRef<NamedValue> attributes,
    size_t n_binders) override {
      if(inputs.size() == 1 && attributes.size() == 0) {
        auto v = inputs[0].value(*m.graph());
        if (v->type()->isSubtypeOf(type_)) {
          return std::make_shared<SimpleValue>(v);
        }
      }
      return BuiltinFunction::call(loc, m , inputs, attributes, n_binders);
  }
private:
  TypePtr type_;
};

static Value* asSimple(const SugaredValuePtr& value) {
  if(SimpleValue* sv = dynamic_cast<SimpleValue*>(value.get())) {
    return sv->getValue();
  }
  return nullptr;
}
// we consider _N where N is a number, to be a non-meaningful name
// and do not record it as a unique name. This allows python printing to
// be able to export and import more consistently named graphs
static bool meaningfulName(const std::string& name) {
  if (name.size() == 0)
    return false;
  if (name[0] != '_')
    return true;
  for (size_t i = 1; i < name.size(); ++i) {
    if (!isdigit(name[i]))
      return true;
  }
  return false;
}

// Auxiliary data structure for desugaring variable binding into our always
// explicitly scoped language as we descend down
// nested control structures in the frontend (which themselves don't introduce
// scopes)
//
// The algorithm is roughly as follows:
// 1) While emitting a block within a control operator, add inputs and outputs
//      from the block for each value referenced (both "reads" and "writes").
//      This sets the value up as a candidate loop carried dependency.
// 2) When we reach the end of the block, examine all the values in the current
//      scope's value map. If the name also resides in an outer scope with a
//      different Value*, this is a true loop-carried dependency. If not, this
//      value was not assigned to. Replace all references to the block input
//      with the Value* pointed to in the tightest enclosing scope. Then delete
//      that block input and output.
// 3) When we emit the actual control operator, take all of the loop-carried
//      dependency values as inputs and return them as outputs from the control
//      op
//
//  Note that an alternative implementation could only add the loop-carried dep
//      inputs and outputs when we see a value that is mutated. This, however
//      requires replacing all references to that value *within the current
//      block* with a new input. That is to say: we need to traverse the pre-
//      decessor nodes and replace inputs that reference that value with the
//      newly-created input. This could be made less expensive with a change to
//      the IR API, but for now we choose to pessimisitically create inputs and
//      delete unnecessary ones later with replaceAllusesWith().
struct Environment {
  Environment(Method & method, Resolver resolver, Block* b, std::shared_ptr<Environment> next = nullptr)
      : method(method), resolver(std::move(resolver)), b(b), next(std::move(next)) {}

  Method & method;
  Resolver resolver;
  std::vector<std::string> captured_inputs;
  std::unordered_map<std::string, std::string> error_messages;
  Block* b;

  std::shared_ptr<Environment> next;

  // set type error in the lowest environment. if the variable is used after an
  // error has been set, then we will use the more informative error message
  void setVariableTypeError(const std::string& name, const std::string &msg) {
    auto runner = this;
    while (runner->next) {
      runner = runner->next.get();
    }
    runner->error_messages[name] = msg;
  }

  // see if type error has been set for a variable
  c10::optional<std::string> findVariableTypeError(const std::string& name) {
    auto runner = this;
    while (runner->next) {
      runner = runner->next.get();
    }
    auto msg = runner->error_messages.find(name);
    if (msg != runner->error_messages.end()) {
      return msg->second;
    } else {
      return c10::nullopt;
    }
  }

  SugaredValuePtr findInThisFrame(const std::string& name) {
    auto it = value_table.find(name);
    if (it != value_table.end()) {
      return it->second;
    }
    return nullptr;
  }

  SugaredValuePtr findInParentFrame(const std::string& name) {
    return next ? next->findInAnyFrame(name) : nullptr;
  }

  SugaredValuePtr findInAnyFrame(const std::string& name) {
    for (auto runner = this; runner; runner = runner->next.get()) {
      if(auto r = runner->findInThisFrame(name)) {
        return r;
      }
    }
    return nullptr;
  }

  Value* getValueInThisFrame(const SourceRange& loc, const std::string& name) {
    return value_table.at(name)->asValue(loc, method);
  }

  SugaredValuePtr createCapturedInput(Value* orig, const std::string& name) {
    // insert the captured input alphabetically in the capture list.
    // this ensures consistency of the order of loop-carried dependencies
    // even when the use in the loop is in a different order
    size_t insert_pos = 0;
    while (insert_pos < captured_inputs.size() && name > captured_inputs[insert_pos]) {
      insert_pos++;
    }
    captured_inputs.insert(captured_inputs.begin() + insert_pos, name);

    // Create the input
    const size_t loop_carried_block_inputs_offset = 1;
    Value* new_input = b->insertInput(loop_carried_block_inputs_offset + insert_pos)
                           ->setType(orig->type());

    // Associate this name with this value
    auto sv = std::make_shared<SimpleValue>(new_input);
    value_table[name] = sv;

    return sv;
  }

  SugaredValuePtr createCapturedInputIfNeeded(const SourceRange& loc, const std::string& ident) {
    auto in_frame = findInThisFrame(ident);
    if (in_frame) {
      return in_frame;
    }

    // recursively handles the case where parent blocks are also loops
    auto from_parent = next ? next->createCapturedInputIfNeeded(loc, ident) : nullptr;

    // recursively create the captured input if it is the loop block
    if (from_parent && getBlockOwningKind() == prim::Loop) {
      if (Value* simple_val = asSimple(from_parent))
        from_parent = createCapturedInput(simple_val, ident);
    }
    return from_parent;
  }

  Block* block() {
    return b;
  }
  Symbol getBlockOwningKind() {
    Symbol owning_kind = Symbol();
    if (b->owningNode()) {
      owning_kind = b->owningNode()->kind();
    }
    return owning_kind;
  }

  void setVar(const SourceRange& loc, const std::string& name, Value* value) {
    setSugaredVar(loc, name, std::make_shared<SimpleValue>(value));
  }

  void setSugaredVar(const SourceRange& loc, const std::string& name, SugaredValuePtr value) {
    Value* as_simple_value = asSimple(value);
    if (as_simple_value && !as_simple_value->hasUniqueName() &&
        meaningfulName(name) &&
        // note: if the value wasn't defined in this block, we might be giving a name
        // only used inside this block to a value outside of this. this is not
        // normally helpful for debugging and causes import/export jitter.
        as_simple_value->node()->owningBlock() == block()) {
      as_simple_value->setUniqueName(name);
    }
    // prevent re-assignment involving any sugared values
    // any reassignment like:
    // a = ...
    // while ...
    //   a = ..
    // requires 'a' to be first-class in the graph since its value depends on
    // control flow
    if(auto parent = findInParentFrame(name)) {
      if(!as_simple_value) {
        throw ErrorReport(loc) << "Cannot re-assign '" << name << "' to a value of type " << value->kind() <<
	" because " << name << " is not a first-class value.  Only reassignments to first-class values are allowed";
      }
      Value* simple_parent = asSimple(parent);
      if(!simple_parent) {
        throw ErrorReport(loc) << "Cannot re-assign '" << name << "' because it has type " << value->kind() <<
	" and " << name << " is not a first-class value.  Only reassignments to first-class values are allowed";
      }
      if (!as_simple_value->type()->isSubtypeOf(
              unshapedType(simple_parent->type()))) {
        std::stringstream errMsg;
        errMsg << "variable '" << name << "' previously has type "
               << simple_parent->type()->str()
               << " but is now being assigned to a value of type "
               << as_simple_value->type()->str();
        // Special-cased error msg if we're trying to assign to a tensor list.
        if (simple_parent->type()->kind() == TypeKind::ListType &&
            as_simple_value->type()->kind() == TypeKind::ListType) {
          errMsg << "\n. (Note: empty lists are constructed as Tensor[]; "
                 << "if you want an empty list of a different type, "
                 << "use `torch.jit.annotate(List[T], [])`, "
                 << "where `T` is the type of elements in the list)";
        }
        throw ErrorReport(loc) << errMsg.str();
      }
    }
    if (as_simple_value)
      createCapturedInputIfNeeded(loc, name);
    value_table[name] = std::move(value);
  }

  SugaredValuePtr getSugaredVar(const Ident& ident, bool required=true) {
    return getSugaredVar(ident.name(), ident.range());
  }
  Value* getVar(const Ident& ident) {
    return getSugaredVar(ident)->asValue(ident.range(), method);
  }

  SugaredValuePtr getSugaredVar(const std::string& ident, const SourceRange& range, bool required=true) {
    auto retval = createCapturedInputIfNeeded(range, ident);

    if(!retval) {
      static std::unordered_map<std::string, SugaredValuePtr> globals = {
        {"print", std::make_shared<PrintValue>()},
        {"float", std::make_shared<CastValue>(FloatType::get(), prim::Float)},
        {"int", std::make_shared<CastValue>(IntType::get(), prim::Int)},
        {"bool", std::make_shared<CastValue>(BoolType::get(), prim::Bool)},
        {"getattr", std::make_shared<GetAttrValue>()},
        {"isinstance", std::make_shared<IsInstanceValue>()},
        // todo(zach): remove when we can correctly export torch.full via ONNX
        // or we have implicit conversion that can convert numbers to tensors
        {"_to_tensor", std::make_shared<CastValue>(DynamicType::get(), prim::NumToTensor)},
      };
      auto it = globals.find(ident);
      if(it != globals.end())
        retval = it->second;
    }

    if(!retval) {
      retval = resolver(ident, method, range);
    }

    if (!retval && required) {
      // check if this value was not emitted in an if statement because of a
      // type mismatch. if it was, then we print a more informative error msg
      if (auto msg = findVariableTypeError(ident)) {
        throw ErrorReport(range) << *msg << "and was used here";
      }
      throw ErrorReport(range) << "undefined value " << ident;
    }
    return retval;
  }

  Value* getVar(const std::string& ident, const SourceRange& range) {
    return getSugaredVar(ident, range)->asValue(range, method);
  }

  // Given that after emitting statements in a block, we've added block inputs
  // for all value references and assignments, delete inputs for which there was
  // no assignment, only references.
  void deleteExtraInputs(const SourceRange& loc) {
    // note: skip i == 0, it is the loop trip count for inputs
    // and the loop condition for outputs.
    // captured_inputs is indexed by i - 1 since it only contains loop
    // carried dependencies
    //          inputs: loop_counter, lcd0, lcd1, ...
    //         outputs: loop_condition, lcd0, lcd1, ...
    // captured_inputs: lcd0, lcd1, ...
    JIT_ASSERT(b->inputs().size() == b->outputs().size());
    JIT_ASSERT(b->inputs().size() == captured_inputs.size() + 1);
    for(size_t i = b->inputs().size() - 1; i > 0; i--) {
      // nothing changed along this loop
      if(b->inputs()[i] == b->outputs()[i]) {
        auto name = captured_inputs[i - 1];
        Value* orig = findInParentFrame(name)->asValue(loc, method);
        b->inputs()[i]->replaceAllUsesWith(orig);
        b->eraseInput(i);
        b->eraseOutput(i);
        captured_inputs.erase(captured_inputs.begin() + i - 1);
      }
    }
  }
  std::vector<std::string> definedVariables() {
    std::vector<std::string> result;
    for(auto & kv : value_table) {
      result.push_back(kv.first);
    }
    return result;
  }
private:
  ValueTable value_table;
};

Value* packOutputs(Graph& g, at::ArrayRef<Value*> values) {
  if(values.size() == 1) {
    return values[0];
  }
  return g.insertNode(g.createTuple(values))->output();
}

at::ArrayRef<Value*> createTupleUnpack(Value* v) {
  // small peephole optimization to ensure IntList attributes can still turn
  // into constants e.g. in x.expand([3, 4])
  if(v->node()->kind() == prim::TupleConstruct)
    return v->node()->inputs();
  auto & g = *v->owningGraph();
  return g.insertNode(g.createTupleUnpack(v))->outputs();
}

inline TypePtr unwrapOptional(TypePtr opt_type) {
  if (auto unwrap_list_type = opt_type->cast<OptionalType>()) {
    return unwrap_list_type->getElementType();
  }
  return opt_type;
}

static inline bool isIntOrFloatUsedAsList(
    const Value* value,
    const Argument& arg) {
  // Look for int[N] or float[N]
  const auto& v_type = value->type();
  if (v_type != FloatType::get() && v_type != IntType::get())
    return false;
  auto arg_type = unwrapOptional(arg.type());
  auto list_type = arg_type->cast<ListType>();
  return list_type && list_type->getElementType() == v_type && arg.N();
}

inline bool convertibleToList(const TypePtr& type, const TypePtr& list_type_) {
  auto list_type = list_type_->cast<ListType>();
  if(!list_type) {
    return false;
  }
  if(type->isSubtypeOf(list_type_)) {
    return true;
  }
  if(auto tuple = type->cast<TupleType>()) {
    return std::all_of(
        tuple->elements().begin(),
        tuple->elements().end(),
        [&](const TypePtr& t) {
          return t->isSubtypeOf(list_type->getElementType());
        });
  }
  return false;
}

// applies implict conversion from value trying to turn it into type concrete_type
// it succeeds if the return_value->isSubclassOf(concrete_type)
Value* tryConvertToType(
    const SourceRange& loc,
    Graph& graph,
    const TypePtr& concrete_type,
    Value* value,
    bool allow_conversions) {

  if (auto value_tuple = value->type()->cast<TupleType>()) {
    // Allow homogeneous tuples to be casted implicitly to lists of appropriate
    // types
    if (convertibleToList(value->type(), unwrapOptional(concrete_type))) {
      auto unpacked = createTupleUnpack(value);
      auto elem_type = unwrapOptional(concrete_type)->expect<ListType>()->getElementType();
      value = graph.insertNode(graph.createList(elem_type, unpacked))->output();
    }
    // inductively apply implicit conversions to tuples
    if (auto concrete_tuple = concrete_type->cast<TupleType>()) {
      if (!value_tuple->isSubtypeOf(concrete_tuple) &&
          concrete_tuple->elements().size() == value_tuple->elements().size()) {
        auto unpacked = createTupleUnpack(value);
        std::vector<Value*> converted;
        for (size_t i = 0; i < concrete_tuple->elements().size(); ++i) {
          converted.emplace_back(tryConvertToType(
              loc,
              graph,
              concrete_tuple->elements().at(i),
              unpacked.at(i),
              allow_conversions));
        }
        value = graph.insertNode(graph.createTuple(converted))->output();
      }
    }
  }

  if (value->type()->isSubtypeOf(NoneType::get()) && !concrete_type->isSubtypeOf(NoneType::get())){
    if (concrete_type->isSubtypeOf(GeneratorType::get())) {
      value = graph.insertNode(graph.createNoneGenerator())->output();
    } else if (concrete_type->isSubtypeOf(OptionalType::ofTensor())) {
      // create undefined tensor when None pass to a optional[tensor] formal arg
      value = graph.insertNode(graph.createUndefined())->output();
    } else if (auto optional_type = concrete_type->cast<OptionalType>()) {
      value = graph.insertNode(graph.createNone(optional_type->getElementType()))->output();
    }
  }

  //implicit conversions
  if(allow_conversions) {
     if(concrete_type->isSubtypeOf(NumberType::get())
      && value->type()->isSubtypeOf(DynamicType::get())) {
      auto n = graph.createImplicitTensorToNum(concrete_type, value);
      value = graph.insertNode(n)
        ->setSourceLocation(std::make_shared<SourceRange>(loc))
        ->output();
    }
    if (value->type()->isSubtypeOf(StringType::get()) &&
        DeviceObjType::get()->isSubtypeOf(concrete_type))  {
      return graph.insert(aten::device, { value }, {}, loc);
    }
  }

  return value;
}

Value* tryMatchArgument(
    const Argument& arg,
    Graph& graph,
    const SourceRange& loc,
    const NamedValue& named_value,
    const std::function<std::ostream&()>& err,
    bool allow_conversions,
    TypeEnv & type_env) {
  Value* value = named_value.value(graph);

  // some functions that take lists of integers or floats for fixed size arrays
  // also allow single ints/floats to be passed in their place.
  // the single int/float is then repeated to the length of the list
  if (isIntOrFloatUsedAsList(value, arg)) {
    std::vector<Value*> repeated(*arg.N(), value);
    value = graph.insertNode(graph.createList(value->type(), repeated))->output();
  }

  const MatchTypeReturn matched_type =
      matchTypeVariables(arg.type(), value->type(), type_env);
  if (!matched_type.type) {
    err() << "could not match type " << value->type()->str() << " to "
          << arg.type()->str() << " in argument '" << arg.name()
          << "': " << matched_type.errMsg << "\n"
          << named_value.locOr(loc);
    return nullptr;
  }
  const auto concrete_type = *matched_type.type;

  value = tryConvertToType(loc, graph, concrete_type, value, allow_conversions);

  if(!value->type()->isSubtypeOf(concrete_type)) {
    err() << "expected a value of type " << concrete_type->str() << " for argument '" << arg.name() << "' but found "
          << value->type()->str() << "\n"
          << named_value.locOr(loc);
    return nullptr;
  }
  return value;
}

c10::optional<size_t> findInputWithName(
    const std::string& name,
    at::ArrayRef<NamedValue> kwargs) {
  for(size_t i = 0; i < kwargs.size(); ++i) {
    if(kwargs[i].name() == name)
      return i;
  }
  return c10::nullopt;
}

Value* tryCreateList(
    const TypePtr& elem_type,
    Graph& graph,
    const SourceRange& loc,
    at::ArrayRef<NamedValue> varargs,
    const std::function<std::ostream&()>& err,
    bool convert_tensor_to_num,
    TypeEnv & type_env) {
  Argument elem_arg("<varargs>", elem_type);
  std::vector<Value*> list_ctor;
  for(const auto& a : varargs) {
    Value* av = tryMatchArgument(elem_arg, graph, loc, a, err, convert_tensor_to_num, type_env);
    if(!av)
      return nullptr;
    list_ctor.push_back(av);
  }
  return graph.insertNode(graph.createList(elem_type, list_ctor))->output();
}

template<class T>
static Value* materializeConstant(T val, Graph& graph,
    const SourceRange& r, std::unordered_map<T, Value*>& map) {
  auto existing_constant = map.find(val);
  if (existing_constant != map.end()) {
    return existing_constant->second;
  }

  WithInsertPoint guard(graph.block()->nodes().front());
  auto new_constant = graph.insertConstant(val, r);
  map[val] = new_constant;

  return new_constant;
}

c10::optional<MatchedSchema> tryMatchSchema(
    const FunctionSchema& schema,
    const SourceRange& loc,
    Graph& graph,
    c10::optional<NamedValue> self,
    at::ArrayRef<NamedValue> args,
    at::ArrayRef<NamedValue> kwargs,
    std::ostream& failure_messages,
    bool allow_conversions) {
  auto err = [&]() -> std::ostream& {
    failure_messages << "\nfor operator " << schema << ":\n";
    return failure_messages;
  };

  TypeEnv type_env;
  std::vector<Value*> positional_inputs;
  std::vector<bool> used_kwarg(kwargs.size(), false);

  // if we finish the loop will we have consumed all arguments?
  size_t used_args = 0;
  for (size_t schema_i = 0; schema_i < schema.arguments().size(); ++schema_i) {
    const auto& arg = schema.arguments()[schema_i];
    c10::optional<NamedValue> v;
    if (arg.name() == "self" && self) {
      v = self;
      self = c10::nullopt;
    } else if (!arg.kwarg_only() && used_args < args.size()) {
      // allow zeros(IntList sizes) to work with zeros(1, 2) or zeros(1)
      if (arg.type()->kind() == TypeKind::ListType && // the formal must be a list
          !arg.N() && // it must not be a broadcasting list like int[3], otherwise
                    // a single int is a valid input
          (schema_i + 1 == schema.arguments().size() ||
           schema.arguments()[schema_i + 1]
               .kwarg_only())) { // must be the last position argument
        auto actual_type = args[used_args].value(graph)->type();
        if (actual_type->kind() != TypeKind::ListType &&
            !convertibleToList(
                actual_type,
                unwrapOptional(arg.type()))) { // and the actual should not be a list already
          auto elem_type = unwrapOptional(arg.type())->expect<ListType>()->getElementType();
          Value* list = tryCreateList(
              elem_type,
              graph,
              loc,
              at::ArrayRef<NamedValue>(args).slice(used_args),
              err,
              allow_conversions,
              type_env);
          if (!list)
            return c10::nullopt;
          used_args = args.size();
          positional_inputs.push_back(list);
          continue;
        }
      }

      v = args[used_args];
      used_args++;
    } else if (auto idx = findInputWithName(arg.name(), kwargs)) {
      const NamedValue& nv = kwargs[*idx];
      if (used_kwarg[*idx]) {
        err() << "argument " << nv.name()
              << " specified twice in schema, submit a bug report!\n"
              << nv.locOr(loc);
        return c10::nullopt;
      }
      used_kwarg[*idx] = true;
      v = nv;
    } else if (arg.default_value()) {
      v = NamedValue(*arg.default_value());
    } else {
      err() << "argument " << schema.arguments()[schema_i].name()
            << " not provided.\n"
            << loc;
      return c10::nullopt;
    }
    Value* positional = tryMatchArgument(
        arg, graph, loc, *v, err, allow_conversions, type_env);
    if (!positional)
      return c10::nullopt;
    positional_inputs.push_back(positional);
  }
  // check for unused self argument
  if(self != c10::nullopt) {
    err() << "provided self argument not used in schema\n";
  }

  if (schema.is_vararg()) {
    for(;used_args < args.size(); ++used_args) {
      positional_inputs.push_back(args[used_args].value(graph));
    }
  }

  // check for unused positional arguments
  if (used_args < args.size()) {
    err() << "expected at most " << used_args << " arguments "
          << "but found " << args.size() << " positional arguments.\n"
          << loc << "\n";
    return c10::nullopt;
  }
  // check for unused kwargs
  for (size_t i = 0; i < kwargs.size(); ++i) {
    const auto& nv = kwargs[i];
    if (!used_kwarg[i]) {
      if (!schema.argumentIndexWithName(nv.name())) {
        err() << "keyword argument " << nv.name() << " unknown\n";
      } else {
        err() << "keyword argument " << nv.name() << " specified twice\n";
      }
      return c10::nullopt;
    }
  }
  auto return_types = fmap(schema.returns(), [&](const Argument& r) {
    return evalTypeVariables(r.type(), type_env);
  });
  return MatchedSchema{std::move(positional_inputs), std::move(return_types)};
}

static std::string prefixLine(const std::string& str, const std::string& prefix) {
  std::stringstream ss;
  bool was_newline = true;
  for(auto c : str) {
    if(was_newline)
      ss << prefix;
    ss.put(c);
    was_newline = c == '\n';
  }
  return ss.str();
}

// Given a successful match between operator schema and symbol, emit a node
// with the appropriate inputs and outputs.
static Value* emitBuiltinNode(
    const MatchedSchema& matched_schema,
    const SourceRange& loc,
    Graph& graph,
    Symbol name) {
  auto n = graph.insertNode(graph.create(name, matched_schema.inputs, 0))
                ->setSourceLocation(std::make_shared<SourceRange>(loc));

  for(auto & ret : matched_schema.return_types) {
    n->addOutput()->setType(ret);
  }

  // assert that we did indeed create an op that has implementation
  // otherwise schema and dispatch are not in sync
  getOperation(n);

  return packOutputs(graph, n->outputs());
}

// Search for operators matching the provided symbol name and input types.
// If one is found, emit a node to the graph for that operator.
Value* emitBuiltinCall(
  const SourceRange& loc,
  Graph& graph,
  Symbol name,
  const c10::optional<NamedValue>& self,
  at::ArrayRef<NamedValue> inputs,
  at::ArrayRef<NamedValue> attributes,
  // if true, emitBuiltinCall will throw an exception if this builtin does not exist,
  // otherwise it will return nullptr if the builtin is not found.
  bool required) {


  const auto& variants = getAllOperatorsFor(name);
  const auto& builtin_functions = getAllBuiltinFunctionsFor(name);

  std::stringstream failure_messages;
  //first we try to match the schema without any conversion
  //if no schema matches then insert ImplicitTensorToNum
  for (bool allow_conversions : {false, true}) {
    // clear previous error messages
    failure_messages.str("");
    for (const std::shared_ptr<Operator>& op : variants) {
      const auto matched_schema = tryMatchSchema(
          op->schema(),
          loc,
          graph,
          self,
          inputs,
          attributes,
          failure_messages,
          allow_conversions);
      if (matched_schema) {
        return emitBuiltinNode(*matched_schema, loc, graph, name);
      }
    }
    for (Method* method : builtin_functions) {
      if (auto result = try_emit_call_to(
              graph,
              loc,
              *method,
              self,
              inputs,
              attributes,
              failure_messages,
              nullptr,
              allow_conversions)) {
        return packOutputs(graph, *result);
      }
    }
  }

  // none of the options worked
  if (!required) {
    return nullptr;
  }
  if(variants.size() == 0) {
    throw ErrorReport(loc) << "unknown builtin op";
  }
  throw ErrorReport(loc) << "arguments for call are not valid:\n"
                         << prefixLine(failure_messages.str(), "  ")
                         << "for call at";
}

static Value* ensureInt(const SourceRange& range, Value* v) {
  if(!v->type()->isSubtypeOf(IntType::get())) {
    throw ErrorReport(range) << "expected a int but found a "
                             << v->type()->str();
  }
  return v;
}

std::shared_ptr<SugaredValue> BuiltinFunction::call(
    const SourceRange& loc,
    Method& m,
    at::ArrayRef<NamedValue> inputs,
    at::ArrayRef<NamedValue> attributes,
    size_t n_binders) {
  return std::make_shared<SimpleValue>(emitBuiltinCall(
      loc, *m.graph(), symbol, self, inputs, attributes, true));
}

inline bool isSupportedListElementType(const TypePtr& type) {
  return type->isSubtypeOf(DynamicType::get()) ||
      type->isSubtypeOf(NumberType::get());
}

c10::optional<std::string> parseBaseTypeName(const Expr& expr);
TypePtr parseTypeFromExpr(const Expr& expr);
c10::optional<std::pair<TypePtr, int32_t>> handleBroadcastList(const Expr& expr);

struct to_ir {
  to_ir(
      Def def_,
      Resolver resolver_,
      SugaredValuePtr self_,
      Method& method) // method being constructed
      : method(method)
      , graph(method.graph())
      , def(std::move(def_))
      , resolver(std::move(resolver_))
      , self(std::move(self_))
      , environment_stack(nullptr) {
    JIT_ASSERT(resolver);
    pushFrame(graph->block());

    // Type annotations exclude explicitly typing the "self" parameter, so in the
    // case that this is a method with self we expect one fewer parameter annotation
    // than the number of parameters this Def takes.
    if (self && def.decl().params().size() == 0) {
      throw ErrorReport(def.decl().params().range()) << "methods must have a self argument";
    }
    auto schema = extractSchemaFromDef(def);
    std::vector<Argument> arguments = emitFormalArguments(self, schema);

    // body
    auto stmts = def.statements();
    auto stmts_begin = stmts.begin();
    auto stmts_end = stmts.end();
    c10::optional<Return> return_stmt;
    if (stmts_begin != stmts_end && (*std::prev(stmts_end)).kind() == TK_RETURN) {
      --stmts_end;
      return_stmt = Return(*stmts_end);
    }
    emitStatements(stmts_begin, stmts_end);
    std::vector<Argument> returns = {emitReturn(
        return_stmt ? return_stmt->range() : def.range(), return_stmt, schema)};

    method.setSchema({def.name().name(), std::move(arguments), std::move(returns)});
    // remove any uses of tuples that we inserted that are not needed
    LowerSimpleTuples(graph);
    ConstantPooling(graph);
  }

private:
  Method& method;
  std::shared_ptr<Graph> graph;
  Def def;
  Resolver resolver;
  SugaredValuePtr self;
  std::unordered_map<int64_t, Value*> integral_constants;
  std::unordered_map<double, Value*> fp_constants;

  // Singly-linked list of environments. This top element contains a member
  // `next` that points to the most immediate enclosing scope's value.
  std::shared_ptr<Environment> environment_stack;

  void pushFrame(Block * b) {
    environment_stack = std::make_shared<Environment>(method, resolver, b, environment_stack);
  }
  std::shared_ptr<Environment> popFrame() {
    auto old_frame = environment_stack;
    environment_stack = environment_stack->next;
    return old_frame;
  }

  std::vector<IValue> evaluateDefaults(const SourceRange& r, const std::vector<Expr>& default_types, const std::vector<Expr>& default_exprs) {
    std::vector<IValue> default_values;
    if (default_exprs.empty())
      return default_values;
    // To evaluate the default expressions, we create a graph with no inputs,
    // and whose returns are the default values we need.
    // We then run constant prop on this graph and check the results are constant.
    // This approach avoids having to have separate handling of default arguments
    // from standard expressions by piecing together existing machinery for
    // graph generation, constant propgation, and constant extraction.
    auto tuple_type = Subscript::create(
        r,
        Var::create(r, Ident::create(r, "Tuple")),
        List<Expr>::create(r, default_types));
    auto blank_decl =
        Decl::create(r, List<Param>::create(r, {}), Maybe<Expr>::create(r, tuple_type));

    auto tuple_expr = TupleLiteral::create(r, List<Expr>::create(r, default_exprs));
    auto ret = Return::create(r, tuple_expr);
    auto def = Def::create(
        r,
        Ident::create(r, "defaults"),
        blank_decl,
        List<Stmt>::create(r, {ret}));
    auto m = std::make_shared<Module>();
    defineMethodsInModule(m, {def}, {resolver}, nullptr);
    Stack stack;
    m->get_method("defaults").run(stack);
    return stack.at(0).toTuple()->elements();
  }

  std::vector<Argument> parseArgsFromDecl(const Decl& decl) {
    auto params_begin = decl.params().begin();
    auto params_end = decl.params().end();
    if (self)
      ++params_begin;
    std::vector<Argument> retval;

    std::vector<Expr> default_types;
    std::vector<Expr> default_exprs;
    // gather any non-empty default arguments
    for (auto it = params_begin; it != params_end; ++it) {
      auto param = *it;
      auto def = param.defaultValue();
      if (def.present()) {
        default_types.emplace_back(param.type());
        default_exprs.emplace_back(def.get());
      }
    }
    auto default_values = evaluateDefaults(decl.range(), default_types, default_exprs);

    auto defaults_it = default_values.begin();
    for (auto it = params_begin; it != params_end; ++it) {
      auto decl_arg = *it;

      TypePtr type;
      c10::optional<int32_t> N;

      //BroadcastList list can only appear at the argument level
      if (auto maybe_broad_list = handleBroadcastList(decl_arg.type())) {
        type = maybe_broad_list->first;
        N = maybe_broad_list->second;
      } else {
        type = parseTypeFromExpr(decl_arg.type());
        N = c10::nullopt;
      }
      c10::optional<IValue> default_value = c10::nullopt;
      if (decl_arg.defaultValue().present()) {
        default_value = *defaults_it++;
      }
      auto arg = Argument(
          decl_arg.ident().name(),
          type,
          N,
          default_value,
          /*kwarg_only =*/false);
      retval.push_back(arg);
    }
    return retval;
  }

  std::vector<Argument> parseReturnFromDecl(const Decl& decl) {
    // we represent no annoation on a return type as having no values in the
    // schema's return() list
    // in emitReturn we take the actual return value to be the value of the return
    // statement if no one was provided here
    if(!decl.return_type().present())
      return {};

    if (handleBroadcastList(decl.return_type().get()))
      throw ErrorReport(decl.return_type().range()) << "Broadcastable lists cannot appear as a return type";
    auto parsed_type = parseTypeFromExpr(decl.return_type().get());
    return {Argument(
        "",
        parsed_type,
        /*N =*/c10::nullopt,
        /*default_value =*/c10::nullopt,
        /*kwarg_only =*/false)};
  }
  FunctionSchema extractSchemaFromDef(const Def &def) {
      auto name = def.name().name();
      std::vector<Argument> args = parseArgsFromDecl(def.decl());
      std::vector<Argument> returns = parseReturnFromDecl(def.decl());
      return FunctionSchema(name, std::move(args), std::move(returns), false, false);
  }

  std::vector<Argument> emitFormalArguments(const SugaredValuePtr& self, const FunctionSchema& schema) {
    std::vector<Argument> arguments; // for schema
    // inputs
    auto it = def.decl().params().begin();
    auto end = def.decl().params().end();
    auto expected_annotation_size = self ? def.decl().params().size() - 1 : def.decl().params().size();
    if (schema.arguments().size() != expected_annotation_size) {
      throw ErrorReport(def.decl().params().range()) << "Number of type annotations for"
        << " function parameters (" << schema.arguments().size() << ")"
        << " does not match the number of parameters on the function ("
        << expected_annotation_size << ")!";
    }
    if(self) {
      JIT_ASSERT(it != end);
      environment_stack->setSugaredVar(def.range(), (*it).ident().name(), self);
      ++it;
    }
    size_t arg_annotation_idx = 0;
    for(;it != end; ++it) {
      auto& name = (*it).ident().name();
      // Add the input to the graph
      Value *new_input = graph->addInput();
      if (meaningfulName(name)) {
        new_input->setUniqueName(name);
      }
      environment_stack->setVar((*it).ident().range(), name, new_input);

      // Record the type for the schema and set the Type on the Value*
      arguments.push_back(schema.arguments().at(arg_annotation_idx++));
      new_input->setType(arguments.back().type());
    }
    return arguments;
  }

  Argument emitReturn(const SourceRange& range, c10::optional<Return> return_stmt, const FunctionSchema& schema) {
    JIT_ASSERT(schema.returns().size() <= 1);
    // outputs
    Value* result = return_stmt ? emitExpr(return_stmt->expr())
                                : graph->insertConstant(IValue(), range);
    TypePtr result_type = schema.returns().size() > 0
        ? schema.returns().at(0).type()
        : result->type();

    if (return_stmt) {
      result = tryConvertToType(
          range, *graph, result_type, result, /*allow_conversions=*/true);
    }

    if (!result->type()->isSubtypeOf(result_type)) {
      throw ErrorReport(range) << "Return value was annotated as having type " << result_type->python_str()
        << " but is actually of type " << result->type()->python_str();
    }
    graph->registerOutput(result);
    return Argument("", result_type);
  }
  void emitStatements(const List<Stmt>& statements) {
    return emitStatements(statements.begin(), statements.end());
  }
  void emitStatements(List<Stmt>::const_iterator begin, List<Stmt>::const_iterator end) {
    for (; begin != end; ++begin) {
      auto stmt = *begin;
      switch (stmt.kind()) {
        case TK_IF:
          emitIf(If(stmt));
          break;
        case TK_WHILE:
          emitWhile(While(stmt));
          break;
        case TK_FOR:
          emitFor(For(stmt));
          break;
        case TK_ASSIGN:
          emitAssignment(Assign(stmt));
          break;
        case TK_AUG_ASSIGN:
          emitAugAssignment(AugAssign(stmt));
          break;
        case TK_GLOBAL:
          for (auto ident : Global(stmt).names()) {
            const auto& name = Ident(ident).name();
            environment_stack->setVar(ident.range(), name, graph->addInput(name));
          }
          break;
        case TK_EXPR_STMT: {
          auto expr = ExprStmt(stmt).expr();
          emitSugaredExpr(expr, 0);
        }
        break;
        case TK_RAISE:
          emitRaise(Raise(stmt).range());
          break;
        case TK_ASSERT:
          emitAssert(Assert(stmt));
          break;
        case TK_RETURN:
          throw ErrorReport(stmt) << "return statements can appear only at the end "
                                  << "of the function body";
          break;
        case TK_PASS:
          // Emit nothing for pass
          break;
        default:
          throw ErrorReport(stmt)
              << "Unrecognized statement kind " << kindToString(stmt.kind());
      }
    }
  }

  std::shared_ptr<Environment> emitSingleIfBranch(
      Block* b,
      const List<Stmt>& branch) {
    pushFrame(b);
    WithInsertPoint guard(b);
    emitStatements(branch);
    return popFrame();
  }

  Node* create(Symbol kind, const SourceRange& loc,  size_t n_outputs) {
    return graph
             ->create(kind, n_outputs)
             ->setSourceLocation(std::make_shared<SourceRange>(loc));
  }

  Value* emitTernaryIf(const TernaryIf& expr) {
    Value* cond_value = emitCond(expr.cond());
    auto true_expr = [&] {
      return emitExpr(expr.true_expr());
    };
    auto false_expr  = [&] {
      return emitExpr(expr.false_expr());
    };
    return emitIfExpr(expr.range(), cond_value, true_expr, false_expr);
  }

  Value* emitShortCircuitIf(
      const SourceRange& loc,
      const TreeRef & first_expr,
      const TreeRef & second_expr,
      bool is_or) {
    Value * first_value = emitCond(Expr(first_expr));

    auto get_first_expr = [first_value] {
      return first_value;
    };
    auto get_second_expr = [&] {
      return emitCond(Expr(second_expr));
    };

    // if this is an OR, eval second expression if first expr is False.
    // If this is an AND, eval second expression if first expr is True
    if (is_or) {
      return emitIfExpr(loc, first_value, get_first_expr, get_second_expr);
    } else {
      return emitIfExpr(loc, first_value, get_second_expr, get_first_expr);
    }
  }

  Value* emitIfExpr(const SourceRange& range, Value * cond_value,
      std::function<Value*()> true_expr,  std::function<Value*()> false_expr) {
    Node* n = graph->insertNode(create(prim::If, range, 0));

    n->addInput(cond_value);
    auto* true_block = n->addBlock();
    auto* false_block = n->addBlock();

    auto emit_if_expr = [this](Block* b, std::function<Value*()> expr_value) {
      pushFrame(b);
      WithInsertPoint guard(b);
      Value* out_val = expr_value();
      b->registerOutput(out_val);
      popFrame();
    };

    emit_if_expr(true_block, std::move(true_expr));
    emit_if_expr(false_block, std::move(false_expr));

    auto true_type = unshapedType(true_block->outputs().at(0)->type());
    auto false_type = unshapedType(false_block->outputs().at(0)->type());
    if (*true_type != *false_type) {
      throw ErrorReport(range)
          << "if-expression's true branch has type " << true_type->str()
          << " but false branch has type " << false_type->str();
    }

    // Add op outputs
    auto expr_value = n->addOutput()->setType(true_type); // Resulting value

    return expr_value;
  }

  Value* emitCond(const Expr& cond) {
    Value* v = emitExpr(cond);
    if (!v->type()->isSubtypeOf(BoolType::get())) {
      ErrorReport error(cond);
      error << "expected a boolean expression for condition but found "
            << v->type()->str();
      if (v->type()->isSubtypeOf(DynamicType::get())) {
        error << ", to use a tensor in a boolean"
              << " expression, explicitly cast it with `bool()`";
      }
      throw error;
    }
    return v;
  }

  void emitIfElseBlocks(Value* cond_value, const If& stmt) {
    Node* n = graph->insertNode(create(prim::If, stmt.range(), 0));
    n->addInput(cond_value);
    auto* true_block = n->addBlock();
    auto* false_block = n->addBlock();

    // Emit both blocks once to get the union of all mutated values
    auto save_true = emitSingleIfBranch(true_block, stmt.trueBranch());
    auto save_false = emitSingleIfBranch(false_block, stmt.falseBranch());

    // In python, every variable assigned in an if statement escapes
    // the scope of the if statement (all variables are scoped to the function).
    // Script is a subset of python: we consider variables to be in scope
    // as long as there is a definition of the variable along all paths
    // through the if statemnent
    // ----
    // if ...:
    //   a =
    // else:
    //   ...
    // ... = a  # error, a is not defined along all paths
    // ----
    // if ...:
    //   a =
    // else:
    //   a =
    // ... = a # OK, a is defined along all paths
    // ----
    // a = ...
    // if ...:
    //   a =
    // ... = a # OK, a is defined along all paths


    //ordered set, because we want deterministic graph output
    std::set<std::string> mutated_variables;

    for(auto & v : save_true->definedVariables()) {
      if(save_false->findInAnyFrame(v)) {
        mutated_variables.insert(v);
      }
    }
    for(auto & v : save_false->definedVariables()) {
      if(save_true->findInAnyFrame(v)) {
        mutated_variables.insert(v);
      }
    }

    // Register outputs in each block
    for (const auto& x : mutated_variables) {
      auto tv = save_true->getVar(x, stmt.range());
      auto fv = save_false->getVar(x, stmt.range());
      auto unified = unifyTypes(tv->type(), fv->type());

      // attempt to unify the types. we allow variables to be set to different types
      // in each branch as long as that variable is not already in scope,
      // or if that variable does not get used later. here, we save the error
      // so that the error message will be more informative in the case that is
      // used later. When a is accessed in (a + 1), the error will get printed
      // if cond:
      //    a = 1
      // else:
      //    a = tensor
      // b = a + 1
      //
      if (!unified) {
        ErrorReport error(stmt);
        error << "Type mismatch: " << x << " is set to type " << tv->type()->str() << " in the true branch"
        << " and type " << fv->type()->str() << " in the false branch";
        if (save_true->findInParentFrame(x) || save_false->findInParentFrame(x)) {
          throw error;
        } else {
          // error gets saved in the lowest environment because all
          // variables are scoped to the function. doesn't matter if this accessed
          // through save_true or save_false
          save_true->setVariableTypeError(x, error.what());
          continue;
        }
      }
      true_block->registerOutput(tv);
      false_block->registerOutput(fv);
      environment_stack->setVar(stmt.range(), x, n->addOutput()->setType(*unified));
    }
  }

  void emitIf(const If& stmt) {
    // NOTE: emitIf checks on If stmt condition to see if the cond AST kind == is/is not,
    // for such cases we do meta programming and disable emitting the corresponding branches
    Expr cond = stmt.cond();

    if (cond.kind() != TK_IS && cond.kind() != TK_ISNOT) {
      // emit normal IF stmt for cases except TK_IS and TK_ISNOT
      Value* cond_value = emitCond(cond);
      emitIfElseBlocks(cond_value, stmt);
      return;
    }
    // meta programming on AST for is/is not cases and emit branches base on the possible output of cond
    auto cond_op = BinOp(cond);
    SugaredValuePtr lhs_val = emitSugaredExpr(cond_op.lhs(), 1);
    SugaredValuePtr rhs_val = emitSugaredExpr(cond_op.rhs(), 1);

    List<Stmt> always_none_branch = cond.kind() == TK_IS? stmt.trueBranch(): stmt.falseBranch();
    List<Stmt> never_none_branch = cond.kind() == TK_IS? stmt.falseBranch(): stmt.trueBranch();

    auto lhs_none= lhs_val->isNone();
    auto rhs_none= rhs_val->isNone();

    // Dispatch logic (A: ALWAYS, N: NEVER, M: MAYBE):
    //
    // AA, -> emit always_none_branch
    // AN , NA-> emit never_none_branch
    // MA, MM, MN, NM, NN, AM -> emit both conditional branches

    if (lhs_none == ALWAYS && rhs_none == ALWAYS) {
      // None is/is not None: only emit the always_none_branch
      emitStatements(always_none_branch);
    } else if ((lhs_none == ALWAYS && rhs_none == NEVER) ||
        (lhs_none == NEVER && rhs_none == ALWAYS)){
      // lhs_val/rhs_val with A/M: only emit never_none_branch
      emitStatements(never_none_branch);
    }
    else {
      // all other cases for lhs_val and rhs_val
      // emit the whole If stmt as usual, finish emitCond first
      auto lhs_range = cond_op.lhs().get()->range();
      auto rhs_range = cond_op.rhs().get()->range();
      auto kind = getNodeKind(cond.kind(), cond.get()->trees().size());
      Value* cond_value = emitBuiltinCall(
          cond.get()->range(),
          *method.graph(),
          kind,
          c10::nullopt,
          {lhs_val->asValue(lhs_range, method), rhs_val->asValue(rhs_range, method)},
          {},
          /*required=*/true);
      emitIfElseBlocks(cond_value, stmt);

    }

  }

  // *********************** Loop Operators ************************************
  // Emits a loop operators conforming to the semantics specified at
  // https://github.com/onnx/onnx/blob/master/docs/Operators.md#experimental-loop
  // TODO: implement scan_outputs

  // the format of the Loop instruction is:
  // loop_carried_outputs* = Loop(max_trip_count, start_condition,
  // loop_carried_inputs*)
  //                          block0(loop_counter, loop_carried_block*) {
  //                             <body>
  //                             -> (continue_condition,
  //                             loop_carried_block_outputs*)
  //                          }
  // all loop_carried_... lists are the same length and represent the value of
  // loop-carried variables whose definitions are updated as the loop executes
  // in a way that ensure single static assignment.

  void emitLoopCommon(
      SourceRange range,
      c10::optional<Expr> max_trip_count,
      c10::optional<Expr> cond,
      const List<Stmt>& body,
      c10::optional<Ident> itr_ident) {
    Node* n = graph->insertNode(create(prim::Loop, range, 0));
    Value *max_trip_count_val, *cond_val;
    {
      WithInsertPoint guard(n);
      if (max_trip_count) {
        max_trip_count_val = ensureInt(
            max_trip_count->range(), emitExpr(max_trip_count.value()));
      } else {
        max_trip_count_val =
            materializeConstant(std::numeric_limits<int64_t>::max(), *graph, range, integral_constants);
      }
      if (cond) {
        cond_val = emitCond(cond.value());
      } else {
        cond_val = graph->insertConstant(true, range);
      }
    }
    n->addInput(max_trip_count_val);
    n->addInput(cond_val);
    auto* body_block = n->addBlock();
    Value* trip_count = body_block->addInput()->setType(IntType::get()); // Iteration num

    {
      pushFrame(body_block);
      if (itr_ident) {
        environment_stack->setVar(itr_ident->range(), itr_ident->name(), trip_count);
      }
      WithInsertPoint guard(body_block);
      emitStatements(body);

      // Also emit the conditional
      if (cond) {
        Value* body_cond_value = emitCond(cond.value());
        body_block->registerOutput(body_cond_value);
      } else {
        Value* cond_value_dummy = graph->insertConstant(true, range);
        body_block->registerOutput(cond_value_dummy);
      }

      auto body_frame = popFrame();
      auto outer_frame = environment_stack;

      // Add block outputs to correspond to each captured input
      // some of these will be removed.
      for (const auto& x : body_frame->captured_inputs) {
        auto fv = body_frame->getValueInThisFrame(range, x);
        body_block->registerOutput(fv);
      }

      // Remove inputs for values that did not mutate within the
      // block
      body_frame->deleteExtraInputs(range);

      // register node inputs/outputs for the true loop carried deps,
      for(size_t i = 0; i < body_frame->captured_inputs.size(); ++i) {
        auto x = body_frame->captured_inputs[i];
        n->addInput(outer_frame->getVar(x, range));
        // body_block->inputs(): loop_counter, lcd0, lcd1, ...
        // captured_inputs: lcd0, lcd1, ...
        auto typ = body_block->inputs()[i + 1]->type();
        outer_frame->setVar(range, x, n->addOutput()->setType(typ));
      }

    }
  }

  void emitForRange(const SourceRange& range, const Ident& target, const List<Expr>& args, const List<Stmt>& body) {
    // TODO: start, stop, step loop
    if (args.size() != 1) {
      throw ErrorReport(range)
          << "range() expects 1 argument but got " << args.size();
    }
    emitLoopCommon(range, {args[0]}, {}, body, target);
  }

  void emitFor(const For& stmt) {
    // For now, we only support range loops. e.g. for i in range(3): ...
    auto targets = stmt.targets();
    auto itrs = stmt.itrs();
    auto body = stmt.body();

    if (stmt.itrs().size() != 1) {
      throw ErrorReport(stmt)
          << "List of iterables is not supported currently.";
    }
    if (targets.size() != 1) {
      throw ErrorReport(stmt) << "Iteration variable unpacking is not supported";
    }

    if (targets[0].kind() != TK_VAR) {
      throw ErrorReport(targets[0]) << "unexpected expression in variable initialization of for loop";
    }
    auto target = Var(targets[0]).name();

    // match range(<expr>) style loops
    // itrs must consist of a single Apply node
    if (itrs[0].kind() == TK_APPLY) {
      Apply range_iterator = Apply(itrs[0]);
      if (range_iterator.callee().kind() == TK_VAR) {
        Var var = Var(range_iterator.callee());
        if (var.name().name() == "range") {
          return emitForRange(stmt.range(), target, range_iterator.inputs(), body);
        }
      }
    }

    // it isn't a range(<expr>) loop, treat it as a sugared value that maybe can be
    // unrolled
    auto sv = emitSugaredExpr(itrs[0], 1);
    auto instances = sv->asTuple(stmt.range(), method);
    const std::string& target_name = target.name();
    pushFrame(environment_stack->block());
    for(const auto& inst : instances) {
      environment_stack->setSugaredVar(itrs[0].range(), target_name, inst);
      emitStatements(body);
    }

    for (const auto & n : environment_stack->definedVariables()) {
      if (environment_stack->findInParentFrame(n)) {
        environment_stack->next->setVar(stmt.range(), n, environment_stack->getVar(n, stmt.range()));
      }
    }
    popFrame();
  }

  void emitWhile(const While& stmt) {
    auto cond = stmt.cond();
    emitLoopCommon(stmt.range(), {}, {cond}, stmt.body(), {});
  }


  // Currently we do not support assigning exceptions to variables,
  // a = Exception("hi")
  // raise a
  //
  // We ignore the expression following raise
  //
  // NYI: add exception logic to control-flow nodes
  // if True:
  //   a = 1
  // else
  //   raise Exception("Hi")
  // print(a)
  void emitRaise(const SourceRange& loc) {
    const std::string exception = "Exception";
    auto string_input = insertConstant(*graph, exception, loc);
    graph->insert(prim::RaiseException, {string_input}, {}, loc);
  }

  void emitAssert(const Assert& stmt) {
    Value* cond_value = emitCond(stmt.test());
    Node* n = graph->insertNode(create(prim::If, stmt.range(), 0));

    n->addInput(cond_value);
    /* true_block =*/n->addBlock();
    auto* false_block = n->addBlock();

    //if assert test is false throw exception
    pushFrame(false_block);
    WithInsertPoint guard(false_block);
    emitRaise(stmt.range());
    popFrame();
  }


  // Validate that the `lhs` Expr's in an assignment statement are valid. That
  // is:
  //
  // 1) All lhs Expr's are either Var or Starred nodes
  // 2) There is at most one Starred node in the lhs Expr
  // 3) A Starred node can only appear when there is another non-Starred lhs Expr
  //    Concretely this means that `*abc = func()` is illegal. Unpacking all
  //    outputs into a tuple is covered by `abc = func()`.
  bool calcNumStarredUnpack(const List<Expr>& lhs, const SourceRange& r) {
    size_t num_normal_assign = 0;
    size_t num_starred = 0;
    for (const auto& assignee : lhs) {
      if (assignee.kind() == TK_VAR || assignee.kind() == TK_SUBSCRIPT) {
        num_normal_assign++;
      } else if (assignee.kind() == TK_STARRED) {
        num_starred++;
      } else {
        throw ErrorReport(assignee) << "lhs of assignment must be a variable, "
                                    << "subscript, or starred expression.";
      }
    }

    if (num_starred > 1) {
      throw ErrorReport(r)
          << "Only one starred expression is allowed on the lhs.";
    }

    if (num_starred > 0 && num_normal_assign == 0) {
      throw ErrorReport(r) << "A Starred expression may only appear on the "
                              << "lhs within the presence of another non-starred"
                              << " expression.";
    }

    return num_starred;
  }

  // Get the appropriate builtin op for this augmented assignment
  // If the RHS is a tensor, return the corresponding ATen in-place op
  // If it's a list of scalars, then return the corresponding list augment op
  Symbol getAugOp(const AugAssign& stmt, bool isTensor) {
      switch (stmt.aug_op()) {
        case '+':
          return isTensor ? aten::add_ : aten::add;
        case '-':
          return isTensor ? aten::sub_ : aten::sub;
        case '/':
          return isTensor ? aten::div_ : aten::div;
        case '*':
          return isTensor ? aten::mul_ : aten::mul;
        default:
          throw ErrorReport(stmt) << "Unknown augmented assignment: "
                                  << kindToString(stmt.aug_op());
      }
  }

  // Emit nodes for augmented assignments like `+=`
  void emitAugAssignment(const AugAssign& stmt) {
    switch (stmt.lhs().kind()) {
      case TK_VAR: {
        emitAugAssignmentToVar(stmt);
      } break;
      case '.': {
        emitAugAssignmentToSelectVar(stmt);
      } break;
      case TK_SUBSCRIPT: {
        emitAugAssignmentToSubscript(stmt);
      } break;
      default:
        throw ErrorReport(stmt.lhs())
            << "unexpected expression on "
            << "left-hand side of augmented assignment.";
    }
  }

  // This will be called when there is a class param or module buffer
  // mutation which make the LHS of the expr be a select expression
  //
  // Example like:
  // class A(Module):
  //  def __init__():
  //    self.register_buffer("running_var", torch.zeros(1))
  //
  //  def forward():
  //    self.num_batches += 1
  //
  // In this case we will only consider the scenario that the module
  // buffer type is a tensor, and we emit the corresponding tensor
  // in place op, and throw error for other unsupported types
  void emitAugAssignmentToSelectVar(const AugAssign& stmt) {
    const auto lhs = Select(stmt.lhs());
    const auto lhsSugaredVar = environment_stack->getSugaredVar(Var(lhs.value()).name());
    const auto lhsValue = lhsSugaredVar->attr(lhs.range(), method, lhs.selector().name())->asValue(lhs.range(), method);
    if (lhsValue->type()->isSubtypeOf(DynamicType::get())) {
      // for module parameter/buffer assignment, only consider tensor types,
      // emit the corresponding in-place op
      const auto rhs = NamedValue(stmt.rhs().range(), emitExpr(stmt.rhs()));
      const auto self = NamedValue(stmt.lhs().range(), "self", lhsValue);
      emitBuiltinCall(
          stmt.range(),
          *method.graph(),
          getAugOp(stmt, /*isTensor=*/true),
          self,
          {rhs},
          {},
          /*required=*/true);

    } else {
        throw ErrorReport(stmt.lhs())
            << "left-hand side of augmented assignment to module "
            << "parameters/buffers can only be tensor types";
    }
  }

  void emitAugAssignmentToVar(const AugAssign& stmt) {
    const auto lhs = Var(stmt.lhs());
    const auto lhsValue = environment_stack->getSugaredVar(lhs.name())
                              ->asValue(lhs.range(), method);
    if (lhsValue->type()->isSubtypeOf(DynamicType::get())) {
      // for tensors, emit the corresponding in-place op
      const auto rhs = NamedValue(stmt.rhs().range(), emitExpr(stmt.rhs()));
      const auto self = NamedValue(stmt.lhs().range(), "self", lhsValue);
      const auto output = emitBuiltinCall(
          stmt.range(),
          *method.graph(),
          getAugOp(stmt, /*isTensor=*/true),
          self,
          {rhs},
          {},
          /*required=*/true);

      environment_stack->setVar(lhs.range(), lhs.name().name(), output);
    } else {
      // for primitive types, desugar into a simple assignment
      //   e.g. foo += 1 becomes foo.2 = foo + 1
      Ident lhs = Var(stmt.lhs()).name();
      Expr expr = BinOp::create(
          stmt.range(),
          stmt.aug_op(),
          Var::create(lhs.range(), lhs),
          stmt.rhs());
      environment_stack->setVar(lhs.range(), lhs.name(), emitExpr(expr));
    }
  }

  void emitAugAssignmentToSubscript(const AugAssign& stmt) {
    // Process the base list value
    const auto lhs = Subscript(stmt.lhs());
    const auto sliceable = emitExpr(lhs.value());

    if (sliceable->type()->isSubtypeOf(DynamicType::get())) {
      // If it's a tensor, just fully evaluate the subscript operation and emit
      // an in-place assignment
      std::vector<Value*> tensorIndices;
      Value* sliced;
      std::tie(sliced, tensorIndices) = emitIntAndSliceIndexing(
          lhs.range(), sliceable, lhs.subscript_exprs());

      const auto slicedArg = NamedValue(stmt.lhs().range(), "self", sliced);
      const auto rhs = NamedValue(stmt.rhs().range(), emitExpr(stmt.rhs()));
      if (tensorIndices.size() == 0) {
        // Common case: we only tried to index with int and slices. Emit the
        // correct augmented assignment op to the sliced value
        emitBuiltinCall(
            stmt.range(),
            *method.graph(),
            getAugOp(stmt, /*isTensor=*/true),
            slicedArg,
            {rhs},
            {},
            /*required=*/true);
      } else {
        // Special case: we tried to do "advanced indexing". Lower this expr
        // into `index` and `index_put_` ops
        const auto indices = graph->insertNode(
          graph->createList(DynamicType::get(), tensorIndices))->output();
        const auto indexed =
            graph->insert(aten::index, {slicedArg, indices}, {}, stmt.range());
        const auto augmented = emitBuiltinCall(
            stmt.range(),
            *method.graph(),
            getAugOp(stmt, /*isTensor=*/true),
            indexed,
            {rhs},
            {},
            /*required=*/true);
        graph->insert(
            aten::index_put_,
            {slicedArg, indices, augmented},
            {},
            stmt.range());
      }
    } else {
      // Otherwise, it should be a list.  Lower this expression into:
      //     list.set_item(get_item(idx).add_(value))
      // similar to how Python handles things.
      const auto listType = sliceable->type()->cast<ListType>();
      JIT_ASSERT(listType != nullptr);

      bool isTensorList =
          listType->getElementType()->isSubtypeOf(DynamicType::get());

      // Get the idx to augment
      const auto subscriptExprs = lhs.subscript_exprs();
      if (subscriptExprs.size() != 1) {
        throw ErrorReport(subscriptExprs)
            << "Sliced expression not yet supported for"
            << " subscripted list augmented assignment. "
            << "File a bug if you want this.";
      }
      const auto idxValue = emitExpr(subscriptExprs[0]);

      const auto listArg = NamedValue(lhs.value().range(), "list", sliceable);
      const auto idxArg = NamedValue(subscriptExprs.range(), "idx", idxValue);
      const auto valueArg =
          NamedValue(stmt.rhs().range(), "value", emitExpr(stmt.rhs()));

      const auto getItem =
          graph->insert(aten::select, {listArg, idxArg}, {}, stmt.range());
      const auto augmentedItem = graph->insert(
          getAugOp(stmt, isTensorList), {getItem, valueArg}, {}, stmt.range());
      graph->insert(
          aten::_set_item, {listArg, idxArg, augmentedItem}, {}, stmt.range());
    }
  }

  // Emit mutating assignments like `foo[0] = bar`
  void emitSubscriptAssign(
      const SourceRange& stmtRange,
      const Subscript& lhs,
      const Expr& rhs) {
    emitSubscriptAssign(
        stmtRange, lhs, NamedValue(rhs.range(), emitExpr(rhs)));
  }

  void emitSubscriptAssign(
      const SourceRange& stmtRange,
      const Subscript& lhs,
      const NamedValue& rhs) {
    // First check the base value.
    auto sliceable = emitExpr(lhs.value());

    // If it's a tensor, copy the RHS data into it
    if (sliceable->type()->isSubtypeOf(DynamicType::get())) {
      std::vector<Value*> tensorIndices;
      Value* sliced;
      // Handle multi-dimensional slicing: first emit int/slice indexing
      // TODO: the Python equivalent code has special-cased copy_to
      // broadcasting to match NumPy semantics (see PR#4853). We can't
      // replicate that without knowing the size of the Tensor; so really that
      // code should be moved into the aten function
      std::tie(sliced, tensorIndices) = emitIntAndSliceIndexing(
          lhs.range(), sliceable, lhs.subscript_exprs());

      const auto slicedArg = NamedValue(lhs.range(), sliced);
      if (tensorIndices.size() == 0) {
        // Common case: we only tried to index with int and slices. Copy the
        // RHS into the resulting tensor.
        graph->insert(aten::copy_, {slicedArg, rhs}, {}, stmtRange);
      } else {
        // Special case: we tried to do "advanced indexing" with a tensor.
        // Dispatch to `aten::index_put_`.
        const auto indices = graph->insertNode(
          graph->createList(DynamicType::get(), tensorIndices))->output();

        graph->insert(
            aten::index_put_, {slicedArg, indices, rhs}, {}, stmtRange);
      }

    // Otherwise, this is a list. Dispatch to aten::_set_item to both select and
    // assign
    } else {
      const auto subscript = lhs.subscript_exprs();
      if (subscript.size() != 1 || subscript[0].kind() == TK_SLICE_EXPR) {
        throw ErrorReport(subscript)
            << "Sliced expression not yet supported for"
            << " subscripted list assignment. "
            << "File a bug if you want this.";
      }

      std::vector<NamedValue> args;
      args.emplace_back(lhs.value().range(), "list", sliceable);
      args.emplace_back(
          lhs.subscript_exprs().range(), "idx", emitExpr(subscript[0]));
      args.push_back(rhs);

      graph->insert(aten::_set_item, args, {}, stmtRange);
    }
  }

  void emitTupleAssign(const TupleLiteral& tl, const Expr& rhs) {
    size_t n_binders = tl.inputs().size();
    bool starred_unpack = calcNumStarredUnpack(tl.inputs(), tl.range());
    if(starred_unpack)
      n_binders--;
    auto output = emitSugaredExpr(rhs, n_binders);
    auto outputs = output->asTuple(
        rhs.range(),
        method,
        starred_unpack ? c10::nullopt : c10::optional<size_t>{n_binders});
    if(outputs.size() < n_binders) {
      throw ErrorReport(tl)
        << "need " << (starred_unpack ? "at least " : "")
        << n_binders << " values to unpack but found only "
        << outputs.size();
    }
    if(outputs.size() > n_binders && !starred_unpack) {
      throw ErrorReport(tl)
      << "too many values to unpack: need " << n_binders << " but found "
      << outputs.size();
    }
    int i = 0;
    for (auto assignee : tl.inputs()) {
      switch (assignee.kind()) {
        case TK_SUBSCRIPT:
          emitSubscriptAssign(
              rhs.range(),
              Subscript(assignee),
              NamedValue(
                  rhs.range(), outputs.at(i)->asValue(rhs.range(), method)));
          i++;
          break;
        case TK_VAR:
          environment_stack->setSugaredVar(assignee.range(), Var(assignee).name().name(), outputs.at(i));
          i++;
          break;
        case TK_STARRED: {
          auto var = Starred(assignee).expr();
          if (var.kind() != TK_VAR) {
            throw ErrorReport(var) << "Cannot pack a tuple into a non-variable.";
          }
          size_t n_matched = outputs.size() - n_binders;
          ArrayRef<std::shared_ptr<SugaredValue>> outputs_ref = outputs;
          auto values = fmap(outputs_ref.slice(i, n_matched), [&](const std::shared_ptr<SugaredValue>& v) {
            return v->asValue(assignee.range(), method);
          });
          auto tup = graph->insertNode(graph->createTuple(values))->output();
          environment_stack->setVar(
            var.range(), Var(var).name().name(), tup);
          i += n_matched;
        } break;
        default:
        throw ErrorReport(assignee) << "unexpected expression on the left-hand side";
      }
    }
  }

  void emitAssignment(const Assign& stmt) {
    switch(stmt.lhs().kind()) {
      case TK_VAR: {
        auto v = Var(stmt.lhs());
        environment_stack->setSugaredVar(
            v.range(), v.name().name(), emitSugaredExpr(stmt.rhs(), 1));
      } break;
      case TK_TUPLE_LITERAL:
        emitTupleAssign(TupleLiteral(stmt.lhs()), stmt.rhs());
        break;
      case TK_SUBSCRIPT:
        emitSubscriptAssign(stmt.range(), Subscript(stmt.lhs()), stmt.rhs());
        break;
      default:
        throw ErrorReport(stmt.lhs()) << "unexpected expression on left-hand side of assignment.";
    }
  }

  NodeKind getNodeKind(int kind, int ninputs) {
    switch (kind) {
      case '+':
        return aten::add;
      case '-':
        return aten::sub;
      case TK_UNARY_MINUS:
        return aten::neg;
      case '*':
        return aten::mul;
      case TK_POW:
        return aten::pow;
      case '@':
        return aten::matmul;
      case TK_STARRED:
        return prim::Starred;
      case '/':
        return aten::div;
      case '%':
        return aten::remainder;
      case TK_NE:
        return aten::ne;
      case TK_EQ:
        return aten::eq;
      case '<':
        return aten::lt;
      case '>':
        return aten::gt;
      case TK_LE:
        return aten::le;
      case TK_GE:
        return aten::ge;
      case TK_AND:
        return aten::__and__;
      case TK_OR:
        return aten::__or__;
      case TK_IS:
        return aten::__is__;
      case TK_ISNOT:
        return aten::__isnot__;
      case TK_NOT:
        return aten::__not__;
      case TK_FLOOR_DIV:
        return aten::floordiv;
      case '&':
        return aten::__and__;
      case '|':
        return aten::__or__;
      case '^':
        return aten::__xor__;
      default:
        throw std::runtime_error("unknown kind " + std::to_string(kind));
    }
  }



  std::vector<NamedValue> getNamedValues(
      const TreeList& trees,
      bool maybe_unpack) {
    std::vector<NamedValue> values;
    for (const auto& tree : trees) {
      if(maybe_unpack && tree->kind() == TK_STARRED) {
        auto starred = Starred(tree);
        auto entries = emitSugaredExpr(starred.expr(), 1)->asTuple(starred.range(), method);
        for(const auto& entry : entries) {
          values.emplace_back(
              tree->range(), entry->asValue(starred.range(), method));
        }
      } else {
        values.emplace_back(tree->range(), emitExpr(Expr(tree)));
      }
    }
    return values;
  }
  std::vector<NamedValue> getNamedValues(
      const List<Expr>& trees,
      bool maybe_unpack) {
    return getNamedValues(trees.tree()->trees(), maybe_unpack);
  }

  std::vector<Value*> getValues(
      const TreeList& trees,
      bool maybe_unpack) {
    return toValues(*graph, getNamedValues(trees, maybe_unpack));
  }
  std::vector<Value*> getValues(
      const List<Expr>& trees,
      bool maybe_unpack) {
    return getValues(trees.tree()->trees(), maybe_unpack);
  }

  std::vector<NamedValue> emitAttributes(const List<Attribute>& attributes) {
    return fmap(attributes, [&](const Attribute& attr) {
      return NamedValue(attr.range(), attr.name().name(), emitExpr(attr.value()));
    });
  }

  void checkApplyExpr(Apply& apply, SourceRange& loc) {
    if (apply.inputs().size() != 2) {
      throw ErrorReport(loc)
          << Var(apply.callee()).name().name()
          << " expected exactly two arguments but found "
          << apply.inputs().size();
    }
    if (apply.attributes().size() > 0) {
      throw ErrorReport(loc)
          << Var(apply.callee()).name().name()
          << " takes no keyword arguments";
    }
  }

  std::shared_ptr<SugaredValue> emitApplyExpr(Apply &apply, size_t n_binders) {
    auto sv = emitSugaredExpr(apply.callee(), 1);
    auto loc = apply.callee().range();
    if (auto fork_value = dynamic_cast<ForkValue*>(sv.get())) {
      auto& trees = apply.inputs().tree()->trees();
      if (trees.size() < 1) {
        throw ErrorReport(loc) << "Expected at least one argument to fork()";
      }

      auto forked = emitSugaredExpr(Expr(trees[0]), 1);
      TreeList sliced_trees(trees.begin() + 1, trees.end());
      auto inputs = getNamedValues(sliced_trees, true);
      auto attributes = emitAttributes(apply.attributes());
      return emitForkExpr(loc, forked, inputs, attributes);
    } else if (auto annotate_value = dynamic_cast<AnnotateValue*>(sv.get())) {
      checkApplyExpr(apply, loc);
      TypePtr type = parseTypeFromExpr(apply.inputs()[0]);
      Value* expr = tryConvertToType(
          apply.range(),
          *graph,
          type,
          emitExpr(apply.inputs()[1], type),
          /*allow_conversions=*/true);
      if (!expr->type()->isSubtypeOf(type)) {
        throw ErrorReport(apply.inputs())
            << "expected an expression of type " << type->python_str()
            << " but found " << expr->type()->python_str();
      }
      return std::make_shared<SimpleValue>(expr);
    } else if(auto getattr = dynamic_cast<GetAttrValue*>(sv.get())) {
      checkApplyExpr(apply, loc);
      auto obj = emitSugaredExpr(apply.inputs()[0], 1);
      auto selector = apply.inputs()[1];
      if (selector.kind() != TK_STRINGLITERAL) {
        throw ErrorReport(loc) << "getattr's second argument must be a string literal";
      }
      const std::string& name = StringLiteral(selector).text();
      return obj->attr(apply.range(), method, name);
    } else if (auto isinstance = dynamic_cast<IsInstanceValue*>(sv.get())) {
      // NOTE: for `isinstance` builtin call in JIT, we only check the static types
      // on the inputs to evaluate, and insert the corresponding constant node
      std::function<bool(Expr, Expr)> isInstanceCheck = [&](Expr obj, Expr classinfo) {
        if (classinfo.kind() == TK_TUPLE_LITERAL) {
          // handle the case for recursive tuple classinfo
          // return true if obj is an instance of any of the types
          for (Expr e: TupleLiteral(classinfo).inputs()) {
            if (isInstanceCheck(obj, e)) {
              return true;
            }
          }
          return false;
        }
        auto type_name = parseBaseTypeName(classinfo);
        if (!type_name) {
          throw ErrorReport(classinfo.range()) << "type must be a type identifier";
        }
        auto val = emitExpr(obj);
        // Special casing for list and tuple since isintance(x, list) and isinstance(x, tuple)
        // does not accept List[int] / Tuple[int] like subscript type annotation in python
        if (*type_name == "list" && val->type()->cast<ListType>()) {
          return true;
        } else if (*type_name == "tuple" && val->type()->cast<TupleType>()) {
          return true;
        } else if (val->type()->cast<OptionalType>()) {
          throw ErrorReport(loc)
                << "Optional isinstance check is not supported, consider use is/isnot None instead";
        } else {
          TypePtr type = parseTypeFromExpr(classinfo);
          if (val->type()->isSubtypeOf(type)) {
            return true;
          }
        }
        return false;
      };
      checkApplyExpr(apply, loc);
      bool is_instance_val = isInstanceCheck(apply.inputs()[0], apply.inputs()[1]);
      return std::make_shared<SimpleValue>(graph->insertConstant(is_instance_val, loc));
    } else {
      auto inputs = getNamedValues(apply.inputs(), true);
      auto attributes = emitAttributes(apply.attributes());
      return sv->call(loc, method, inputs, attributes, n_binders);
    }
  }

  Value* emitExpr(const Expr& tree, TypePtr type_hint = nullptr) {
    return emitSugaredExpr(tree, 1, std::move(type_hint))->asValue(tree.range(), method);
  }

  NodeKind reverseComparision(NodeKind kind) {
    if (kind == aten::lt) {
      return aten::gt;
    } else if (kind == aten::le) {
      return aten::ge;
    } else if (kind == aten::gt) {
      return aten::lt;
    } else if (kind == aten::ge) {
      return aten::le;
    }
    throw std::runtime_error("reverseComparision: unsupported NodeKind. File a bug");
  }

  // any expression that can produce a SugaredValue is handled here
  // expressions that only return a single Value* are handled in emitSimpleExpr
  // type_hint is set if there is a type that this value is expected to be
  // e.g. a : List[int] = []
  // or a = torch.jit.annotate(List[int], [])
  // the caller is responsible for checking that the result matches type_hint
  // emitSugaredExpr is free to ignore it.
  std::shared_ptr<SugaredValue> emitSugaredExpr(const Expr& tree, size_t n_binders, TypePtr type_hint=nullptr) {
    switch(tree.kind()) {
      case TK_VAR:
        return environment_stack->getSugaredVar(Var(tree).name());
      case '.': {
        auto select = Select(tree);
        auto sv = emitSugaredExpr(select.value(), 1);
        return sv->attr(select.range(), method, select.selector().name());
      }
      case TK_APPLY: {
        auto apply = Apply(tree);
        return emitApplyExpr(apply, n_binders);
      } break;
      default:
        return std::make_shared<SimpleValue>(emitSimpleExpr(tree, std::move(type_hint)));
    }
  }

  Value * emitNegate(const TreeRef& tree) {
    const auto& inputs = tree->trees();
    auto named_values = getNamedValues(inputs, /*maybe_unpack=*/false);

    auto neg_val = emitBuiltinCall(
               tree->range(),
               *method.graph(),
               aten::neg,
               c10::nullopt,
               named_values,
               {},
               /*required=*/true);

    // constant fold the input if possible
    auto maybe_constant_input = toIValue(neg_val->node()->input());
    if (!maybe_constant_input) {
      return neg_val;
    }
    auto op = getOperation(neg_val->node());
    Stack stack;
    stack.push_back(*maybe_constant_input);
    op(stack);
    JIT_ASSERT(stack.size() == 1);
    return graph->insertConstant(stack[0], tree->range());
  }

  // This function extract a new graph from its original subgraph
  std::shared_ptr<SugaredValue> emitForkExpr(
      SourceRange loc,
      const std::shared_ptr<SugaredValue> &forked,
      at::ArrayRef<NamedValue> inputs,
      at::ArrayRef<NamedValue> attributes) {
    // Build the fork node without inputs
    auto fork_node = method.graph()->insertNode(method.graph()->create(prim::fork, 1))
                ->setSourceLocation(std::make_shared<SourceRange>(loc));
    auto body_block = fork_node->addBlock();

    // Build a template of the graph to be executed
    Value *node_output;
    {
      WithInsertPoint guard(body_block);
      auto fn_sugared_output = forked->call(loc, method, inputs, attributes, 1);
      auto fn_simple_output = fn_sugared_output->asValue(loc, method);
      body_block->registerOutput(fn_simple_output);
      node_output = fork_node->output()->setType(FutureType::create(fn_simple_output->type()));
    }

    // Fork a new graph from its orignal owning graph
    auto forked_graph = std::make_shared<Graph>();

    // Make sure we capture everything in the new graph.
    // The uncaptured values will be added to the fork signature.
    std::unordered_map<Value*, Value*> uncaptures_map;
    auto env = [&](Value* v) -> Value* {
      if (!uncaptures_map.count(v)) {
        // Capture values for both graphs
        uncaptures_map[v] = forked_graph->addInput()->copyMetadata(v);
        fork_node->addInput(v);
      }
      return uncaptures_map[v];
    };
    forked_graph->block()->cloneFrom(body_block, env);

    // Separate the subgraph and clean up the orignal one
    fork_node->g_(attr::Subgraph, forked_graph);
    fork_node->eraseBlock(0);

    return std::make_shared<SimpleValue>(node_output);
  }

  Value* emitSimpleExpr(
      const TreeRef& tree,
      const TypePtr& type_hint = nullptr) {
    switch (tree->kind()) {
      case '@':
      case TK_POW:
      case TK_IS:
      case TK_ISNOT:
      case TK_NOT:
      case TK_NE:
      case TK_EQ:
      case '<':
      case '>':
      case TK_LE:
      case TK_GE:
      case '*':
      case '/':
      case '+':
      case '-':
      case '%':
      case '&':
      case '|':
      case '^':
      case TK_FLOOR_DIV: {
        const auto& inputs = tree->trees();
        auto kind = getNodeKind(tree->kind(), inputs.size());
        auto named_values = getNamedValues(inputs, /*maybe_unpack=*/false);
        return emitBuiltinCall(
                   tree->range(),
                   *method.graph(),
                   kind,
                   c10::nullopt,
                   named_values,
                   {},
                   /*required=*/true);
      }
      case TK_UNARY_MINUS: {
        return emitNegate(tree);
      }
      case TK_AND:
      case TK_OR: {
        const auto& inputs = tree->trees();
        return emitShortCircuitIf(
          tree->range(),
          inputs[0],
          inputs[1],
          tree->kind() == TK_OR);
      }
      case TK_STARRED: {
        throw ErrorReport(tree) << "Unexpected starred expansion. File a bug report.";
      }
      case TK_CONST: {
        return emitConst(Const(tree));
      } break;
      case TK_TRUE: {
        return graph->insertConstant(true, tree->range());
      } break;
      case TK_FALSE: {
        return graph->insertConstant(false, tree->range());
      } break;
      case TK_NONE: {
        return graph->insertConstant(IValue(), tree->range());
      } break;
      case TK_SUBSCRIPT: {
        return emitSubscript(Subscript(tree));
      } break;
      case TK_IF_EXPR: {
        return emitTernaryIf(TernaryIf(tree));
      } break;
      case TK_STRINGLITERAL: {
        return emitStringLiteral(StringLiteral(tree));
      } break;
      case TK_LIST_LITERAL: {
        auto ll = ListLiteral(tree);
        auto values = getValues(ll.inputs(), /*maybe_unpack=*/true);

        // determine the element type of the list
        // if we have a type hint of List[T], use T
        // if the list is non-empty use type_of(list[0])
        // otherwise assume it is List[Tensor]
        TypePtr elem_type = DynamicType::get();
        if (type_hint && type_hint->kind() == TypeKind::ListType) {
          elem_type = type_hint->expect<ListType>()->getElementType();
        } else if (!values.empty()) {
          elem_type = values.at(0)->type();
        }
        for (auto v : values) {
          if (v->type() != elem_type) {
            throw ErrorReport(tree)
                << "Lists must contain only a single type, expected: "
                << *elem_type << " but found " << *v->type() << " instead";
          }
        }
        Value* result = graph->insertNode(graph->createList(elem_type, values))
            ->output();
        return result;
      } break;
      case TK_TUPLE_LITERAL: {
        auto ll = TupleLiteral(tree);
        auto values = getValues(ll.inputs(), /*maybe_unpack=*/true);
        return graph->insertNode(graph->createTuple(values))->output();
      } break;
      default:
        throw ErrorReport(tree) << "NYI: " << tree;
        break;
    }
  }

  Value* emitConst(const Const& c) {
    if (c.isFloatingPoint())
      return materializeConstant(c.asFloatingPoint(), *graph, c.range(), fp_constants);
    else
     return materializeConstant(c.asIntegral(), *graph, c.range(), integral_constants);
  }

  Value* emitStringLiteral(const StringLiteral& c) {
    return insertConstant(*graph, c.text(), c.range());
  }

  // Desugars select indexing: tensor[i] -> tensor.select(dim, i)
  Value* emitSelect(
      const SourceRange& loc,
      Value* input,
      int64_t dim,
      Value* index) {
    return emitBuiltinCall(
        loc, *graph, aten::select, c10::nullopt,
        {input, graph->insertConstant(dim, loc), index}, {}, true);
  }

  // Desugars slice indexing: tensor[begin:end] -> tensor.slice(dim, begin, end, 1)
  Value* emitSlice(
      const SourceRange& loc,
      Value* input,
      c10::optional<int64_t> dim, // Only used for tensor slicing
      const SliceExpr& slice) {
    std::vector<NamedValue> args;
    args.reserve(4);
    args.emplace_back(loc, "self", input);

    // XXX: If list slicing becomes more complicated or stops using
    // aten::slice, we should separate it from this function.
    if (dim) {
      JIT_ASSERT(input->type()->isSubtypeOf(DynamicType::get()));
      args.emplace_back(loc, "dim", graph->insertConstant(dim.value(), loc));
    } else {
      JIT_ASSERT(!input->type()->isSubtypeOf(DynamicType::get()));
    }

    args.emplace_back(loc, "begin", emitExpr(Expr(slice.startOr(0))));
    const auto has_end = slice.end().present();
    if (has_end) {
      args.emplace_back(loc, "end", emitExpr(Expr(slice.end().get())));
    }
    if (input->type()->cast<TupleType>()) {
      if (has_end) {
        return emitTupleSlice(loc, args[0], args[1], /*end*/args[2]);
      } else {
        return emitTupleSlice(loc, args[0], args[1], c10::nullopt);
      }
    }
    NamedValue step = NamedValue(loc, "step", graph->insertConstant(1, loc));
    return emitBuiltinCall(loc, *graph, aten::slice, c10::nullopt, args, {step}, true);
  }

  Value* emitIndex(
      const SourceRange& loc,
      Value* input,
      at::ArrayRef<Value*> indices) {
    auto* index = graph->insertNode(
        graph->createList(DynamicType::get(), indices))->output();
    return emitBuiltinCall(loc, *graph, aten::index, c10::nullopt,  {input, index}, {}, true);
  }

  // Emits multidimensional slicing with int and slice indices.
  // Returns:
  // - Value*: the input after it has been indexed by int and slice indices.
  // - vector<Value*>: A list of tensor Value* indices that have not been applied yet.
  //   Should be NULL at indices where sliceable (post-slicing) isn't indexed by a tensor.
  std::pair<Value*, std::vector<Value*>> emitIntAndSliceIndexing(
      const SourceRange& loc,
      Value* sliceable,
      const List<Expr>& subscript_exprs) {
    std::vector<Value*> tensor_indices;
    size_t dim = 0;

    auto handle_tensor = [&](Value* tensor) {
      // NB: tensor_indices can have NULL holes because of how at::index works.
      tensor_indices.resize(dim + 1);
      tensor_indices[dim] = tensor;
      dim++;
    };

    for (const auto & subscript_expr : subscript_exprs) {
      if (subscript_expr.kind() == TK_SLICE_EXPR) {
        sliceable = emitSlice(loc, sliceable, dim, SliceExpr(subscript_expr));
        ++dim;
        continue;
      }
      auto index = emitExpr(subscript_expr);
      if (index->type() == IntType::get()) {
        sliceable = emitSelect(loc, sliceable, dim, index);
        continue;
      } else if (index->type()->isSubtypeOf(DynamicType::get())) {
        handle_tensor(index);
        continue;
      }
      throw ErrorReport(loc)
        << "Unsupported operation: indexing tensor with unsupported index type "
        << index->type()->str() << ". Only ints, slices, and tensors are supported.";
    }
    // at::index takes in a TensorList where some tensors can be undefined.
    // Convert NULL tensorIndices to undefined tensors to pass to at::index.
    for (auto& index : tensor_indices) {
      if (index == nullptr) {
        index = graph->insertNode(graph->createUndefined())->output();
      }
    }
    return std::make_pair(sliceable, tensor_indices);
  }

  // Desugars multidim slicing into slice/select/index calls.
  //
  // XXX: Errors in user code are not elegantly reported.
  // Let's say someone were to do the following:
  //   @torch.jit.script
  //   def fn(x):
  //       return x[0, 1]
  //   fn(torch.randn(5))
  // Because we desugar this into two aten::select ops, the error message
  // complains about aten::select failing rather than there "not being
  // enough dimensions to index".
  //
  // The strategy is to slice and select the tensor for int and slices first
  // in one pass and then apply at::index on the result of the slicing/selecting.
  // Call the tensor after we've applied slice / select the `sliced`.
  // tensor_indices should have the same size as sliced.dim():
  // - tensor_indices[i] = NULL if we should not index `sliced` at dim i
  // - tensor_indices[i] = t if we should index `sliced` at dim i with tensor t.
  Value* emitMultidimSlicing(
      const SourceRange& loc,
      Value* sliceable,
      const List<Expr>& subscript_exprs) {
    if (!sliceable->type()->isSubtypeOf(DynamicType::get())) {
      throw ErrorReport(loc)
        << "Unsupported operation: attempted to use multidimensional "
        << "indexing on a non-tensor type.";
    }

    std::vector<Value*> tensor_indices;
    std::tie(sliceable, tensor_indices) =
        emitIntAndSliceIndexing(loc, sliceable, subscript_exprs);

    if (tensor_indices.empty()) {
      // XXX: Might need to at::alias this when we support mutability
      return sliceable;
    }

    return emitIndex(loc, sliceable, tensor_indices);
  }

  // Desugars slice syntactic sugar tensor[begin:end] -> tensor.slice(begin,
  // end).
  Value* emitBasicSlice(
      const SourceRange& loc,
      Value* sliceable,
      const List<Expr>& subscript_exprs) {
    JIT_ASSERT(subscript_exprs.size() == 1);
    JIT_ASSERT(subscript_exprs[0].kind() == TK_SLICE_EXPR);
    auto slice_exp = SliceExpr(subscript_exprs[0]);
    c10::optional<int64_t> maybe_dim;
    if (sliceable->type()->isSubtypeOf(DynamicType::get())) {
      // If the sliceable object is a tensor, specify a default dimension
      maybe_dim = 0;
    }
    return emitSlice(loc, sliceable, maybe_dim, slice_exp);
  }

  int64_t getTupleIndexVal(const SourceRange& loc,
    const TupleTypePtr& tuple_type,
      Value * idx_val,
      bool allow_out_of_bounds) {
     int64_t index;
    at::optional<IValue> ivalue = toIValue(idx_val);
    if (ivalue && ivalue->isInt()) {
      index = ivalue->to<int64_t>();
    } else {
      throw ErrorReport(loc)
        << "tuple indices must be integer constants";
    }
     // set index to be positive to simplify logic in runtime
    int64_t adj_index = index;
    int64_t tuple_len = tuple_type->elements().size();
    if (index < 0) {
      adj_index = tuple_len + index;
    }
    if (!allow_out_of_bounds && (adj_index >= tuple_len || adj_index < 0)) {
      throw ErrorReport(loc)
        << "Tuple index out of range. Tuple is length " << tuple_len
        << " and index is " << index;
    }
    return adj_index;
  }
   Value* emitTupleIndex(const SourceRange& loc,
      Value * tuple_val,
      Value * idx_val) {
    auto tuple_typ = tuple_val->type()->cast<TupleType>();
    auto adj_index = getTupleIndexVal(loc, tuple_typ, idx_val, /*allow_out_of_bounds*/false);
    return graph->insertNode(
        graph->createTupleIndex(tuple_val, adj_index))->output();
  }

  Value* emitTupleSlice(const SourceRange& loc,
      const NamedValue& tuple_val,
      const NamedValue& beg_val,
      const at::optional<NamedValue>& end_val) {
    auto tuple_type = tuple_val.value(*graph)->type()->expect<TupleType>();
    int64_t beg = getTupleIndexVal(loc, tuple_type, beg_val.value(*graph), /*allow_out_of_bounds*/true);
    int64_t end;
    int64_t tuple_len = tuple_type->elements().size();
    if (end_val) {
      end = getTupleIndexVal(loc, tuple_type, end_val->value(*graph), true);
    } else {
      end = tuple_len;
    }
    // slicing does not throw out of bounds errors
    end = std::min(std::max((int64_t)0, end), tuple_len);
    beg = std::min(std::max((int64_t)0, beg), tuple_len);

    return graph->insertNode(
        graph->createTupleSlice(tuple_val.value(*graph), beg, end))->output();
  }

  Value* emitSubscript(const Subscript& subscript) {
    return emitSubscript(
        subscript.range(),
        emitExpr(subscript.value()),
        subscript.subscript_exprs());
  }

  Value* emitSubscript(
      const SourceRange& loc,
      Value* sliceable,
      const List<Expr>& subscript_exprs) {
    if (subscript_exprs.size() != 1) {
      return emitMultidimSlicing(loc, sliceable, subscript_exprs);
    }
    if (subscript_exprs[0].kind() == TK_SLICE_EXPR) {
      return emitBasicSlice(loc, sliceable, subscript_exprs);
    } else {
      return emitBasicGather(loc, sliceable, subscript_exprs);
    }
  }

  // Desugars gather syntactic sugar foo[i]
  Value* emitBasicGather(
      const SourceRange& loc,
      Value* gatherable,
      const List<Expr>& subscript_exprs) {
    JIT_ASSERT(subscript_exprs.size() == 1);

    if (gatherable->type()->kind() == TypeKind::ListType) {
      // if it's a list, emit a regular index selection op
      auto* idx = emitExpr(subscript_exprs[0]);
      return emitBuiltinCall(
                 loc, *graph, aten::select, c10::nullopt, {gatherable, idx}, {}, true);
    } else if (gatherable->type()->isSubtypeOf(DynamicType::get())) {
      return emitMultidimSlicing(loc, gatherable, subscript_exprs);
    } else if (auto tuple_type = gatherable->type()->cast<TupleType>()) {
      auto* idx = emitExpr(subscript_exprs[0]);
      return emitTupleIndex(loc, gatherable, idx);
    } else {
      throw ErrorReport(loc)
        << "Indexing only supported on lists, tensors, and tuples.";
    }
  }
};

static const std::unordered_map<std::string, std::string> &builtin_cast_methods() {
  static std::unordered_map<std::string, std::string> builtin_cast_methods = {
    {"byte", "_cast_Byte"},
    {"char", "_cast_Char"},
    {"double", "_cast_Double"},
    {"float", "_cast_Float"},
    {"int", "_cast_Int"},
    {"long", "_cast_Long"},
    {"short", "_cast_Short"},
    {"half", "_cast_Half"}
  };
  return builtin_cast_methods;
}

// support syntax sugar for x.foo(y, z) by allowing x.foo to return a
// callable value that will resolve to foo(x, y, z) when called.
std::shared_ptr<SugaredValue> SimpleValue::attr(const SourceRange& loc, Method & m, const std::string& field) {
  // Allow method-style casts on Tensor types. e.g. x.int()
  if (value->type()->isSubtypeOf(DynamicType::get())) {
    if (builtin_cast_methods().count(field)) {
      return std::make_shared<BuiltinFunction>(
          Symbol::aten(builtin_cast_methods().at(field)),
          NamedValue(loc, "self", value));
    }
    // functions that are just direct property lookups on tensor
    // must be registered as prim::<name>(Tensor t) -> <return_type>
    static const std::unordered_set<std::string> fields = {
      "dtype",
      "device",
      "shape",
      "is_cuda",
      "requires_grad",
    };
    if (fields.count(field)) {
      auto r = m.graph()->insert(Symbol::fromQualString("prim::"+field), {value});
      return std::make_shared<SimpleValue>(r);
    }
  }
  if (getValue()->type()->isSubtypeOf(NumberType::get())) {
    throw ErrorReport(loc) << "Cannot call methods on numbers";
  }
  return std::make_shared<BuiltinFunction>(
      Symbol::aten(field), NamedValue(loc, "self", value));
}

std::vector<Value*> inlineCallTo(Graph& g, Graph& callee, ArrayRef<Value*> inputs) {
  std::unordered_map<Value*, Value*> value_map;
  auto value_map_func = [&](Value* v) { return value_map.at(v); };
  JIT_ASSERT(callee.inputs().size() == inputs.size());
  for (size_t i = 0; i < inputs.size(); ++i) {
    value_map[callee.inputs()[i]] = inputs[i];
  }
  for (auto* node : callee.nodes()) {
    auto* new_node =
        g.insertNode(g.createClone(node, value_map_func));
    for (size_t i = 0; i < node->outputs().size(); ++i) {
      value_map[node->outputs()[i]] = new_node->outputs()[i];
    }
  }

  std::vector<Value*> outputs;
  for (auto* output : callee.outputs()) {
    outputs.push_back(value_map_func(output));
  }
  return outputs;
}

void defineMethodsInModule(const std::shared_ptr<Module>& m, const std::vector<Def>& definitions, const std::vector<Resolver>& resolvers, const SugaredValuePtr& self) {
  JIT_ASSERT(definitions.size() == resolvers.size());
  auto resolver_it = resolvers.begin();
  std::vector<Method*> methods;
  std::unordered_map<std::string, Method*> function_table;
  for(const Def& def : definitions) {
    const std::string& name = def.name().name();
    auto resolver = *resolver_it++;
    JIT_ASSERT(resolver);
    if(!self) {
      // if self is defined, then these are methods and do not go into the global namespace
      // otherwise, they get defined together so we add them to the function table
      // so the methods can see each other
      resolver = [resolver, &function_table](
                     const std::string& name,
                     Method& m,
                     const SourceRange& loc) -> std::shared_ptr<SugaredValue> {
        auto it = function_table.find(name);
        if (it != function_table.end()) {
          return std::make_shared<MethodValue>(nullptr, *it->second);
        }
        return resolver(name, m, loc);
      };
    }
    auto creator = [def, resolver, self](Method& method) {
      JIT_ASSERT(resolver);
      to_ir(def, resolver, self,  method);
    };
    Method& method = m->create_method(name, creator);
    function_table[name] = &method;
    methods.push_back(&method);
  }
  for(Method* method : methods) {
    method->ensure_defined();
  }
  didFinishEmitModule(m);
}

const std::unordered_map<std::string, TypePtr> &ident_to_type_lut() {
  static std::unordered_map<std::string, TypePtr> map = {
    {"Tensor", DynamicType::get()},
    {"int", IntType::get()},
    {"float", FloatType::get()},
    {"bool", BoolType::get()},
    {"str", StringType::get()},
    {"Device", DeviceObjType::get()},
    // technically this is not a python type but we need it when
    // parsing serialized methods that use implicit converions to Scalar
    {"number", NumberType::get()},
    {"None", NoneType::get()},
  };
  return map;
}

const std::unordered_map<std::string, std::function<TypePtr(Subscript)>> &subscript_to_type_fns() {
  static std::unordered_map<std::string, std::function<TypePtr(Subscript)>> map = {
    {"Tuple", [](Subscript subscript) -> TypePtr {
      std::vector<TypePtr> subscript_expr_types;
      for (auto expr : subscript.subscript_exprs()) {
        subscript_expr_types.push_back(parseTypeFromExpr(expr));
      }
      return TupleType::create(subscript_expr_types);
    }},
    {"List", [](Subscript subscript) -> TypePtr {
      if (subscript.subscript_exprs().size() != 1) {
        throw ErrorReport(subscript) << " expected exactly one element type but found " << subscript.subscript_exprs().size();
      }
      auto elem_type = parseTypeFromExpr(*subscript.subscript_exprs().begin());
      return ListType::create(elem_type);
    }},
    {"Optional", [](Subscript subscript) -> TypePtr {
      if (subscript.subscript_exprs().size() != 1) {
        throw ErrorReport(subscript) << " expected exactly one element type but found " << subscript.subscript_exprs().size();
      }
      auto elem_type = parseTypeFromExpr(*subscript.subscript_exprs().begin());
      return OptionalType::create(elem_type);
    }},
    {"Future", [](Subscript subscript) -> TypePtr {
      if (subscript.subscript_exprs().size() != 1) {
        throw ErrorReport(subscript) << " expected exactly one element type but found " << subscript.subscript_exprs().size();
      }
      auto elem_type = parseTypeFromExpr(*subscript.subscript_exprs().begin());
      return FutureType::create(elem_type);
    }},
  };
  return map;
}

bool isTorch(const Expr& expr) {
  return expr.kind() == TK_VAR && Var(expr).name().name() == "torch";
}

// gets the base type name given namespaces where the types live
// turns torch.Tensor -> Tensor, X -> X
c10::optional<std::string> parseBaseTypeName(const Expr& expr) {
  switch (expr.kind()) {
    case TK_VAR: {
      return Var(expr).name().name();
    }
    case TK_NONE: {
      return "None";
    }
    case '.': {
      auto select = Select(expr);
      const std::string& name = select.selector().name();
      if (isTorch(select.value()) && name == "Tensor")
        return "Tensor";
    } break;
  }
  return at::nullopt;
}

TypePtr parseTypeFromExpr(const Expr& expr) {
  if (expr.kind() == TK_SUBSCRIPT) {
    auto subscript = Subscript(expr);
    auto value_name = parseBaseTypeName(subscript.value());
    if (!value_name) {
      throw ErrorReport(subscript.value().range()) << "Subscripted type must be a type identifier";
    }
    if (!subscript_to_type_fns().count(*value_name)) {
      throw ErrorReport(subscript.range()) << "Unknown type constructor " << *value_name;
    }
    return subscript_to_type_fns().at(*value_name)(subscript);
  } else if (auto name = parseBaseTypeName(expr)) {
    auto itr = ident_to_type_lut().find(*name);
    if (itr != ident_to_type_lut().end()) {
      return itr->second;
    }
    throw ErrorReport(expr) << "Unknown type name " << *name;
  }
  throw ErrorReport(expr.range()) << "Expression of type " << kindToString(expr.kind())
                                  << " cannot be used in a type expression";
}

c10::optional<std::pair<TypePtr, int32_t>> handleBroadcastList(const Expr& expr) {
  if (expr.kind() != TK_SUBSCRIPT)
    return c10::nullopt;
  auto subscript = Subscript(expr);
  if (subscript.value().kind() != TK_VAR)
    return c10::nullopt;
  auto var = Var(subscript.value());
  auto subscript_exprs = subscript.subscript_exprs();

  // handle the case where the BroadcastingList is wrapped in a Optional type
  if(var.name().name() == "Optional") {
    auto broadcast_list = handleBroadcastList(subscript_exprs[0]);
    if (broadcast_list) {
      TypePtr opt_type = OptionalType::create(broadcast_list->first);
      return std::pair<TypePtr, int32_t>(opt_type, broadcast_list->second);
    } else {
      return c10::nullopt;
    }
  } else if (var.name().name().find("BroadcastingList") != 0) {
    return c10::nullopt;
  }

  if (subscript_exprs.size() != 1)
    throw ErrorReport(subscript.subscript_exprs().range())
      << "BroadcastingList/Optional[BroadcastingList] must be subscripted with a type";

  auto typ = subscript_exprs[0];
  auto len = var.name().name().substr(strlen("BroadcastingList"));

  if (typ.kind() != TK_VAR)
    throw ErrorReport(subscript.value().range()) << "Subscripted type must be a type identifier";

  auto value_name = Var(typ).name().name();
  if (value_name != "float" && value_name != "int")
    throw ErrorReport(subscript.value().range()) << "Broadcastable lists only supported for int or float";

  auto elem_ptr = ident_to_type_lut().find(value_name);
  JIT_ASSERT(elem_ptr != ident_to_type_lut().end());
  TypePtr list_ptr = ListType::create(elem_ptr->second);

  const char* len_c = len.c_str();
  char* end;
  size_t len_v = strtoull(len_c, &end, 10);
  if (end != len_c + len.size()) {
    throw ErrorReport(subscript.subscript_exprs().range())
        << "subscript of Broadcastable list must be a positive integer";
  }
  return std::pair<TypePtr, int32_t>(list_ptr, len_v);
}

void defineMethodsInModule(std::shared_ptr<Module> m, const std::string& source, const Resolver& resolver, const SugaredValuePtr& self) {
  Parser p(source);
  std::vector<Def> definitions;
  std::vector<Resolver> resolvers;
  while (p.lexer().cur().kind != TK_EOF) {
    auto def = Def(p.parseFunction(/*is_method=*/bool(self)));
    definitions.push_back(def);
    resolvers.push_back(resolver);
  }
  defineMethodsInModule(std::move(m), definitions, resolvers, self);
}

std::vector<std::shared_ptr<SugaredValue>> SimpleValue::asTuple(
    const SourceRange& loc,
    Method& m,
    const c10::optional<size_t>& size_hint) {
  static const auto make_simple_value = [](Value* v) -> std::shared_ptr<SugaredValue> {
    return std::make_shared<SimpleValue>(v);
  };
  if(value->type()->kind() == TypeKind::TupleType) {
    auto outputs = createTupleUnpack(value);
    return fmap(outputs, make_simple_value);
  } else if (value->type()->kind() == TypeKind::ListType) {
    if (!size_hint) {
      throw ErrorReport(loc) << "cannot statically infer the expected size of a list in this context";
    }
    auto graph = value->owningGraph();
    Node *unpack = graph->insertNode(graph->createListUnpack(value, *size_hint));
    return fmap(unpack->outputs(), make_simple_value);
  }
  throw ErrorReport(loc) << value->type()->str() << " cannot be used as a tuple";
}

} // namespace script
} // namespace jit
} // namespace torch
