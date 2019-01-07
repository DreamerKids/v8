// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_WASM_WASM_ENGINE_H_
#define V8_WASM_WASM_ENGINE_H_

#include <memory>
#include <unordered_set>

#include "src/wasm/wasm-code-manager.h"
#include "src/wasm/wasm-memory.h"
#include "src/wasm/wasm-tier.h"
#include "src/zone/accounting-allocator.h"

namespace v8 {
namespace internal {

class AsmWasmData;
class CodeTracer;
class CompilationStatistics;
class HeapNumber;
class WasmInstanceObject;
class WasmModuleObject;

namespace wasm {

class AsyncCompileJob;
class ErrorThrower;
struct ModuleWireBytes;
struct WasmFeatures;

class V8_EXPORT_PRIVATE CompilationResultResolver {
 public:
  virtual void OnCompilationSucceeded(Handle<WasmModuleObject> result) = 0;
  virtual void OnCompilationFailed(Handle<Object> error_reason) = 0;
  virtual ~CompilationResultResolver() = default;
};

class V8_EXPORT_PRIVATE InstantiationResultResolver {
 public:
  virtual void OnInstantiationSucceeded(Handle<WasmInstanceObject> result) = 0;
  virtual void OnInstantiationFailed(Handle<Object> error_reason) = 0;
  virtual ~InstantiationResultResolver() = default;
};

// The central data structure that represents an engine instance capable of
// loading, instantiating, and executing WASM code.
class V8_EXPORT_PRIVATE WasmEngine {
 public:
  WasmEngine();
  ~WasmEngine();

  // Synchronously validates the given bytes that represent an encoded WASM
  // module.
  bool SyncValidate(Isolate* isolate, const WasmFeatures& enabled,
                    const ModuleWireBytes& bytes);

  // Synchronously compiles the given bytes that represent a translated
  // asm.js module.
  MaybeHandle<AsmWasmData> SyncCompileTranslatedAsmJs(
      Isolate* isolate, ErrorThrower* thrower, const ModuleWireBytes& bytes,
      Vector<const byte> asm_js_offset_table_bytes,
      Handle<HeapNumber> uses_bitset);
  Handle<WasmModuleObject> FinalizeTranslatedAsmJs(
      Isolate* isolate, Handle<AsmWasmData> asm_wasm_data,
      Handle<Script> script);

  // Synchronously compiles the given bytes that represent an encoded WASM
  // module.
  MaybeHandle<WasmModuleObject> SyncCompile(Isolate* isolate,
                                            const WasmFeatures& enabled,
                                            ErrorThrower* thrower,
                                            const ModuleWireBytes& bytes);

  // Synchronously instantiate the given WASM module with the given imports.
  // If the module represents an asm.js module, then the supplied {memory}
  // should be used as the memory of the instance.
  MaybeHandle<WasmInstanceObject> SyncInstantiate(
      Isolate* isolate, ErrorThrower* thrower,
      Handle<WasmModuleObject> module_object, MaybeHandle<JSReceiver> imports,
      MaybeHandle<JSArrayBuffer> memory);

  // Begin an asynchronous compilation of the given bytes that represent an
  // encoded WASM module.
  // The {is_shared} flag indicates if the bytes backing the module could
  // be shared across threads, i.e. could be concurrently modified.
  void AsyncCompile(Isolate* isolate, const WasmFeatures& enabled,
                    std::shared_ptr<CompilationResultResolver> resolver,
                    const ModuleWireBytes& bytes, bool is_shared);

  // Begin an asynchronous instantiation of the given WASM module.
  void AsyncInstantiate(Isolate* isolate,
                        std::unique_ptr<InstantiationResultResolver> resolver,
                        Handle<WasmModuleObject> module_object,
                        MaybeHandle<JSReceiver> imports);

  std::shared_ptr<StreamingDecoder> StartStreamingCompilation(
      Isolate* isolate, const WasmFeatures& enabled, Handle<Context> context,
      std::shared_ptr<CompilationResultResolver> resolver);

