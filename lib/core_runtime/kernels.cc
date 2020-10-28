// Copyright 2020 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//===- core_runtime/kernels.cc --------------------------------------------===//
//
// This library contains kernels that allows the bef_executor to drive the core
// runtime.
//
//===----------------------------------------------------------------------===//

#include "tfrt/core_runtime/kernels.h"

#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/SmallString.h"
#include "tfrt/core_runtime/core_runtime.h"
#include "tfrt/core_runtime/execute_op_impl.h"
#include "tfrt/core_runtime/logging_op_handler.h"
#include "tfrt/core_runtime/op_attrs.h"
#include "tfrt/core_runtime/op_handler.h"
#include "tfrt/core_runtime/tensor_handle.h"
#include "tfrt/host_context/async_dispatch.h"
#include "tfrt/host_context/async_value.h"
#include "tfrt/host_context/async_value_ref.h"
#include "tfrt/host_context/attribute_utils.h"
#include "tfrt/host_context/chain.h"
#include "tfrt/host_context/function.h"
#include "tfrt/host_context/kernel_utils.h"
#include "tfrt/host_context/sync_kernel_utils.h"
#include "tfrt/support/error_util.h"
#include "tfrt/support/ref_count.h"
#include "tfrt/tensor/conversion_registry.h"
#include "tfrt/tensor/dense_host_tensor.h"
#include "tfrt/tensor/dense_host_tensor_view.h"
#include "tfrt/tensor/host_tensor.h"
#include "tfrt/tensor/string_host_tensor.h"
#include "tfrt/tensor/tensor_serialize_utils.h"