  // Compiles the function with the given index at a specific compilation tier.
  // Errors are stored internally in the CompilationState.
  // This is mostly used for testing to force a function into a specific tier.
  void CompileFunction(Isolate* isolate, NativeModule* native_module,
                       uint32_t function_index, ExecutionTier tier);

  // Exports the sharable parts of the given module object so that they can be
  // transferred to a different Context/Isolate using the same engine.
  std::shared_ptr<NativeModule> ExportNativeModule(
      Handle<WasmModuleObject> module_object);

  // Imports the shared part of a module from a different Context/Isolate using
  // the the same engine, recreating a full module object in the given Isolate.
  Handle<WasmModuleObject> ImportNativeModule(
      Isolate* isolate, std::shared_ptr<NativeModule> shared_module);

  WasmCodeManager* code_manager() { return &code_manager_; }

  WasmMemoryTracker* memory_tracker() { return &memory_tracker_; }

  AccountingAllocator* allocator() { return &allocator_; }

  // Compilation statistics for TurboFan compilations.
  CompilationStatistics* GetOrCreateTurboStatistics();

  // Prints the gathered compilation statistics, then resets them.
  void DumpAndResetTurboStatistics();

  // Used to redirect tracing output from {stdout} to a file.
  CodeTracer* GetCodeTracer();

  // Remove {job} from the list of active compile jobs.
  void RemoveCompileJob(AsyncCompileJob* job);

  // Returns true if at least one AsyncCompileJob that belongs to the given
  // Isolate is currently running.
  bool HasRunningCompileJob(Isolate* isolate);

  // Aborts all AsyncCompileJobs that belong to the given Isolate. All
  // compilation is aborted, no more callbacks will be triggered. This is used
  // for tearing down an isolate, or to clean it up to be reused.
  void AbortCompileJobsOnIsolate(Isolate* isolate);

  // Manage the set of Isolates that use this WasmEngine.
  void AddIsolate(Isolate* isolate);
  void RemoveIsolate(Isolate* isolate);

  // Call on process start and exit.
  static void InitializeOncePerProcess();
  static void GlobalTearDown();

  // Constructs a WasmEngine instance. Depending on whether we are sharing
  // engines this might be a pointer to a new instance or to a shared one.
  static std::shared_ptr<WasmEngine> GetWasmEngine();

 private:
  std::shared_ptr<AsyncCompileJob> CreateAsyncCompileJob(
      Isolate* isolate, const WasmFeatures& enabled,
      std::unique_ptr<byte[]> bytes_copy, size_t length,
      Handle<Context> context,
      std::shared_ptr<CompilationResultResolver> resolver);

  WasmMemoryTracker memory_tracker_;
  WasmCodeManager code_manager_;
  AccountingAllocator allocator_;

  // This mutex protects all information which is mutated concurrently or
  // fields that are initialized lazily on the first access.
  base::Mutex mutex_;

  //////////////////////////////////////////////////////////////////////////////
  // Protected by {mutex_}:

  // Keep weak_ptrs to the AsyncCompileJob so we can detect the intermediate
  // state where the refcount already dropped to zero (and the weak_ptr is
  // cleared) but the destructor did not run to completion yet.
  std::unordered_map<AsyncCompileJob*, std::weak_ptr<AsyncCompileJob>>
      async_compile_jobs_;

  std::unique_ptr<CompilationStatistics> compilation_stats_;
  std::unique_ptr<CodeTracer> code_tracer_;

  // Set of isolates which use this WasmEngine. Used for cross-isolate GCs.
  std::unordered_set<Isolate*> isolates_;

  // End of fields protected by {mutex_}.
  //////////////////////////////////////////////////////////////////////////////

  DISALLOW_COPY_AND_ASSIGN(WasmEngine);
};

}  // namespace wasm
}  // namespace internal
}  // namespace v8

#endif  // V8_WASM_WASM_ENGINE_H_