namespace tfrt {

// Convert a HostTensor (or subclass) into a TensorHandle for use by
// Core Runtime.
static void HTToTensorHandle(Argument<HostTensor> arg, Argument<Chain> in_chain,
                             Result<TensorHandle> tensorhandle_output,
                             const ExecutionContext &exec_ctx) {
  // Since we know the Tensor is present, we can access its metadata.
  // TODO(b/158775215): Replace the placeholder device with the device from
  // HostTensor.
  tensorhandle_output.Emplace(exec_ctx.host()->GetHostDeviceRef(),
                              arg->metadata(), arg.ValueRef());
}

static void TensorHandleToHT(Argument<TensorHandle> arg,
                             Result<HostTensor> ht_output) {
  ht_output.Set(FormRef(arg->GetAsyncTensor()));
}

// Get TensorShape of a TensorHandle for use by Core Runtime.
static void TensorHandleToShape(Argument<TensorHandle> arg,
                                Result<TensorShape> tensorshape_result,
                                const ExecutionContext &exec_ctx) {
  if (arg->IsMetadataAvailable()) {
    auto shape = arg->GetAvailableMetadata().shape;
    tensorshape_result.Emplace(std::move(shape));
    return;
  }
  // The metadata is not available yet.
  const AsyncValueRef<TensorMetadata> &metadata = arg->GetAsyncMetadata();

  auto value = tensorshape_result.AllocateIndirect();
  metadata.AndThen([value_ref = std::move(value),
                    metadata_ref = metadata.CopyRef(),
                    host = exec_ctx.host()]() mutable {
    if (metadata_ref.IsError()) {
      value_ref->ForwardTo(metadata_ref.ReleaseRCRef());
      return;
    }
    auto shape = metadata_ref.get().shape;
    value_ref->ForwardTo(
        MakeAvailableAsyncValueRef<TensorShape>(host, std::move(shape)));
  });
}

// Convert a HostTensor (or subclass) into a TensorHandle for use by
// Core Runtime.
static void PrintTensorHandleSync(const TensorHandle &arg) {
  llvm::SmallString<256> message;
  llvm::raw_svector_ostream(message) << arg << "\n";
  printf("%s", message.c_str());
  fflush(stdout);
}
static Chain PrintTensorHandle(const TensorHandle &arg) {
  PrintTensorHandleSync(arg);
  return Chain();
}

static void CreateOpAttrs(Result<OpAttrs> result) { result.Emplace(); }

static Chain OpAttrsSetBool(Argument<OpAttrs> attrs, StringAttribute key,
                            Attribute<int8_t> value) {
  attrs->Set(key, static_cast<bool>(*value));
  return Chain();
}

template <typename T>
static Chain OpAttrsSet(Argument<OpAttrs> attrs, StringAttribute key,
                        Attribute<T> value) {
  attrs->Set(key, *value);
  return Chain();
}

static Chain OpAttrsSetDType(Argument<OpAttrs> attrs, StringAttribute key,
                             Attribute<DType::Kind> value) {
  attrs->Set(key, GetOpAttrTypeFromDType(*value));
  return Chain();
}

static Chain OpAttrsSetDense(Argument<OpAttrs> attrs, StringAttribute key,
                             DenseAttr value) {  // NOLINT
  attrs->Set(key, value);
  return Chain();
}

static Chain OpAttrsSetAggregate(Argument<OpAttrs> attrs, StringAttribute key,
                                 AggregateAttr value) {  // NOLINT
  attrs->Set(key, value);
  return Chain();
}
static Chain OpAttrsSetShape(Argument<OpAttrs> attrs, StringAttribute key,
                             ShapeAttr value) {  // NOLINT
  attrs->Set(key, value);
  return Chain();
}

template <typename T>
static Chain OpAttrsSetArray(Argument<OpAttrs> attrs, StringAttribute key,
                             ArrayAttribute<T> value) {
  attrs->SetArray(key, value.data());
  return Chain();
}

static Chain OpAttrsSetString(Argument<OpAttrs> attrs, StringAttribute key,
                              StringAttribute value) {
  attrs->SetString(key, value.get());
  return Chain();
}

static llvm::Expected<TensorHandle> ConstStringTensor(
    ArrayAttr shape, AggregateAttr value, const ExecutionContext &exec_ctx) {
  TensorMetadata metadata(DType(DType::String), shape.GetValue<int64_t>());

  auto tensor_ref =
      StringHostTensor::MakeConstructedAsyncValueRef(metadata, exec_ctx.host());
  if (!tensor_ref)
    return MakeStringError("failed to allocate string host tensor");

  auto strings = tensor_ref.get().strings();

  if (value.GetNumElements() == 1) {
    // All elements are the same, and only one element is saved in BEF.
    string_view sv = value.GetAttributeOfType<StringAttr>(0).GetValue();
    for (int i = 0, e = strings.size(); i != e; ++i) {
      strings[i] = sv.str();
    }
  } else {
    assert(strings.size() == value.GetNumElements());
    for (int i = 0, e = strings.size(); i != e; ++i) {
      strings[i] = value.GetAttributeOfType<StringAttr>(i).GetValue().str();
    }
  }
  tensor_ref.SetStateConcrete();

  return TensorHandle(exec_ctx.host()->GetHostDeviceRef(), metadata,
                      std::move(tensor_ref));
}

static llvm::Expected<TensorHandle> ConstDenseTensor(
    DenseAttr value, const ExecutionContext &context) {
  auto *host = context.host();
  auto dht = DeserializeDenseHostTensorFromDenseAttr(value, host);
  if (!dht) return dht.takeError();

  auto metadata = dht->metadata();
  auto tensor_ref =
      MakeAvailableAsyncValueRef<DenseHostTensor>(host, std::move(*dht));
  if (!tensor_ref)
    return MakeStringError("failed to allocate dense host tensor");

  return TensorHandle(host->GetHostDeviceRef(), metadata,
                      std::move(tensor_ref));
}

template <typename DType>
static llvm::Expected<TensorHandle> CreateDenseTensor(
    ArrayAttribute<int64_t> shape, ArrayAttribute<DType> value,
    const ExecutionContext &context) {
  auto *host = context.host();

  TensorMetadata metadata(GetDType<DType>(), shape.data());
  auto dht = DenseHostTensor::MakeConstructedAsyncValueRef(metadata, host);
  if (!dht) return MakeStringError("failed to allocate dense host tensor");

  std::memcpy(dht->data(), value.data().data(), dht->DataSizeInBytes());

  dht.SetStateConcrete();

  return TensorHandle(host->GetHostDeviceRef(), metadata, std::move(dht));
}

// ExecuteOp executes the `op_name` operation on the `op_handler`.
static void ExecuteOp(Argument<OpHandler *> op_handler, RemainingArguments args,
                      RemainingResults results, AggregateAttr op_attr_array,
                      StringAttr op_name, KernelErrorHandler handler,
                      const ExecutionContext &exec_ctx) {
  auto *host = exec_ctx.host();
  auto *core_rt = CoreRuntime::GetFromHostContext(host);
  if (!core_rt) return handler.ReportError("no CoreRuntime available");

  auto expected_op = core_rt->MakeOp(op_name.GetValue(), op_handler.get());
  if (!expected_op) return handler.ReportError(StrCat(expected_op.takeError()));

  for (int b = 0, e = results.size(); b < e; ++b)
    results.AllocateAt<TensorHandle>(b);

  ExecuteOpImpl(std::move(expected_op.get()), args.values(),
                /*op_chain =*/nullptr, results.values(), op_attr_array,
                exec_ctx);
}

// Synchronous version of ExecuteOp.
static Error ExecuteOpSync(SyncArgument<OpHandler *> op_handler,
                           RepeatedSyncArguments<TensorHandle> args,
                           SyncKernelFrame *frame, AggregateAttr op_attr_array,
                           StringAttr op_name,
                           const ExecutionContext &exec_ctx) {
  auto *host = exec_ctx.host();
  auto *core_rt = CoreRuntime::GetFromHostContext(host);
  if (!core_rt) return MakeStringError("no CoreRuntime available");

  auto expected_op = core_rt->MakeOp(op_name.GetValue(), op_handler.get());
  if (!expected_op) return MakeStringError(expected_op.takeError());
  ExecuteOpImplSync(expected_op.get(), args,
                    /*op_chain =*/nullptr, frame, op_attr_array, exec_ctx);
  return Error::success();
}

// ExecuteOpSeq executes the `op_name` operation on the `op_handler`. It takes
// an `in_op_chain` and produces an `out_op_chain` for sequencing op execution.
// The execution is only started when `in_op_chain` is ready, and the
// `out_op_chain` is ready only after the execution is finished.
static void ExecuteOpSeq(Argument<OpHandler *> op_handler,
                         Argument<Chain> in_op_chain, RemainingArguments args,
                         Result<Chain> out_op_chain, RemainingResults results,
                         AggregateAttr op_attr_array, StringAttr op_name,
                         KernelErrorHandler handler,
                         const ExecutionContext &exec_ctx) {
  auto *host = exec_ctx.host();
  auto *core_rt = CoreRuntime::GetFromHostContext(host);
  if (!core_rt) return handler.ReportError("no CoreRuntime available");

  for (int b = 0, e = results.size(); b < e; ++b)
    results.AllocateAt<TensorHandle>(b);

  SmallVector<AsyncValue *, 4> async_args;
  if (!op_handler.value()->IsConcrete())
    async_args.push_back(op_handler.value());
  for (auto *arg_av : args.values())
    if (!arg_av->IsConcrete()) async_args.push_back(arg_av);

  // If all arguments except in_op_chain are ready, we can just execute the op.
  if (async_args.empty()) {
    auto expected_op = core_rt->MakeOp(op_name.GetValue(), op_handler.get());
    if (!expected_op)
      return handler.ReportError(StrCat(expected_op.takeError()));

    auto op_chain = in_op_chain.ValueRef();
    ExecuteOpImpl(std::move(expected_op.get()), args.values(), &op_chain,
                  results.values(), op_attr_array, exec_ctx);
    out_op_chain.Set(std::move(op_chain));
    return;
  }

  // Otherwise, we need to create references to all arguments and asynchronouly
  // execute the op when they are ready.

  SmallVector<AsyncValueRef<TensorHandle>, 4> arg_refs;
  for (auto *av : args.values()) {
    arg_refs.push_back(AsyncValueRef<TensorHandle>(FormRef(av)));
  }

  SmallVector<RCReference<AsyncValue>, 4> result_refs;
  for (auto &av : results.values()) {
    result_refs.push_back(av.CopyRef());
  }

  RunWhenReady(
      async_args,
      [core_rt, op_handler = op_handler.ValueRef(),
       op_chain = in_op_chain.ValueRef(), arg_refs = std::move(arg_refs),
       result_refs = std::move(result_refs),
       out_op_chain = out_op_chain.Allocate(), op_name = op_name.GetValue(),
       op_attr_array, exec_ctx]() mutable {
        auto propgate_error = [&](const DecodedDiagnostic &diag) {
          out_op_chain.SetError(diag);
          for (auto &result_ref : result_refs) result_ref->SetError(diag);
        };

        if (op_handler.IsError()) return propgate_error(op_handler.GetError());
        if (op_chain.IsError()) return propgate_error(op_chain.GetError());

        auto expected_op = core_rt->MakeOp(op_name, op_handler.get());
        if (!expected_op)
          return propgate_error(
              EmitError(exec_ctx, StrCat(expected_op.takeError())));

        SmallVector<AsyncValue *, 4> arg_avs;
        for (const auto &arg_ref : arg_refs) {
          if (arg_ref.IsError()) return propgate_error(arg_ref.GetError());
          arg_avs.push_back(arg_ref.GetAsyncValue());
        }

        ExecuteOpImpl(std::move(expected_op.get()), arg_avs, &op_chain,
                      result_refs, op_attr_array, exec_ctx);

        auto *op_chain_av = op_chain.GetAsyncValue();
        op_chain_av->AndThen([op_chain = std::move(op_chain),
                              out_op_chain = out_op_chain.CopyRef()]() {
          // TODO(chky): we should have a version of AndThen that passes the
          // resolved state into the waiter.
          if (op_chain.IsError()) {
            out_op_chain.SetError(op_chain.GetError());
          } else {
            out_op_chain.emplace();
          }
        });
      });
}

// ExecuteOp executes the `op_name` operation on the `op_handler`.
static void ExecuteCoreRuntimeOp(Argument<CoreRuntimeOp> op,
                                 RemainingArguments args,
                                 RemainingResults results,
                                 AggregateAttr op_attrs,
                                 KernelErrorHandler handler,
                                 const ExecutionContext &exec_ctx) {
  auto *host = exec_ctx.host();
  auto *core_rt = CoreRuntime::GetFromHostContext(host);
  if (!core_rt) return handler.ReportError("no CoreRuntime available");

  for (int b = 0, e = results.size(); b < e; ++b)
    results.AllocateAt<TensorHandle>(b);

  ExecuteOpImpl(std::move(op.get()), args.values(),
                /*op_chain =*/nullptr, results.values(), op_attrs, exec_ctx);
}

static tfrt::Expected<CoreRuntimeOp> MakeCompositeOp(
    Attribute<Function> fn_const, const ExecutionContext &exec_ctx) {
  auto *host = exec_ctx.host();
  auto *core_rt = CoreRuntime::GetFromHostContext(host);
  if (!core_rt) return MakeStringError("no CoreRuntime available");

  Function *fn = const_cast<Function *>(&(*fn_const));
  return core_rt->MakeCompositeOp(fn);
}

// GetOpHandler accepts chains because the op_handlers now can be registered
// dynamically as well.
static tfrt::Expected<OpHandler *> GetOpHandlerSync(
    StringAttribute op_handler_name, const ExecutionContext &exec_ctx) {
  auto *runtime = CoreRuntime::GetFromHostContext(exec_ctx.host());
  assert(runtime);

  if (auto *op_handler = runtime->GetOpHandler(op_handler_name.get())) {
    return op_handler;
  }
  return tfrt::MakeStringError("op_handler not found.");
}

static tfrt::Expected<OpHandler *> GetOpHandler(
    Argument<Chain> in_op_chain, StringAttribute op_handler_name,
    const ExecutionContext &exec_ctx) {
  return GetOpHandlerSync(op_handler_name, exec_ctx);
}

static void RegisterOpHandlerSync(Argument<OpHandler *> root,
                                  StringAttribute chain_name,
                                  const ExecutionContext &exec_ctx) {
  assert(root.get());
  auto *runtime = CoreRuntime::GetFromHostContext(exec_ctx.host());
  assert(runtime);

  runtime->RegisterOpHandler(chain_name, root.get());
}

static Chain RegisterOpHandler(Argument<OpHandler *> root,
                               StringAttribute chain_name,
                               const ExecutionContext &exec_ctx) {
  RegisterOpHandlerSync(root, chain_name, exec_ctx);
  return Chain();
}

void CreateLoggingOpHandlerKernel(Argument<OpHandler *> fallback,
                                  Result<OpHandler *> op_handler,
                                  Attribute<bool> sync_log_results,
                                  const ExecutionContext &exec_ctx) {
  auto *runtime = tfrt::CoreRuntime::GetFromHostContext(exec_ctx.host());
  assert(runtime);
  auto op_handler_ptr =
      CreateLoggingOpHandler(runtime, fallback.get(), sync_log_results.get());
  assert(op_handler_ptr);
  op_handler.Emplace(op_handler_ptr.get());
}

static bool GetDHTPredicateValue(const DenseHostTensor &dht) {
  switch (dht.dtype().kind()) {
    default:
      llvm_unreachable("dtype not supported");
      break;
    case DType::BOOL: {
      auto dht_view = DHTArrayView<bool>(&dht);
      assert(dht_view.NumElements() == 1);
      return dht_view[0];
    }
#define DTYPE_INT(ENUM)                                                      \
  case DType::ENUM: {                                                        \
    auto dht_view = DHTArrayView<tfrt::TypeForDTypeKind<DType::ENUM>>(&dht); \
    assert(dht_view.NumElements() == 1);                                     \
    return dht_view[0] != 0;                                                 \
  }
#include "tfrt/dtype/dtype.def"  // NOLINT
  }
}

// Returns true if any errors were propagated to the results.
static bool ReturnAfterHandlingError(
    AsyncValue *condition,
    MutableArrayRef<RCReference<IndirectAsyncValue>> results) {
  // If we have an error, then we can force propagate errors to all the
  // results.
  if (condition->IsError()) {
    for (auto &result : results) result->ForwardTo(FormRef(condition));
    return true;
  }

  if (condition->IsType<TensorHandle>() &&
      condition->get<TensorHandle>().IsError()) {
    for (auto &result : results) {
      result->ForwardTo(
          FormRef(condition->get<TensorHandle>().GetErrorAsyncValue()));
    }
    return true;
  }

  return false;
}

static llvm::Expected<bool> GetTensorPredicateValue(const Tensor &tensor) {
  // TODO(hanbinyoon): Handle other tensor types and other dtypes.
  if (const DenseHostTensor *dht = llvm::dyn_cast<DenseHostTensor>(&tensor)) {
    return GetDHTPredicateValue(*dht);
  } else if (const StringHostTensor *sht =
                 llvm::dyn_cast<StringHostTensor>(&tensor)) {
    ArrayRef<std::string> strings = sht->strings();
    // Only empty string is false.
    return !strings.empty() && !strings[0].empty();
  }
  return MakeStringError("tensor predicate does not support type ",
                         tensor.tensor_type().name());
}

// corert.cond dispatches to a 'true' or 'false' function based on a condition.
//
// Arguments: The first argument is the condition, with type TensorHandle, and
// any additional arguments are passed to the selected function.
//
// Attributes: The first attribute is the true_fn, and the second attribute is
// the false_fn. The functions must have matching signatures, and their
// signatures must match corert.cond's signature.
//
// corert.cond supports "non-strict" invocation: it is safe to invoke before all
// its arguments are ready. The caller must set the bef.nonstrict attribute on
// hex.if to make an invocation non-strict.
static void CoreRtConditional(RemainingArguments args, RemainingResults results,
                              Attribute<Function> true_fn_const,
                              Attribute<Function> false_fn_const,
                              const ExecutionContext &exec_ctx) {
  assert(args.size() > 0);

  const Function *true_fn = &(*true_fn_const);
  const Function *false_fn = &(*false_fn_const);

  assert(true_fn->argument_types().size() == args.size() - 1 &&
         "argument count mismatch");
  assert(true_fn->result_types().size() == results.size() &&
         "result count mismatch");
  assert(true_fn->argument_types() == false_fn->argument_types() &&
         true_fn->result_types() == false_fn->result_types() &&
         "true and false function types need to line up");

  // Note: At this point, the condition's availability is unknown. It may become
  // available at any time.

  // Copy `args` and add a ref to each arg. These refs will be dropped when the
  // RCArray is destroyed. arg_refs is captured by the lambda so the kernel's
  // arguments will be available when the closure runs.
  RCArray<AsyncValue> arg_refs(args.values());

  // We need to create all the result values eagerly so we can return them
  // from the HexIf function, even though we don't know their types.  Use
  // an IndirectAsyncValue for this, because it can lazily get resolved.
  SmallVector<RCReference<IndirectAsyncValue>, 4> result_refs;
  result_refs.reserve(results.size());
  for (int i = 0, e = results.size(); i != e; ++i) {
    auto result = results.AllocateIndirectResultAt(i);
    // To ensure the results live long enough to be filled in by our deferred
    // evaluation, we keep the RCReferences holding the results.
    result_refs.push_back(std::move(result));
  }

  auto if_impl =
      [](const HostTensor &ht, const Function *true_fn,
         const Function *false_fn, ArrayRef<AsyncValue *> arg_refs,
         MutableArrayRef<RCReference<IndirectAsyncValue>> result_refs,
         const ExecutionContext &exec_ctx) {
        llvm::Expected<bool> predicate = GetTensorPredicateValue(ht);
        if (!predicate) {
          RCReference<ErrorAsyncValue> error_value =
              EmitErrorAsync(exec_ctx, StrCat(predicate.takeError()));
          for (auto &result : result_refs) {
            result->SetError(error_value->GetError());
          }
          return;
        }

        const Function *fn = predicate.get() ? true_fn : false_fn;
        SmallVector<RCReference<AsyncValue>, 8> results;
        results.resize(result_refs.size());
        fn->Execute(exec_ctx, arg_refs.drop_front(), results);

        // Forward result_refs to results. This transfers the +1
        // results returned by Execute to the ForwardTo call.
        for (int i = 0, e = result_refs.size(); i != e; ++i) {
          result_refs[i]->ForwardTo(std::move(results[i]));
        }
      };

  // Arg[0] is a TensorHandle async value condition predicate.
  AsyncValue *condition_tensorhandle = args[0];
  // Dispatch when the condition becomes available.
  condition_tensorhandle->AndThen(
      [condition_tensorhandle, exec_ctx, if_impl,
       true_fn_ref = FormRef(true_fn), false_fn_ref = FormRef(false_fn),
       arg_refs = std::move(arg_refs),
       result_refs = std::move(result_refs)]() mutable {
        if (ReturnAfterHandlingError(condition_tensorhandle, result_refs))
          return;
        AsyncValue *condition_async_tensor =
            condition_tensorhandle->get<TensorHandle>().GetAsyncTensor();
        auto src_device_ref =
            condition_tensorhandle->get<TensorHandle>().CopyRefDevice();

        // TODO(hanbinyoon): Remove this extra level of asynchrony after
        // b/162752746 is fixed.
        // TODO(hanbinyoon): Consider refactoring to reduce code repetition -
        // possibly a version of RunWhenReady that takes a vector of closures
        // that return AsyncValues.
        condition_async_tensor->AndThen(
            [condition_async_tensor, src_device_ref = std::move(src_device_ref),
             exec_ctx, if_impl, true_fn_ref = std::move(true_fn_ref),
             false_fn_ref = std::move(false_fn_ref),
             arg_refs = std::move(arg_refs),
             result_refs = std::move(result_refs)]() mutable {
              if (ReturnAfterHandlingError(condition_async_tensor, result_refs))
                return;

              auto &tensor = condition_async_tensor->get<Tensor>();
              AsyncValueRef<HostTensor> condition_host_tensor =
                  AsyncValueRef<HostTensor>(
                      ConvertTensor(exec_ctx, tensor, *src_device_ref,
                                    exec_ctx.host()->GetHostDevice(),
                                    DenseHostTensor::kTensorType));

              condition_host_tensor.AndThen(
                  [condition_host_tensor = condition_host_tensor.CopyRef(),
                   exec_ctx, if_impl, true_fn_ref = std::move(true_fn_ref),
                   false_fn_ref = std::move(false_fn_ref),
                   arg_refs = std::move(arg_refs),
                   result_refs = std::move(result_refs)]() mutable {
                    if (ReturnAfterHandlingError(
                            condition_host_tensor.GetAsyncValue(), result_refs))
                      return;

                    if_impl(*condition_host_tensor, true_fn_ref.get(),
                            false_fn_ref.get(), arg_refs.values(), result_refs,
                            exec_ctx);
                  });
            });
      });
}

// TODO(fishx): Take device object as an argument instead of attribute.
// Right now we cannot do that because a kernel cannot take RCReference as an
// argument. We need to use either CopyRef() or std::move for RCReference
// argument.
static Expected<TensorHandle> TransferToDevice(
    const TensorHandle &src, StringAttribute device,
    StringAttribute dst_tensor_type_name, const ExecutionContext &exec_ctx) {
  auto device_ref =
      exec_ctx.host()->GetDeviceManager()->GetDeviceRef<Device>(device);
  if (!device_ref)
    return MakeStringError("failed to find device with name: ", device);
  return src.TransferTo(exec_ctx, std::move(device_ref),
                        GetStaticTensorType(dst_tensor_type_name));
}

// Forward declaration for use in CoreRtWhileLoopIterationImpl.
static void CoreRtWhileLoopIteration(
    const ExecutionContext &exec_ctx, RCReference<const Function> cond_fn_ref,
    RCReference<const Function> body_fn_ref,
    SmallVector<RCReference<AsyncValue>, 4> arg_refs,
    SmallVector<RCReference<IndirectAsyncValue>, 4> result_refs);

// This is a helper function that runs a single iteration (or zero iterations if
// the condition is not met) of CoreRtWhileLoop.
static void CoreRtWhileLoopIterationImpl(
    const ExecutionContext &exec_ctx, const Tensor &condition,
    RCReference<const Function> cond_fn_ref,
    RCReference<const Function> body_fn_ref,
    SmallVector<RCReference<AsyncValue>, 4> arg_refs,
    SmallVector<RCReference<IndirectAsyncValue>, 4> result_refs) {
  // Determine whether to execute the loop body function.
  llvm::Expected<bool> predicate = GetTensorPredicateValue(condition);
  if (!predicate) {
    // Set errors to all the results instead of executing the loop body
    // function.
    RCReference<ErrorAsyncValue> error_value =
        EmitErrorAsync(exec_ctx, StrCat(predicate.takeError()));
    for (auto &result : result_refs) {
      result->SetError(error_value->GetError());
    }
    return;
  }

  if (!predicate.get()) {
    // Copy args to results instead of executing the loop body function.
    for (int arg = 0; arg != arg_refs.size(); ++arg) {
      result_refs[arg]->ForwardTo(FormRef(arg_refs[arg].get()));
    }
    return;
  }

  // Execute the loop body function.
  SmallVector<AsyncValue *, 4> args;
  for (RCReference<AsyncValue> &arg : arg_refs) args.push_back(arg.get());
  SmallVector<RCReference<AsyncValue>, 4> passed_args;
  passed_args.resize(result_refs.size());
  body_fn_ref->Execute(exec_ctx, args, passed_args);

  EnqueueWork(exec_ctx, [exec_ctx, cond_fn_ref = std::move(cond_fn_ref),
                         body_fn_ref = std::move(body_fn_ref),
                         arg_refs = std::move(passed_args),
                         result_refs = std::move(result_refs)]() mutable {
    CoreRtWhileLoopIteration(exec_ctx, std::move(cond_fn_ref),
                             std::move(body_fn_ref), std::move(arg_refs),
                             std::move(result_refs));
  });
}

// This is a helper function that executes the loop condition function and kicks
// off a potential iteration of CoreRtWhileLoop.
static void CoreRtWhileLoopIteration(
    const ExecutionContext &exec_ctx, RCReference<const Function> cond_fn_ref,
    RCReference<const Function> body_fn_ref,
    SmallVector<RCReference<AsyncValue>, 4> arg_refs,
    SmallVector<RCReference<IndirectAsyncValue>, 4> result_refs) {
  if (auto cancel_av = exec_ctx.GetCancelAsyncValue()) {
    // Cancellation detected. Set results to the cancel async value, and break
    // out.
    for (auto &result : result_refs) {
      result->ForwardTo(FormRef(cancel_av));
    }
    return;
  }

  // TODO(hanbinyoon): Look for ways to avoid allocating this args SmallVector
  // on each iteration of the loop. For example, consider the reuse of
  // passed_args in TFRTRepeatI32Block().
  SmallVector<AsyncValue *, 4> args;
  for (RCReference<AsyncValue> &arg : arg_refs) args.push_back(arg.get());
  SmallVector<RCReference<AsyncValue>, 2> condition;
  condition.resize(2);
  cond_fn_ref->Execute(exec_ctx, args, condition);

  assert(condition[0]->IsType<Chain>() &&
         "Cond function did not return a chain");
  assert(condition[1]->IsType<TensorHandle>() &&
         "Cond function did not return a TensorHandle");

  // Dispatch when the condition becomes available.
  RunWhenReady(condition, [condition_tensorhandle_ref = condition[1].CopyRef(),
                           exec_ctx = std::move(exec_ctx),
                           cond_fn_ref = std::move(cond_fn_ref),
                           body_fn_ref = std::move(body_fn_ref),
                           arg_refs = std::move(arg_refs),
                           result_refs = std::move(result_refs)]() mutable {
    AsyncValue *condition_tensorhandle = condition_tensorhandle_ref.get();
    if (ReturnAfterHandlingError(condition_tensorhandle, result_refs)) return;

    AsyncValue *condition_async_tensor =
        condition_tensorhandle->get<TensorHandle>().GetAsyncTensor();
    auto src_device_ref =
        condition_tensorhandle->get<TensorHandle>().CopyRefDevice();
    if (!src_device_ref->IsDeviceType(CpuDevice::kDeviceType)) {
      RCReference<ErrorAsyncValue> error_value = EmitErrorAsync(
          exec_ctx, StrCat(MakeStringError(
                        "non-cpu device for condition tensor handle")));
      for (auto &result : result_refs) {
        result->SetError(error_value->GetError());
      }
      return;
    }

    // TODO(hanbinyoon): Remove this extra level of asynchrony after
    // b/162752746 is fixed.
    condition_async_tensor->AndThen(
        [condition_tensorhandle_ref = std::move(condition_tensorhandle_ref),
         src_device_ref = std::move(src_device_ref),
         exec_ctx = std::move(exec_ctx), cond_fn_ref = std::move(cond_fn_ref),
         body_fn_ref = std::move(body_fn_ref), arg_refs = std::move(arg_refs),
         result_refs = std::move(result_refs)]() mutable {
          AsyncValue *condition_tensorhandle = condition_tensorhandle_ref.get();
          AsyncValue *condition_async_tensor =
              condition_tensorhandle->get<TensorHandle>().GetAsyncTensor();
          if (ReturnAfterHandlingError(condition_async_tensor, result_refs))
            return;

          auto &tensor = condition_async_tensor->get<Tensor>();
          AsyncValueRef<HostTensor> condition_host_tensor =
              AsyncValueRef<HostTensor>(
                  ConvertTensor(exec_ctx, tensor, *src_device_ref,
                                exec_ctx.host()->GetHostDevice(),
                                DenseHostTensor::kTensorType));

          condition_host_tensor.AndThen(
              [condition_host_tensor = condition_host_tensor.CopyRef(),
               exec_ctx = std::move(exec_ctx),
               cond_fn_ref = std::move(cond_fn_ref),
               body_fn_ref = std::move(body_fn_ref),
               arg_refs = std::move(arg_refs),
               result_refs = std::move(result_refs)]() mutable {
                if (ReturnAfterHandlingError(
                        condition_host_tensor.GetAsyncValue(), result_refs))
                  return;

                CoreRtWhileLoopIterationImpl(
                    exec_ctx, *condition_host_tensor, std::move(cond_fn_ref),
                    std::move(body_fn_ref), std::move(arg_refs),
                    std::move(result_refs));
              });
        });
  });
}

// corert.while dispatches multiple iterations of a 'Body' function based on a
// 'Cond' function like the following:
//     results = args; while (cond_fn(results)) { results = body_fn(results) }
//
// Arguments: All arguments are passed to the 'Cond' and 'Body' functions.
//
// Attributes: The first attribute is the cond_fn, and the second attribute is
// the body_fn. The functions must have matching input signatures, and body_fn's
// signature must match corert.while's signature.
static void CoreRtWhileLoop(RemainingArguments args, RemainingResults results,
                            Attribute<Function> cond_fn_const,
                            Attribute<Function> body_fn_const,
                            const ExecutionContext &exec_ctx) {
  assert(args.size() > 0);

  const Function *cond_fn = &(*cond_fn_const);
  const Function *body_fn = &(*body_fn_const);

  assert(body_fn->argument_types() == body_fn->result_types() &&
         "Argument and result types of repeat body_fn must match");
  assert(body_fn->argument_types() == cond_fn->argument_types() &&
         "body and cond function argument types need to line up");
  assert(body_fn->argument_types().size() == args.size() &&
         "argument count mismatch");
  assert(body_fn->result_types().size() == results.size() &&
         "result count mismatch");

  // Copy `args` and add a ref to each arg. These refs will be dropped when the
  // RCReferences are destroyed. arg_refs is captured by the lambda (in
  // CoreRtWhileLoopIteration) so the kernel's arguments will be available when
  // the closure runs.
  SmallVector<RCReference<AsyncValue>, 4> arg_refs;
  for (AsyncValue *arg : args.values()) {
    arg_refs.push_back(FormRef(arg));
  }

  // Create a RCRef of Function to extend its lifetime into the lambda (in
  // CoreRtWhileLoopIteration).
  RCReference<const Function> cond_fn_ref = FormRef(cond_fn);
  RCReference<const Function> body_fn_ref = FormRef(body_fn);

  // Define results as IndirectAsync values. The actual results are set in the
  // last iteration of the loop.
  // TODO(hanbinyoon): Consider using concrete types; the first is a Chain and
  // the rest are TensorHandles.
  SmallVector<RCReference<IndirectAsyncValue>, 4> result_refs;
  result_refs.reserve(results.size());
  for (int i = 0, e = results.size(); i != e; ++i) {
    auto result = results.AllocateIndirectResultAt(i);
    result_refs.push_back(std::move(result));
  }

  CoreRtWhileLoopIteration(std::move(exec_ctx), std::move(cond_fn_ref),
                           std::move(body_fn_ref), std::move(arg_refs),
                           std::move(result_refs));
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterCreateDenseTensor(KernelRegistry *registry) {
#define REGISTER_CREATE_DENSE_TENSOR(CPP_TYPE, TYPE_NAME)            \
  registry->AddKernel("corert.create_dense_tensor." #TYPE_NAME,      \
                      TFRT_KERNEL(CreateDenseTensor<CPP_TYPE>));     \
  registry->AddKernel("corert_sync.create_dense_tensor." #TYPE_NAME, \
                      TFRT_KERNEL(CreateDenseTensor<CPP_TYPE>))
  REGISTER_CREATE_DENSE_TENSOR(uint8_t, ui8);
  REGISTER_CREATE_DENSE_TENSOR(uint16_t, ui16);
  REGISTER_CREATE_DENSE_TENSOR(uint32_t, ui32);
  REGISTER_CREATE_DENSE_TENSOR(uint64_t, ui64);
  // TODO(b/170482990): Unify bool and i1. We should only use tfrt::i1 for i1
  // dtype data.
  static_assert(
      sizeof(bool) == 1,
      "bool should contain exactly 1 byte for CreateDenseTensor to work.");
  REGISTER_CREATE_DENSE_TENSOR(bool, i1);
  REGISTER_CREATE_DENSE_TENSOR(int8_t, i8);
  REGISTER_CREATE_DENSE_TENSOR(int16_t, i16);
  REGISTER_CREATE_DENSE_TENSOR(int32_t, i32);
  REGISTER_CREATE_DENSE_TENSOR(int64_t, i64);
  REGISTER_CREATE_DENSE_TENSOR(bf16, bf16);
  REGISTER_CREATE_DENSE_TENSOR(float, f32);
  REGISTER_CREATE_DENSE_TENSOR(double, f64);
#undef REGISTER_CREATE_DENSE_TENSOR
}

void RegisterCoreRuntimeKernels(KernelRegistry *registry) {
  registry->AddKernel("corert.tensorhandle_to_shape",
                      TFRT_KERNEL(TensorHandleToShape));
  registry->AddKernel("corert.ht_to_tensorhandle",
                      TFRT_KERNEL(HTToTensorHandle));
  registry->AddKernel("corert.tensorhandle_to_ht",
                      TFRT_KERNEL(TensorHandleToHT));
  registry->AddKernel("corert.print_tensorhandle",
                      TFRT_KERNEL(PrintTensorHandle));
  registry->AddKernel("corert.create_op_attrs", TFRT_KERNEL(CreateOpAttrs));
  registry->AddKernel("corert.op_attrs_set.bool", TFRT_KERNEL(OpAttrsSetBool));
  registry->AddKernel("corert.op_attrs_set.i32",
                      TFRT_KERNEL(OpAttrsSet<int32_t>));
  registry->AddKernel("corert.op_attrs_set_array.i32",
                      TFRT_KERNEL(OpAttrsSetArray<int32_t>));
  registry->AddKernel("corert.op_attrs_set_array.i64",
                      TFRT_KERNEL(OpAttrsSetArray<int64_t>));
  registry->AddKernel("corert.op_attrs_set.f32",
                      TFRT_KERNEL(OpAttrsSet<float>));
  registry->AddKernel("corert.op_attrs_set_array.f32",
                      TFRT_KERNEL(OpAttrsSetArray<float>));
  registry->AddKernel("corert.op_attrs_set.dtype",
                      TFRT_KERNEL(OpAttrsSetDType));
  registry->AddKernel("corert.op_attrs_set.dense",
                      TFRT_KERNEL(OpAttrsSetDense));
  registry->AddKernel("corert.op_attrs_set.aggregate",
                      TFRT_KERNEL(OpAttrsSetAggregate));
  registry->AddKernel("corert.op_attrs_set.shape",
                      TFRT_KERNEL(OpAttrsSetShape));
  registry->AddKernel("corert.op_attrs_set.str", TFRT_KERNEL(OpAttrsSetString));
  registry->AddKernel("corert.executeop", TFRT_KERNEL(ExecuteOp));
  registry->AddKernel("corert.executeop.seq", TFRT_KERNEL(ExecuteOpSeq));
  registry->AddKernel("corert.execute_crt_op",
                      TFRT_KERNEL(ExecuteCoreRuntimeOp));
  registry->AddKernel("corert.make_composite_op", TFRT_KERNEL(MakeCompositeOp));
  registry->AddKernel("corert.get_op_handler", TFRT_KERNEL(GetOpHandler));
  registry->AddKernel("corert.register_op_handler",
                      TFRT_KERNEL(RegisterOpHandler));
  registry->AddKernel("corert.create_logging_op_handler",
                      TFRT_KERNEL(CreateLoggingOpHandlerKernel));
  registry->AddKernel("corert.const_dense_tensor",
                      TFRT_KERNEL(ConstDenseTensor));
  registry->AddKernel("corert.const_string_tensor",
                      TFRT_KERNEL(ConstStringTensor));
  registry->AddKernel("corert.cond", TFRT_KERNEL(CoreRtConditional));
  registry->AddKernel("corert.transfer", TFRT_KERNEL(TransferToDevice));
  registry->AddKernel("corert.while", TFRT_KERNEL(CoreRtWhileLoop));

  registry->AddSyncKernel("corert_sync.print_tensorhandle",
                          TFRT_SYNC_KERNEL(PrintTensorHandleSync));
  registry->AddSyncKernel("corert_sync.get_op_handler",
                          TFRT_SYNC_KERNEL(GetOpHandlerSync));
  registry->AddSyncKernel("corert_sync.register_op_handler",
                          TFRT_SYNC_KERNEL(RegisterOpHandlerSync));
  registry->AddSyncKernel("corert_sync.executeop",
                          TFRT_SYNC_KERNEL(ExecuteOpSync));

  RegisterCreateDenseTensor(registry);
}

}  // namespace tfrt
