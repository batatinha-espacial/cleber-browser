/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/HelperThreads.h"

#include "mozilla/ReverseIterator.h"  // mozilla::Reversed(...)
#include "mozilla/ScopeExit.h"
#include "mozilla/Span.h"  // mozilla::Span<TaggedScriptThingIndex>
#include "mozilla/Utf8.h"  // mozilla::Utf8Unit

#include <algorithm>

#include "frontend/CompilationStencil.h"  // frontend::{CompilationStencil, ExtensibleCompilationStencil, CompilationInput}
#include "frontend/FrontendContext.h"
#include "gc/GC.h"
#include "jit/IonCompileTask.h"
#include "jit/JitRuntime.h"
#include "jit/JitScript.h"
#include "js/CompileOptions.h"  // JS::CompileOptions, JS::ReadOnlyDecodeOptions, JS::ReadOnlyCompileOptions
#include "js/experimental/CompileScript.h"
#include "js/experimental/JSStencil.h"
#include "js/friend/StackLimits.h"  // js::ReportOverRecursed
#include "js/HelperThreadAPI.h"
#include "js/OffThreadScriptCompilation.h"  // JS::OffThreadToken, JS::OffThreadCompileCallback
#include "js/SourceText.h"
#include "js/Stack.h"
#include "js/Transcoding.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "threading/CpuCount.h"
#include "vm/ErrorReporting.h"
#include "vm/HelperThreadState.h"
#include "vm/InternalThreadPool.h"
#include "vm/MutexIDs.h"
#include "wasm/WasmGenerator.h"

using namespace js;

using mozilla::TimeDuration;
using mozilla::TimeStamp;
using mozilla::Utf8Unit;

using JS::CompileOptions;
using JS::DispatchReason;
using JS::ReadOnlyCompileOptions;

namespace js {

Mutex gHelperThreadLock(mutexid::GlobalHelperThreadState);
GlobalHelperThreadState* gHelperThreadState = nullptr;

}  // namespace js

bool js::CreateHelperThreadsState() {
  MOZ_ASSERT(!gHelperThreadState);
  gHelperThreadState = js_new<GlobalHelperThreadState>();
  return gHelperThreadState;
}

void js::DestroyHelperThreadsState() {
  AutoLockHelperThreadState lock;

  if (!gHelperThreadState) {
    return;
  }

  gHelperThreadState->finish(lock);
  js_delete(gHelperThreadState);
  gHelperThreadState = nullptr;
}

bool js::EnsureHelperThreadsInitialized() {
  MOZ_ASSERT(gHelperThreadState);
  return gHelperThreadState->ensureInitialized();
}

static size_t ClampDefaultCPUCount(size_t cpuCount) {
  // It's extremely rare for SpiderMonkey to have more than a few cores worth
  // of work. At higher core counts, performance can even decrease due to NUMA
  // (and SpiderMonkey's lack of NUMA-awareness), contention, and general lack
  // of optimization for high core counts. So to avoid wasting thread stack
  // resources (and cluttering gdb and core dumps), clamp to 8 cores for now.
  return std::min<size_t>(cpuCount, 8);
}

static size_t ThreadCountForCPUCount(size_t cpuCount) {
  // We need at least two threads for tier-2 wasm compilations, because
  // there's a master task that holds a thread while other threads do the
  // compilation.
  return std::max<size_t>(cpuCount, 2);
}

bool js::SetFakeCPUCount(size_t count) {
  HelperThreadState().setCpuCount(count);
  return true;
}

void GlobalHelperThreadState::setCpuCount(size_t count) {
  // This must be called before any threads have been initialized.
  AutoLockHelperThreadState lock;
  MOZ_ASSERT(!isInitialized(lock));

  // We can't do this if an external thread pool is in use.
  MOZ_ASSERT(!dispatchTaskCallback);

  cpuCount = count;
  threadCount = ThreadCountForCPUCount(count);
}

size_t js::GetHelperThreadCount() { return HelperThreadState().threadCount; }

size_t js::GetHelperThreadCPUCount() { return HelperThreadState().cpuCount; }

size_t js::GetMaxWasmCompilationThreads() {
  return HelperThreadState().maxWasmCompilationThreads();
}

void JS::SetProfilingThreadCallbacks(
    JS::RegisterThreadCallback registerThread,
    JS::UnregisterThreadCallback unregisterThread) {
  HelperThreadState().registerThread = registerThread;
  HelperThreadState().unregisterThread = unregisterThread;
}

static size_t ThreadStackQuotaForSize(size_t size) {
  // Set the stack quota to 10% less that the actual size.
  return size_t(double(size) * 0.9);
}

// Bug 1630189: Without MOZ_NEVER_INLINE, Windows PGO builds have a linking
// error for HelperThreadTaskCallback.
JS_PUBLIC_API MOZ_NEVER_INLINE void JS::SetHelperThreadTaskCallback(
    HelperThreadTaskCallback callback, size_t threadCount, size_t stackSize) {
  AutoLockHelperThreadState lock;
  HelperThreadState().setDispatchTaskCallback(callback, threadCount, stackSize,
                                              lock);
}

void GlobalHelperThreadState::setDispatchTaskCallback(
    JS::HelperThreadTaskCallback callback, size_t threadCount, size_t stackSize,
    const AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(!isInitialized(lock));
  MOZ_ASSERT(!dispatchTaskCallback);
  MOZ_ASSERT(threadCount != 0);
  MOZ_ASSERT(stackSize >= 16 * 1024);

  dispatchTaskCallback = callback;
  this->threadCount = threadCount;
  this->stackQuota = ThreadStackQuotaForSize(stackSize);
}

bool js::StartOffThreadWasmCompile(wasm::CompileTask* task,
                                   wasm::CompileMode mode) {
  return HelperThreadState().submitTask(task, mode);
}

bool GlobalHelperThreadState::submitTask(wasm::CompileTask* task,
                                         wasm::CompileMode mode) {
  AutoLockHelperThreadState lock;
  if (!wasmWorklist(lock, mode).pushBack(task)) {
    return false;
  }

  dispatch(DispatchReason::NewTask, lock);
  return true;
}

size_t js::RemovePendingWasmCompileTasks(
    const wasm::CompileTaskState& taskState, wasm::CompileMode mode,
    const AutoLockHelperThreadState& lock) {
  wasm::CompileTaskPtrFifo& worklist =
      HelperThreadState().wasmWorklist(lock, mode);
  return worklist.eraseIf([&taskState](wasm::CompileTask* task) {
    return &task->state == &taskState;
  });
}

void js::StartOffThreadWasmTier2Generator(wasm::UniqueTier2GeneratorTask task) {
  (void)HelperThreadState().submitTask(std::move(task));
}

bool GlobalHelperThreadState::submitTask(wasm::UniqueTier2GeneratorTask task) {
  AutoLockHelperThreadState lock;

  MOZ_ASSERT(isInitialized(lock));

  if (!wasmTier2GeneratorWorklist(lock).append(task.get())) {
    return false;
  }
  (void)task.release();

  dispatch(DispatchReason::NewTask, lock);
  return true;
}

static void CancelOffThreadWasmTier2GeneratorLocked(
    AutoLockHelperThreadState& lock) {
  if (!HelperThreadState().isInitialized(lock)) {
    return;
  }

  // Remove pending tasks from the tier2 generator worklist and cancel and
  // delete them.
  {
    wasm::Tier2GeneratorTaskPtrVector& worklist =
        HelperThreadState().wasmTier2GeneratorWorklist(lock);
    for (size_t i = 0; i < worklist.length(); i++) {
      wasm::Tier2GeneratorTask* task = worklist[i];
      HelperThreadState().remove(worklist, &i);
      js_delete(task);
    }
  }

  // There is at most one running Tier2Generator task and we assume that
  // below.
  static_assert(GlobalHelperThreadState::MaxTier2GeneratorTasks == 1,
                "code must be generalized");

  // If there is a running Tier2 generator task, shut it down in a predictable
  // way.  The task will be deleted by the normal deletion logic.
  for (auto* helper : HelperThreadState().helperTasks(lock)) {
    if (helper->is<wasm::Tier2GeneratorTask>()) {
      // Set a flag that causes compilation to shortcut itself.
      helper->as<wasm::Tier2GeneratorTask>()->cancel();

      // Wait for the generator task to finish.  This avoids a shutdown race
      // where the shutdown code is trying to shut down helper threads and the
      // ongoing tier2 compilation is trying to finish, which requires it to
      // have access to helper threads.
      uint32_t oldFinishedCount =
          HelperThreadState().wasmTier2GeneratorsFinished(lock);
      while (HelperThreadState().wasmTier2GeneratorsFinished(lock) ==
             oldFinishedCount) {
        HelperThreadState().wait(lock);
      }

      // At most one of these tasks.
      break;
    }
  }
}

void js::CancelOffThreadWasmTier2Generator() {
  AutoLockHelperThreadState lock;
  CancelOffThreadWasmTier2GeneratorLocked(lock);
}

bool js::StartOffThreadIonCompile(jit::IonCompileTask* task,
                                  const AutoLockHelperThreadState& lock) {
  return HelperThreadState().submitTask(task, lock);
}

bool GlobalHelperThreadState::submitTask(
    jit::IonCompileTask* task, const AutoLockHelperThreadState& locked) {
  MOZ_ASSERT(isInitialized(locked));

  if (!ionWorklist(locked).append(task)) {
    return false;
  }

  // The build is moving off-thread. Freeze the LifoAlloc to prevent any
  // unwanted mutations.
  task->alloc().lifoAlloc()->setReadOnly();

  dispatch(DispatchReason::NewTask, locked);
  return true;
}

bool js::StartOffThreadIonFree(jit::IonCompileTask* task,
                               const AutoLockHelperThreadState& lock) {
  js::UniquePtr<jit::IonFreeTask> freeTask =
      js::MakeUnique<jit::IonFreeTask>(task);
  if (!freeTask) {
    return false;
  }

  return HelperThreadState().submitTask(std::move(freeTask), lock);
}

bool GlobalHelperThreadState::submitTask(
    UniquePtr<jit::IonFreeTask> task, const AutoLockHelperThreadState& locked) {
  MOZ_ASSERT(isInitialized(locked));

  if (!ionFreeList(locked).append(std::move(task))) {
    return false;
  }

  dispatch(DispatchReason::NewTask, locked);
  return true;
}

/*
 * Move an IonCompilationTask for which compilation has either finished, failed,
 * or been cancelled into the global finished compilation list. All off thread
 * compilations which are started must eventually be finished.
 */
void js::FinishOffThreadIonCompile(jit::IonCompileTask* task,
                                   const AutoLockHelperThreadState& lock) {
  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!HelperThreadState().ionFinishedList(lock).append(task)) {
    oomUnsafe.crash("FinishOffThreadIonCompile");
  }
  task->script()
      ->runtimeFromAnyThread()
      ->jitRuntime()
      ->numFinishedOffThreadTasksRef(lock)++;
}

static JSRuntime* GetSelectorRuntime(const CompilationSelector& selector) {
  struct Matcher {
    JSRuntime* operator()(JSScript* script) {
      return script->runtimeFromMainThread();
    }
    JSRuntime* operator()(Zone* zone) { return zone->runtimeFromMainThread(); }
    JSRuntime* operator()(ZonesInState zbs) { return zbs.runtime; }
    JSRuntime* operator()(JSRuntime* runtime) { return runtime; }
  };

  return selector.match(Matcher());
}

static bool JitDataStructuresExist(const CompilationSelector& selector) {
  struct Matcher {
    bool operator()(JSScript* script) { return !!script->zone()->jitZone(); }
    bool operator()(Zone* zone) { return !!zone->jitZone(); }
    bool operator()(ZonesInState zbs) { return zbs.runtime->hasJitRuntime(); }
    bool operator()(JSRuntime* runtime) { return runtime->hasJitRuntime(); }
  };

  return selector.match(Matcher());
}

static bool IonCompileTaskMatches(const CompilationSelector& selector,
                                  jit::IonCompileTask* task) {
  struct TaskMatches {
    jit::IonCompileTask* task_;

    bool operator()(JSScript* script) { return script == task_->script(); }
    bool operator()(Zone* zone) {
      return zone == task_->script()->zoneFromAnyThread();
    }
    bool operator()(JSRuntime* runtime) {
      return runtime == task_->script()->runtimeFromAnyThread();
    }
    bool operator()(ZonesInState zbs) {
      return zbs.runtime == task_->script()->runtimeFromAnyThread() &&
             zbs.state == task_->script()->zoneFromAnyThread()->gcState();
    }
  };

  return selector.match(TaskMatches{task});
}

static void CancelOffThreadIonCompileLocked(const CompilationSelector& selector,
                                            AutoLockHelperThreadState& lock) {
  if (!HelperThreadState().isInitialized(lock)) {
    return;
  }

  MOZ_ASSERT(GetSelectorRuntime(selector)->jitRuntime() != nullptr);

  /* Cancel any pending entries for which processing hasn't started. */
  GlobalHelperThreadState::IonCompileTaskVector& worklist =
      HelperThreadState().ionWorklist(lock);
  for (size_t i = 0; i < worklist.length(); i++) {
    jit::IonCompileTask* task = worklist[i];
    if (IonCompileTaskMatches(selector, task)) {
      // Once finished, tasks are added to a Linked list which is
      // allocated with the IonCompileTask class. The IonCompileTask is
      // allocated in the LifoAlloc so we need the LifoAlloc to be mutable.
      worklist[i]->alloc().lifoAlloc()->setReadWrite();

      FinishOffThreadIonCompile(task, lock);
      HelperThreadState().remove(worklist, &i);
    }
  }

  /* Wait for in progress entries to finish up. */
  bool cancelled;
  do {
    cancelled = false;
    for (auto* helper : HelperThreadState().helperTasks(lock)) {
      if (!helper->is<jit::IonCompileTask>()) {
        continue;
      }

      jit::IonCompileTask* ionCompileTask = helper->as<jit::IonCompileTask>();
      if (IonCompileTaskMatches(selector, ionCompileTask)) {
        ionCompileTask->mirGen().cancel();
        cancelled = true;
      }
    }
    if (cancelled) {
      HelperThreadState().wait(lock);
    }
  } while (cancelled);

  /* Cancel code generation for any completed entries. */
  GlobalHelperThreadState::IonCompileTaskVector& finished =
      HelperThreadState().ionFinishedList(lock);
  for (size_t i = 0; i < finished.length(); i++) {
    jit::IonCompileTask* task = finished[i];
    if (IonCompileTaskMatches(selector, task)) {
      JSRuntime* rt = task->script()->runtimeFromAnyThread();
      rt->jitRuntime()->numFinishedOffThreadTasksRef(lock)--;
      jit::FinishOffThreadTask(rt, task, lock);
      HelperThreadState().remove(finished, &i);
    }
  }

  /* Cancel lazy linking for pending tasks (attached to the ionScript). */
  JSRuntime* runtime = GetSelectorRuntime(selector);
  jit::IonCompileTask* task =
      runtime->jitRuntime()->ionLazyLinkList(runtime).getFirst();
  while (task) {
    jit::IonCompileTask* next = task->getNext();
    if (IonCompileTaskMatches(selector, task)) {
      jit::FinishOffThreadTask(runtime, task, lock);
    }
    task = next;
  }
}

void js::CancelOffThreadIonCompile(const CompilationSelector& selector) {
  if (!JitDataStructuresExist(selector)) {
    return;
  }

  AutoLockHelperThreadState lock;
  CancelOffThreadIonCompileLocked(selector, lock);
}

#ifdef DEBUG
bool js::HasOffThreadIonCompile(Zone* zone) {
  AutoLockHelperThreadState lock;

  if (!HelperThreadState().isInitialized(lock)) {
    return false;
  }

  GlobalHelperThreadState::IonCompileTaskVector& worklist =
      HelperThreadState().ionWorklist(lock);
  for (size_t i = 0; i < worklist.length(); i++) {
    jit::IonCompileTask* task = worklist[i];
    if (task->script()->zoneFromAnyThread() == zone) {
      return true;
    }
  }

  for (auto* helper : HelperThreadState().helperTasks(lock)) {
    if (!helper->is<jit::IonCompileTask>()) {
      continue;
    }
    JSScript* script = helper->as<jit::IonCompileTask>()->script();
    if (script->zoneFromAnyThread() == zone) {
      return true;
    }
  }

  GlobalHelperThreadState::IonCompileTaskVector& finished =
      HelperThreadState().ionFinishedList(lock);
  for (size_t i = 0; i < finished.length(); i++) {
    jit::IonCompileTask* task = finished[i];
    if (task->script()->zoneFromAnyThread() == zone) {
      return true;
    }
  }

  JSRuntime* rt = zone->runtimeFromMainThread();
  jit::IonCompileTask* task = rt->jitRuntime()->ionLazyLinkList(rt).getFirst();
  while (task) {
    if (task->script()->zone() == zone) {
      return true;
    }
    task = task->getNext();
  }

  return false;
}
#endif

ParseTask::ParseTask(ParseTaskKind kind, JSContext* cx,
                     JS::OffThreadCompileCallback callback, void* callbackData)
    : kind(kind), options(cx), callback(callback), callbackData(callbackData) {}

bool ParseTask::init(JSContext* cx, const ReadOnlyCompileOptions& options) {
  if (!this->options.copy(cx, options)) {
    return false;
  }

  runtime = cx->runtime();

  if (!fc_.allocateOwnedPool()) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

void ParseTask::moveInstantiationStorageInto(
    JS::InstantiationStorage& storage) {
  storage.gcOutput_ = instantiationStorage_.gcOutput_;
  instantiationStorage_.gcOutput_ = nullptr;
}

ParseTask::~ParseTask() {
  // The LinkedListElement destructor will remove us from any list we are part
  // of without synchronization, so ensure that doesn't happen.
  MOZ_DIAGNOSTIC_ASSERT(!isInList());
}

size_t ParseTask::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  size_t compileStorageSize = compileStorage_.sizeOfIncludingThis(mallocSizeOf);
  size_t stencilSize =
      stencil_ ? stencil_->sizeOfIncludingThis(mallocSizeOf) : 0;
  size_t gcOutputSize =
      instantiationStorage_.gcOutput_
          ? instantiationStorage_.gcOutput_->sizeOfExcludingThis(mallocSizeOf)
          : 0;

  // TODO: 'errors' requires adding support to `CompileError`. They are not
  // common though.

  return options.sizeOfExcludingThis(mallocSizeOf) + compileStorageSize +
         stencilSize + gcOutputSize;
}

void ParseTask::runHelperThreadTask(AutoLockHelperThreadState& locked) {
  runTask(locked);

  // The callback is invoked while we are still off thread.
  callback(this, callbackData);

  // FinishOffThreadScript will need to be called on the script to
  // migrate it into the correct compartment.
  HelperThreadState().parseFinishedList(locked).insertBack(this);
}

void ParseTask::runTask(AutoLockHelperThreadState& lock) {
  fc_.setStackQuota(HelperThreadState().stackQuota);

  AutoUnlockHelperThreadState unlock(lock);

  parse(&fc_);

  fc_.nameCollectionPool().purge();
}

template <typename Unit>
struct CompileToStencilTask : public ParseTask {
  JS::SourceText<Unit> data;

  CompileToStencilTask(JSContext* cx, JS::SourceText<Unit>& srcBuf,
                       JS::OffThreadCompileCallback callback,
                       void* callbackData);
  void parse(FrontendContext* fc) override;
};

template <typename Unit>
struct CompileModuleToStencilTask : public ParseTask {
  JS::SourceText<Unit> data;

  CompileModuleToStencilTask(JSContext* cx, JS::SourceText<Unit>& srcBuf,
                             JS::OffThreadCompileCallback callback,
                             void* callbackData);
  void parse(FrontendContext* fc) override;
};

struct DecodeStencilTask : public ParseTask {
  const JS::TranscodeRange range;

  DecodeStencilTask(JSContext* cx, const JS::TranscodeRange& range,
                    JS::OffThreadCompileCallback callback, void* callbackData);
  void parse(FrontendContext* fc) override;
};

template <typename Unit>
CompileToStencilTask<Unit>::CompileToStencilTask(
    JSContext* cx, JS::SourceText<Unit>& srcBuf,
    JS::OffThreadCompileCallback callback, void* callbackData)
    : ParseTask(ParseTaskKind::ScriptStencil, cx, callback, callbackData),
      data(std::move(srcBuf)) {}

template <typename Unit>
void CompileToStencilTask<Unit>::parse(FrontendContext* fc) {
  stencil_ =
      JS::CompileGlobalScriptToStencil(fc, options, data, compileStorage_);
  if (!stencil_) {
    return;
  }

  if (options.allocateInstantiationStorage) {
    if (!JS::PrepareForInstantiate(fc, *stencil_, instantiationStorage_)) {
      stencil_ = nullptr;
    }
  }
}

template <typename Unit>
CompileModuleToStencilTask<Unit>::CompileModuleToStencilTask(
    JSContext* cx, JS::SourceText<Unit>& srcBuf,
    JS::OffThreadCompileCallback callback, void* callbackData)
    : ParseTask(ParseTaskKind::ModuleStencil, cx, callback, callbackData),
      data(std::move(srcBuf)) {}

template <typename Unit>
void CompileModuleToStencilTask<Unit>::parse(FrontendContext* fc) {
  stencil_ =
      JS::CompileModuleScriptToStencil(fc, options, data, compileStorage_);
  if (!stencil_) {
    return;
  }

  if (options.allocateInstantiationStorage) {
    if (!JS::PrepareForInstantiate(fc, *stencil_, instantiationStorage_)) {
      stencil_ = nullptr;
    }
  }
}

DecodeStencilTask::DecodeStencilTask(JSContext* cx,
                                     const JS::TranscodeRange& range,
                                     JS::OffThreadCompileCallback callback,
                                     void* callbackData)
    : ParseTask(ParseTaskKind::StencilDecode, cx, callback, callbackData),
      range(range) {
  MOZ_ASSERT(JS::IsTranscodingBytecodeAligned(range.begin().get()));
}

static void ReportDecodeFailure(JS::FrontendContext* fc) {
  js::ErrorMetadata metadata;
  metadata.filename = JS::ConstUTF8CharsZ("<unknown>");
  metadata.lineNumber = 0;
  metadata.columnNumber = 0;
  metadata.lineLength = 0;
  metadata.tokenOffset = 0;
  metadata.isMuted = false;

  js::ReportCompileErrorLatin1(fc, std::move(metadata), nullptr,
                               JSMSG_DECODE_FAILURE);
}

void DecodeStencilTask::parse(FrontendContext* fc) {
  JS::DecodeOptions decodeOptions(options);

  JS::TranscodeResult tr =
      JS::DecodeStencil(fc, decodeOptions, range, getter_AddRefs(stencil_));
  if (tr != JS::TranscodeResult::Ok) {
    if (tr != JS::TranscodeResult::Throw) {
      ReportDecodeFailure(fc);
    }
    return;
  }

  if (options.allocateInstantiationStorage) {
    if (!JS::PrepareForInstantiate(fc, *stencil_, instantiationStorage_)) {
      stencil_ = nullptr;
    }
  }
}

void js::StartOffThreadDelazification(
    JSContext* maybeCx, const ReadOnlyCompileOptions& options,
    const frontend::CompilationStencil& stencil) {
  // Skip delazify tasks if we parse everything on-demand or ahead.
  auto strategy = options.eagerDelazificationStrategy();
  if (strategy == JS::DelazificationOption::OnDemandOnly ||
      strategy == JS::DelazificationOption::ParseEverythingEagerly) {
    return;
  }

  // Skip delazify task if code coverage is enabled.
  if (maybeCx && maybeCx->realm()->collectCoverageForDebug()) {
    return;
  }

  if (!CanUseExtraThreads()) {
    return;
  }

  JSRuntime* maybeRuntime = maybeCx ? maybeCx->runtime() : nullptr;
  UniquePtr<DelazifyTask> task;
  task = DelazifyTask::Create(maybeRuntime, options, stencil);
  if (!task) {
    return;
  }

  // Schedule delazification task if there is any function to delazify.
  if (!task->done()) {
    AutoLockHelperThreadState lock;
    HelperThreadState().submitTask(task.release(), lock);
  }
}

UniquePtr<DelazifyTask> DelazifyTask::Create(
    JSRuntime* maybeRuntime, const JS::ReadOnlyCompileOptions& options,
    const frontend::CompilationStencil& stencil) {
  UniquePtr<DelazifyTask> task;
  task.reset(js_new<DelazifyTask>(maybeRuntime, options.prefableOptions()));
  if (!task) {
    return nullptr;
  }

  if (!task->init(options, stencil)) {
    // In case of errors, skip this and delazify on-demand.
    return nullptr;
  }

  return task;
}

DelazifyTask::DelazifyTask(
    JSRuntime* maybeRuntime,
    const JS::PrefableCompileOptions& initialPrefableOptions)
    : maybeRuntime(maybeRuntime),
      delazificationCx(initialPrefableOptions, HelperThreadState().stackQuota) {
}

DelazifyTask::~DelazifyTask() {
  // The LinkedListElement destructor will remove us from any list we are part
  // of without synchronization, so ensure that doesn't happen.
  MOZ_DIAGNOSTIC_ASSERT(!isInList());
}

bool DelazifyTask::init(const JS::ReadOnlyCompileOptions& options,
                        const frontend::CompilationStencil& stencil) {
  return delazificationCx.init(options, stencil);
}

size_t DelazifyTask::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return delazificationCx.sizeOfExcludingThis(mallocSizeOf);
}

void DelazifyTask::runHelperThreadTask(AutoLockHelperThreadState& lock) {
  {
    AutoUnlockHelperThreadState unlock(lock);
    // NOTE: We do not report errors beyond this scope, as there is no where
    // to report these errors to. In the mean time, prevent the eager
    // delazification from running after any kind of errors.
    (void)runTask();
  }

  // If we should continue to delazify even more functions, then re-add this
  // task to the vector of delazification tasks. This might happen when the
  // DelazifyTask is interrupted by a higher priority task. (see
  // mozilla::TaskController & mozilla::Task)
  if (!delazificationCx.done()) {
    HelperThreadState().submitTask(this, lock);
  } else {
    UniquePtr<FreeDelazifyTask> freeTask(js_new<FreeDelazifyTask>(this));
    if (freeTask) {
      HelperThreadState().submitTask(std::move(freeTask), lock);
    }
  }
}

bool DelazifyTask::runTask() { return delazificationCx.delazify(); }

bool DelazifyTask::done() const { return delazificationCx.done(); }

void FreeDelazifyTask::runHelperThreadTask(AutoLockHelperThreadState& locked) {
  {
    AutoUnlockHelperThreadState unlock(locked);
    js_delete(task);
    task = nullptr;
  }

  js_delete(this);
}

static void WaitForOffThreadParses(JSRuntime* rt,
                                   AutoLockHelperThreadState& lock) {
  if (!HelperThreadState().isInitialized(lock)) {
    return;
  }

  GlobalHelperThreadState::ParseTaskVector& worklist =
      HelperThreadState().parseWorklist(lock);

  while (true) {
    bool pending = false;
    for (const auto& task : worklist) {
      if (task->runtimeMatches(rt)) {
        pending = true;
        break;
      }
    }
    if (!pending) {
      bool inProgress = false;
      for (auto* helper : HelperThreadState().helperTasks(lock)) {
        if (helper->is<ParseTask>() &&
            helper->as<ParseTask>()->runtimeMatches(rt)) {
          inProgress = true;
          break;
        }
      }
      if (!inProgress) {
        break;
      }
    }
    HelperThreadState().wait(lock);
  }

#ifdef DEBUG
  for (const auto& task : worklist) {
    MOZ_ASSERT(!task->runtimeMatches(rt));
  }
  for (auto* helper : HelperThreadState().helperTasks(lock)) {
    MOZ_ASSERT_IF(helper->is<ParseTask>(),
                  !helper->as<ParseTask>()->runtimeMatches(rt));
  }
#endif
}

void js::CancelOffThreadParses(JSRuntime* rt) {
  AutoLockHelperThreadState lock;

  // Instead of forcibly canceling pending parse tasks, just wait for all
  // scheduled and in progress ones to complete. Otherwise the final GC may not
  // collect everything due to zones being used off thread.
  WaitForOffThreadParses(rt, lock);

  // Clean up any parse tasks which haven't been finished by the main thread.
  auto& finished = HelperThreadState().parseFinishedList(lock);
  while (true) {
    bool found = false;
    ParseTask* next;
    ParseTask* task = finished.getFirst();
    while (task) {
      next = task->getNext();
      if (task->runtimeMatches(rt)) {
        found = true;
        task->remove();
        HelperThreadState().destroyParseTask(rt, task);
      }
      task = next;
    }
    if (!found) {
      break;
    }
  }

#ifdef DEBUG
  for (ParseTask* task : finished) {
    MOZ_ASSERT(!task->runtimeMatches(rt));
  }
#endif
}

static void CancelPendingDelazifyTask(JSRuntime* rt,
                                      AutoLockHelperThreadState& lock) {
  auto& delazifyList = HelperThreadState().delazifyWorklist(lock);

  auto end = delazifyList.end();
  for (auto iter = delazifyList.begin(); iter != end;) {
    DelazifyTask* task = *iter;
    ++iter;
    if (task->runtimeMatchesOrNoRuntime(rt)) {
      task->removeFrom(delazifyList);
      js_delete(task);
    }
  }
}

static void WaitUntilCancelledDelazifyTasks(JSRuntime* rt,
                                            AutoLockHelperThreadState& lock) {
  if (!HelperThreadState().isInitialized(lock)) {
    return;
  }

  while (true) {
    CancelPendingDelazifyTask(rt, lock);

    // If running tasks are delazifying any functions, then we have to wait
    // until they complete to remove them from the pending list. DelazifyTask
    // are inserting themself back to be processed once more after delazifying a
    // function.
    bool inProgress = false;
    for (auto* helper : HelperThreadState().helperTasks(lock)) {
      if (helper->is<DelazifyTask>() &&
          helper->as<DelazifyTask>()->runtimeMatchesOrNoRuntime(rt)) {
        inProgress = true;
        break;
      }
    }
    if (!inProgress) {
      break;
    }

    HelperThreadState().wait(lock);
  }

#ifdef DEBUG
  for (DelazifyTask* task : HelperThreadState().delazifyWorklist(lock)) {
    MOZ_ASSERT(!task->runtimeMatchesOrNoRuntime(rt));
  }
  for (auto* helper : HelperThreadState().helperTasks(lock)) {
    MOZ_ASSERT_IF(helper->is<DelazifyTask>(),
                  !helper->as<DelazifyTask>()->runtimeMatchesOrNoRuntime(rt));
  }
#endif
}

static void WaitUntilEmptyFreeDelazifyTaskVector(
    AutoLockHelperThreadState& lock) {
  if (!HelperThreadState().isInitialized(lock)) {
    return;
  }

  while (true) {
    bool inProgress = false;
    auto& freeList = HelperThreadState().freeDelazifyTaskVector(lock);
    if (!freeList.empty()) {
      inProgress = true;
    }

    // If running tasks are delazifying any functions, then we have to wait
    // until they complete to remove them from the pending list. DelazifyTask
    // are inserting themself back to be processed once more after delazifying a
    // function.
    for (auto* helper : HelperThreadState().helperTasks(lock)) {
      if (helper->is<FreeDelazifyTask>()) {
        inProgress = true;
        break;
      }
    }
    if (!inProgress) {
      break;
    }

    HelperThreadState().wait(lock);
  }
}

void js::CancelOffThreadDelazify(JSRuntime* runtime) {
  AutoLockHelperThreadState lock;

  // Cancel all Delazify tasks from the given runtime, and wait if tasks are
  // from the given runtime are being executed.
  WaitUntilCancelledDelazifyTasks(runtime, lock);

  // Empty the free list of delazify task, in case one of the delazify task
  // ended and therefore did not returned to the pending list of delazify tasks.
  WaitUntilEmptyFreeDelazifyTaskVector(lock);
}

static bool HasAnyDelazifyTask(JSRuntime* rt, AutoLockHelperThreadState& lock) {
  auto& delazifyList = HelperThreadState().delazifyWorklist(lock);
  for (auto task : delazifyList) {
    if (task->runtimeMatchesOrNoRuntime(rt)) {
      return true;
    }
  }

  for (auto* helper : HelperThreadState().helperTasks(lock)) {
    if (helper->is<DelazifyTask>() &&
        helper->as<DelazifyTask>()->runtimeMatchesOrNoRuntime(rt)) {
      return true;
    }
  }

  return false;
}

void js::WaitForAllDelazifyTasks(JSRuntime* rt) {
  AutoLockHelperThreadState lock;
  if (!HelperThreadState().isInitialized(lock)) {
    return;
  }

  while (true) {
    if (!HasAnyDelazifyTask(rt, lock)) {
      break;
    }

    HelperThreadState().wait(lock);
  }
}

static bool QueueOffThreadParseTask(JSContext* cx, UniquePtr<ParseTask> task) {
  AutoLockHelperThreadState lock;

  bool result =
      HelperThreadState().submitTask(cx->runtime(), std::move(task), lock);

  if (!result) {
    ReportOutOfMemory(cx);
  }
  return result;
}

bool GlobalHelperThreadState::submitTask(
    JSRuntime* rt, UniquePtr<ParseTask> task,
    const AutoLockHelperThreadState& locked) {
  if (!parseWorklist(locked).append(std::move(task))) {
    return false;
  }

  dispatch(DispatchReason::NewTask, locked);
  return true;
}

void GlobalHelperThreadState::submitTask(
    DelazifyTask* task, const AutoLockHelperThreadState& locked) {
  delazifyWorklist(locked).insertBack(task);
  dispatch(DispatchReason::NewTask, locked);
}

bool GlobalHelperThreadState::submitTask(
    UniquePtr<FreeDelazifyTask> task, const AutoLockHelperThreadState& locked) {
  if (!freeDelazifyTaskVector(locked).append(std::move(task))) {
    return false;
  }
  dispatch(DispatchReason::NewTask, locked);
  return true;
}

static JS::OffThreadToken* StartOffThreadParseTask(
    JSContext* cx, UniquePtr<ParseTask> task,
    const ReadOnlyCompileOptions& options) {
  // Suppress GC so that calls below do not trigger a new incremental GC
  // which could require barriers on the atoms zone.
  gc::AutoSuppressGC nogc(cx);

  if (!task->init(cx, options)) {
    return nullptr;
  }

  JS::OffThreadToken* token = task.get();
  if (!QueueOffThreadParseTask(cx, std::move(task))) {
    return nullptr;
  }

  // Return an opaque pointer to caller so that it may query/cancel the task
  // before the callback is fired.
  return token;
}

template <typename Unit>
static JS::OffThreadToken* StartOffThreadCompileToStencilInternal(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    JS::SourceText<Unit>& srcBuf, JS::OffThreadCompileCallback callback,
    void* callbackData) {
  auto task = cx->make_unique<CompileToStencilTask<Unit>>(cx, srcBuf, callback,
                                                          callbackData);
  if (!task) {
    return nullptr;
  }

  return StartOffThreadParseTask(cx, std::move(task), options);
}

JS::OffThreadToken* js::StartOffThreadCompileToStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, JS::OffThreadCompileCallback callback,
    void* callbackData) {
  return StartOffThreadCompileToStencilInternal(cx, options, srcBuf, callback,
                                                callbackData);
}

JS::OffThreadToken* js::StartOffThreadCompileToStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    JS::SourceText<Utf8Unit>& srcBuf, JS::OffThreadCompileCallback callback,
    void* callbackData) {
  return StartOffThreadCompileToStencilInternal(cx, options, srcBuf, callback,
                                                callbackData);
}

template <typename Unit>
static JS::OffThreadToken* StartOffThreadCompileModuleToStencilInternal(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    JS::SourceText<Unit>& srcBuf, JS::OffThreadCompileCallback callback,
    void* callbackData) {
  auto task = cx->make_unique<CompileModuleToStencilTask<Unit>>(
      cx, srcBuf, callback, callbackData);
  if (!task) {
    return nullptr;
  }

  return StartOffThreadParseTask(cx, std::move(task), options);
}

JS::OffThreadToken* js::StartOffThreadCompileModuleToStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, JS::OffThreadCompileCallback callback,
    void* callbackData) {
  return StartOffThreadCompileModuleToStencilInternal(cx, options, srcBuf,
                                                      callback, callbackData);
}

JS::OffThreadToken* js::StartOffThreadCompileModuleToStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    JS::SourceText<Utf8Unit>& srcBuf, JS::OffThreadCompileCallback callback,
    void* callbackData) {
  return StartOffThreadCompileModuleToStencilInternal(cx, options, srcBuf,
                                                      callback, callbackData);
}

JS::OffThreadToken* js::StartOffThreadDecodeStencil(
    JSContext* cx, const JS::ReadOnlyDecodeOptions& options,
    const JS::TranscodeRange& range, JS::OffThreadCompileCallback callback,
    void* callbackData) {
  auto task =
      cx->make_unique<DecodeStencilTask>(cx, range, callback, callbackData);
  if (!task) {
    return nullptr;
  }

  JS::CompileOptions compileOptions(cx);
  options.copyTo(compileOptions);

  return StartOffThreadParseTask(cx, std::move(task), compileOptions);
}

bool GlobalHelperThreadState::ensureInitialized() {
  MOZ_ASSERT(CanUseExtraThreads());
  MOZ_ASSERT(this == &HelperThreadState());

  AutoLockHelperThreadState lock;

  if (isInitialized(lock)) {
    return true;
  }

  for (size_t& i : runningTaskCount) {
    i = 0;
  }

  useInternalThreadPool_ = !dispatchTaskCallback;
  if (useInternalThreadPool(lock)) {
    if (!InternalThreadPool::Initialize(threadCount, lock)) {
      return false;
    }
  }

  MOZ_ASSERT(dispatchTaskCallback);

  if (!ensureThreadCount(threadCount, lock)) {
    finishThreads(lock);
    return false;
  }

  MOZ_ASSERT(threadCount != 0);
  isInitialized_ = true;
  return true;
}

bool GlobalHelperThreadState::ensureThreadCount(
    size_t count, AutoLockHelperThreadState& lock) {
  if (!helperTasks_.reserve(count)) {
    return false;
  }

  if (useInternalThreadPool(lock)) {
    InternalThreadPool& pool = InternalThreadPool::Get();
    if (pool.threadCount(lock) < count) {
      if (!pool.ensureThreadCount(count, lock)) {
        return false;
      }

      threadCount = pool.threadCount(lock);
    }
  }

  return true;
}

GlobalHelperThreadState::GlobalHelperThreadState()
    : cpuCount(0),
      threadCount(0),
      totalCountRunningTasks(0),
      registerThread(nullptr),
      unregisterThread(nullptr),
      wasmTier2GeneratorsFinished_(0) {
  MOZ_ASSERT(!gHelperThreadState);

  cpuCount = ClampDefaultCPUCount(GetCPUCount());
  threadCount = ThreadCountForCPUCount(cpuCount);
  gcParallelThreadCount = threadCount;

  MOZ_ASSERT(cpuCount > 0, "GetCPUCount() seems broken");
}

void GlobalHelperThreadState::finish(AutoLockHelperThreadState& lock) {
  if (!isInitialized(lock)) {
    return;
  }

  finishThreads(lock);

  // Make sure there are no Ion free tasks left. We check this here because,
  // unlike the other tasks, we don't explicitly block on this when
  // destroying a runtime.
  auto& freeList = ionFreeList(lock);
  while (!freeList.empty()) {
    UniquePtr<jit::IonFreeTask> task = std::move(freeList.back());
    freeList.popBack();
    jit::FreeIonCompileTask(task->compileTask());
  }
}

void GlobalHelperThreadState::finishThreads(AutoLockHelperThreadState& lock) {
  waitForAllTasksLocked(lock);
  terminating_ = true;

  if (InternalThreadPool::IsInitialized()) {
    InternalThreadPool::ShutDown(lock);
  }
}

#ifdef DEBUG
void GlobalHelperThreadState::assertIsLockedByCurrentThread() const {
  gHelperThreadLock.assertOwnedByCurrentThread();
}
#endif  // DEBUG

void GlobalHelperThreadState::dispatch(
    DispatchReason reason, const AutoLockHelperThreadState& locked) {
  if (canStartTasks(locked) && tasksPending_ < threadCount) {
    // This doesn't guarantee that we don't dispatch more tasks to the external
    // pool than necessary if tasks are taking a long time to start, but it does
    // limit the number.
    tasksPending_++;

    // The hazard analysis can't tell that the callback doesn't GC.
    JS::AutoSuppressGCAnalysis nogc;

    dispatchTaskCallback(reason);
  }
}

void GlobalHelperThreadState::wait(
    AutoLockHelperThreadState& locked,
    TimeDuration timeout /* = TimeDuration::Forever() */) {
  consumerWakeup.wait_for(locked, timeout);
}

void GlobalHelperThreadState::notifyAll(const AutoLockHelperThreadState&) {
  consumerWakeup.notify_all();
}

void GlobalHelperThreadState::notifyOne(const AutoLockHelperThreadState&) {
  consumerWakeup.notify_one();
}

bool GlobalHelperThreadState::hasActiveThreads(
    const AutoLockHelperThreadState& lock) {
  return !helperTasks(lock).empty();
}

void js::WaitForAllHelperThreads() { HelperThreadState().waitForAllTasks(); }

void js::WaitForAllHelperThreads(AutoLockHelperThreadState& lock) {
  HelperThreadState().waitForAllTasksLocked(lock);
}

void GlobalHelperThreadState::waitForAllTasks() {
  AutoLockHelperThreadState lock;
  waitForAllTasksLocked(lock);
}

void GlobalHelperThreadState::waitForAllTasksLocked(
    AutoLockHelperThreadState& lock) {
  CancelOffThreadWasmTier2GeneratorLocked(lock);

  while (canStartTasks(lock) || tasksPending_ || hasActiveThreads(lock)) {
    wait(lock);
  }

  MOZ_ASSERT(gcParallelWorklist().isEmpty(lock));
  MOZ_ASSERT(ionWorklist(lock).empty());
  MOZ_ASSERT(wasmWorklist(lock, wasm::CompileMode::Tier1).empty());
  MOZ_ASSERT(promiseHelperTasks(lock).empty());
  MOZ_ASSERT(parseWorklist(lock).empty());
  MOZ_ASSERT(compressionWorklist(lock).empty());
  MOZ_ASSERT(ionFreeList(lock).empty());
  MOZ_ASSERT(wasmWorklist(lock, wasm::CompileMode::Tier2).empty());
  MOZ_ASSERT(wasmTier2GeneratorWorklist(lock).empty());
  MOZ_ASSERT(!tasksPending_);
  MOZ_ASSERT(!hasActiveThreads(lock));
}

// A task can be a "master" task, ie, it will block waiting for other worker
// threads that perform work on its behalf.  If so it must not take the last
// available thread; there must always be at least one worker thread able to do
// the actual work.  (Or the system may deadlock.)
//
// If a task is a master task it *must* pass isMaster=true here, or perform a
// similar calculation to avoid deadlock from starvation.
//
// isMaster should only be true if the thread calling checkTaskThreadLimit() is
// a helper thread.
//
// NOTE: Calling checkTaskThreadLimit() from a helper thread in the dynamic
// region after currentTask.emplace() and before currentTask.reset() may cause
// it to return a different result than if it is called outside that dynamic
// region, as the predicate inspects the values of the threads' currentTask
// members.

bool GlobalHelperThreadState::checkTaskThreadLimit(
    ThreadType threadType, size_t maxThreads, bool isMaster,
    const AutoLockHelperThreadState& lock) const {
  MOZ_ASSERT(maxThreads > 0);

  if (!isMaster && maxThreads >= threadCount) {
    return true;
  }

  size_t count = runningTaskCount[threadType];
  if (count >= maxThreads) {
    return false;
  }

  MOZ_ASSERT(threadCount >= totalCountRunningTasks);
  size_t idle = threadCount - totalCountRunningTasks;

  // It is possible for the number of idle threads to be zero here, because
  // checkTaskThreadLimit() can be called from non-helper threads.  Notably,
  // the compression task scheduler invokes it, and runs off a helper thread.
  if (idle == 0) {
    return false;
  }

  // A master thread that's the last available thread must not be allowed to
  // run.
  if (isMaster && idle == 1) {
    return false;
  }

  return true;
}

static inline bool IsHelperThreadSimulatingOOM(js::ThreadType threadType) {
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
  return js::oom::simulator.targetThread() == threadType;
#else
  return false;
#endif
}

void GlobalHelperThreadState::addSizeOfIncludingThis(
    JS::GlobalStats* stats, const AutoLockHelperThreadState& lock) const {
#ifdef DEBUG
  assertIsLockedByCurrentThread();
#endif

  mozilla::MallocSizeOf mallocSizeOf = stats->mallocSizeOf_;
  JS::HelperThreadStats& htStats = stats->helperThread;

  htStats.stateData += mallocSizeOf(this);

  if (InternalThreadPool::IsInitialized()) {
    htStats.stateData +=
        InternalThreadPool::Get().sizeOfIncludingThis(mallocSizeOf, lock);
  }

  // Report memory used by various containers
  htStats.stateData +=
      ionWorklist_.sizeOfExcludingThis(mallocSizeOf) +
      ionFinishedList_.sizeOfExcludingThis(mallocSizeOf) +
      ionFreeList_.sizeOfExcludingThis(mallocSizeOf) +
      wasmWorklist_tier1_.sizeOfExcludingThis(mallocSizeOf) +
      wasmWorklist_tier2_.sizeOfExcludingThis(mallocSizeOf) +
      wasmTier2GeneratorWorklist_.sizeOfExcludingThis(mallocSizeOf) +
      promiseHelperTasks_.sizeOfExcludingThis(mallocSizeOf) +
      parseWorklist_.sizeOfExcludingThis(mallocSizeOf) +
      parseFinishedList_.sizeOfExcludingThis(mallocSizeOf) +
      compressionPendingList_.sizeOfExcludingThis(mallocSizeOf) +
      compressionWorklist_.sizeOfExcludingThis(mallocSizeOf) +
      compressionFinishedList_.sizeOfExcludingThis(mallocSizeOf) +
      gcParallelWorklist_.sizeOfExcludingThis(mallocSizeOf, lock) +
      helperTasks_.sizeOfExcludingThis(mallocSizeOf);

  // Report ParseTasks on wait lists
  for (const auto& task : parseWorklist_) {
    htStats.parseTask += task->sizeOfIncludingThis(mallocSizeOf);
  }
  for (auto task : parseFinishedList_) {
    htStats.parseTask += task->sizeOfIncludingThis(mallocSizeOf);
  }

  // Report IonCompileTasks on wait lists
  for (auto task : ionWorklist_) {
    htStats.ionCompileTask += task->sizeOfExcludingThis(mallocSizeOf);
  }
  for (auto task : ionFinishedList_) {
    htStats.ionCompileTask += task->sizeOfExcludingThis(mallocSizeOf);
  }
  for (const auto& task : ionFreeList_) {
    htStats.ionCompileTask +=
        task->compileTask()->sizeOfExcludingThis(mallocSizeOf);
  }

  // Report wasm::CompileTasks on wait lists
  for (auto task : wasmWorklist_tier1_) {
    htStats.wasmCompile += task->sizeOfExcludingThis(mallocSizeOf);
  }
  for (auto task : wasmWorklist_tier2_) {
    htStats.wasmCompile += task->sizeOfExcludingThis(mallocSizeOf);
  }

  // Report number of helper threads.
  MOZ_ASSERT(htStats.idleThreadCount == 0);
  MOZ_ASSERT(threadCount >= totalCountRunningTasks);
  htStats.activeThreadCount = totalCountRunningTasks;
  htStats.idleThreadCount = threadCount - totalCountRunningTasks;
}

size_t GlobalHelperThreadState::maxIonCompilationThreads() const {
  if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_ION)) {
    return 1;
  }
  return threadCount;
}

size_t GlobalHelperThreadState::maxWasmCompilationThreads() const {
  if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_WASM_COMPILE_TIER1) ||
      IsHelperThreadSimulatingOOM(js::THREAD_TYPE_WASM_COMPILE_TIER2)) {
    return 1;
  }
  return std::min(cpuCount, threadCount);
}

size_t GlobalHelperThreadState::maxWasmTier2GeneratorThreads() const {
  return MaxTier2GeneratorTasks;
}

size_t GlobalHelperThreadState::maxPromiseHelperThreads() const {
  if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_WASM_COMPILE_TIER1) ||
      IsHelperThreadSimulatingOOM(js::THREAD_TYPE_WASM_COMPILE_TIER2)) {
    return 1;
  }
  return std::min(cpuCount, threadCount);
}

size_t GlobalHelperThreadState::maxParseThreads() const {
  if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_PARSE)) {
    return 1;
  }
  return std::min(cpuCount, threadCount);
}

size_t GlobalHelperThreadState::maxCompressionThreads() const {
  if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_COMPRESS)) {
    return 1;
  }

  // Compression is triggered on major GCs to compress ScriptSources. It is
  // considered low priority work.
  return 1;
}

size_t GlobalHelperThreadState::maxGCParallelThreads(
    const AutoLockHelperThreadState& lock) const {
  if (IsHelperThreadSimulatingOOM(js::THREAD_TYPE_GCPARALLEL)) {
    return 1;
  }
  return gcParallelThreadCount;
}

HelperThreadTask* GlobalHelperThreadState::maybeGetWasmTier1CompileTask(
    const AutoLockHelperThreadState& lock) {
  return maybeGetWasmCompile(lock, wasm::CompileMode::Tier1);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetWasmTier2CompileTask(
    const AutoLockHelperThreadState& lock) {
  return maybeGetWasmCompile(lock, wasm::CompileMode::Tier2);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetWasmCompile(
    const AutoLockHelperThreadState& lock, wasm::CompileMode mode) {
  if (!canStartWasmCompile(lock, mode)) {
    return nullptr;
  }

  return wasmWorklist(lock, mode).popCopyFront();
}

bool GlobalHelperThreadState::canStartWasmTier1CompileTask(
    const AutoLockHelperThreadState& lock) {
  return canStartWasmCompile(lock, wasm::CompileMode::Tier1);
}

bool GlobalHelperThreadState::canStartWasmTier2CompileTask(
    const AutoLockHelperThreadState& lock) {
  return canStartWasmCompile(lock, wasm::CompileMode::Tier2);
}

bool GlobalHelperThreadState::canStartWasmCompile(
    const AutoLockHelperThreadState& lock, wasm::CompileMode mode) {
  if (wasmWorklist(lock, mode).empty()) {
    return false;
  }

  // Parallel compilation and background compilation should be disabled on
  // unicore systems.

  MOZ_RELEASE_ASSERT(cpuCount > 1);

  // If Tier2 is very backlogged we must give priority to it, since the Tier2
  // queue holds onto Tier1 tasks.  Indeed if Tier2 is backlogged we will
  // devote more resources to Tier2 and not start any Tier1 work at all.

  bool tier2oversubscribed = wasmTier2GeneratorWorklist(lock).length() > 20;

  // For Tier1 and Once compilation, honor the maximum allowed threads to
  // compile wasm jobs at once, to avoid oversaturating the machine.
  //
  // For Tier2 compilation we need to allow other things to happen too, so we
  // do not allow all logical cores to be used for background work; instead we
  // wish to use a fraction of the physical cores.  We can't directly compute
  // the physical cores from the logical cores, but 1/3 of the logical cores
  // is a safe estimate for the number of physical cores available for
  // background work.

  size_t physCoresAvailable = size_t(ceil(cpuCount / 3.0));

  size_t threads;
  ThreadType threadType;
  if (mode == wasm::CompileMode::Tier2) {
    if (tier2oversubscribed) {
      threads = maxWasmCompilationThreads();
    } else {
      threads = physCoresAvailable;
    }
    threadType = THREAD_TYPE_WASM_COMPILE_TIER2;
  } else {
    if (tier2oversubscribed) {
      threads = 0;
    } else {
      threads = maxWasmCompilationThreads();
    }
    threadType = THREAD_TYPE_WASM_COMPILE_TIER1;
  }

  return threads != 0 && checkTaskThreadLimit(threadType, threads, lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetWasmTier2GeneratorTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartWasmTier2GeneratorTask(lock)) {
    return nullptr;
  }

  return wasmTier2GeneratorWorklist(lock).popCopy();
}

bool GlobalHelperThreadState::canStartWasmTier2GeneratorTask(
    const AutoLockHelperThreadState& lock) {
  return !wasmTier2GeneratorWorklist(lock).empty() &&
         checkTaskThreadLimit(THREAD_TYPE_WASM_GENERATOR_TIER2,
                              maxWasmTier2GeneratorThreads(),
                              /*isMaster=*/true, lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetPromiseHelperTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartPromiseHelperTask(lock)) {
    return nullptr;
  }

  return promiseHelperTasks(lock).popCopy();
}

bool GlobalHelperThreadState::canStartPromiseHelperTask(
    const AutoLockHelperThreadState& lock) {
  // PromiseHelperTasks can be wasm compilation tasks that in turn block on
  // wasm compilation so set isMaster = true.
  return !promiseHelperTasks(lock).empty() &&
         checkTaskThreadLimit(THREAD_TYPE_PROMISE_TASK,
                              maxPromiseHelperThreads(),
                              /*isMaster=*/true, lock);
}

static bool IonCompileTaskHasHigherPriority(jit::IonCompileTask* first,
                                            jit::IonCompileTask* second) {
  // Return true if priority(first) > priority(second).
  //
  // This method can return whatever it wants, though it really ought to be a
  // total order. The ordering is allowed to race (change on the fly), however.

  // A higher warm-up counter indicates a higher priority.
  jit::JitScript* firstJitScript = first->script()->jitScript();
  jit::JitScript* secondJitScript = second->script()->jitScript();
  return firstJitScript->warmUpCount() / first->script()->length() >
         secondJitScript->warmUpCount() / second->script()->length();
}

HelperThreadTask* GlobalHelperThreadState::maybeGetIonCompileTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartIonCompileTask(lock)) {
    return nullptr;
  }

  return highestPriorityPendingIonCompile(lock,
                                          /* checkExecutionStatus */ true);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetLowPrioIonCompileTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartIonCompileTask(lock)) {
    return nullptr;
  }

  return highestPriorityPendingIonCompile(lock,
                                          /* checkExecutionStatus */ false);
}

bool GlobalHelperThreadState::canStartIonCompileTask(
    const AutoLockHelperThreadState& lock) {
  return !ionWorklist(lock).empty() &&
         checkTaskThreadLimit(THREAD_TYPE_ION, maxIonCompilationThreads(),
                              lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetIonFreeTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartIonFreeTask(lock)) {
    return nullptr;
  }

  UniquePtr<jit::IonFreeTask> task = std::move(ionFreeList(lock).back());
  ionFreeList(lock).popBack();
  return task.release();
}

bool GlobalHelperThreadState::canStartIonFreeTask(
    const AutoLockHelperThreadState& lock) {
  return !ionFreeList(lock).empty();
}

jit::IonCompileTask* GlobalHelperThreadState::highestPriorityPendingIonCompile(
    const AutoLockHelperThreadState& lock, bool checkExecutionStatus) {
  auto& worklist = ionWorklist(lock);
  MOZ_ASSERT(!worklist.empty());

  // Get the highest priority IonCompileTask which has not started compilation
  // yet.
  size_t index = worklist.length();
  for (size_t i = 0; i < worklist.length(); i++) {
    if (checkExecutionStatus && !worklist[i]->isMainThreadRunningJS()) {
      continue;
    }
    if (i < index ||
        IonCompileTaskHasHigherPriority(worklist[i], worklist[index])) {
      index = i;
    }
  }

  if (index == worklist.length()) {
    return nullptr;
  }
  jit::IonCompileTask* task = worklist[index];
  worklist.erase(&worklist[index]);
  return task;
}

HelperThreadTask* GlobalHelperThreadState::maybeGetParseTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartParseTask(lock)) {
    return nullptr;
  }

  auto& worklist = parseWorklist(lock);
  UniquePtr<ParseTask> task = std::move(worklist.back());
  worklist.popBack();
  return task.release();
}

bool GlobalHelperThreadState::canStartParseTask(
    const AutoLockHelperThreadState& lock) {
  // Parse tasks that end up compiling asm.js in turn may use Wasm compilation
  // threads to generate machine code.  We have no way (at present) to know
  // ahead of time whether a parse task is going to parse asm.js content or not,
  // so we just assume that all parse tasks are master tasks.
  return !parseWorklist(lock).empty() &&
         checkTaskThreadLimit(THREAD_TYPE_PARSE, maxParseThreads(),
                              /*isMaster=*/true, lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetFreeDelazifyTask(
    const AutoLockHelperThreadState& lock) {
  auto& freeList = freeDelazifyTaskVector(lock);
  if (!freeList.empty()) {
    UniquePtr<FreeDelazifyTask> task = std::move(freeList.back());
    freeList.popBack();
    return task.release();
  }
  return nullptr;
}

bool GlobalHelperThreadState::canStartFreeDelazifyTask(
    const AutoLockHelperThreadState& lock) {
  return !freeDelazifyTaskVector(lock).empty() &&
         checkTaskThreadLimit(THREAD_TYPE_DELAZIFY_FREE, maxParseThreads(),
                              /*isMaster=*/true, lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetDelazifyTask(
    const AutoLockHelperThreadState& lock) {
  // NOTE: We want to span all cores availables with delazification tasks, in
  // order to parse a maximum number of functions ahead of their executions.
  // Thus, as opposed to parse task which have a higher priority, we are not
  // exclusively executing these task on parse threads.
  auto& worklist = delazifyWorklist(lock);
  if (worklist.isEmpty()) {
    return nullptr;
  }
  return worklist.popFirst();
}

bool GlobalHelperThreadState::canStartDelazifyTask(
    const AutoLockHelperThreadState& lock) {
  return !delazifyWorklist(lock).isEmpty() &&
         checkTaskThreadLimit(THREAD_TYPE_DELAZIFY, maxParseThreads(),
                              /*isMaster=*/true, lock);
}

HelperThreadTask* GlobalHelperThreadState::maybeGetCompressionTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartCompressionTask(lock)) {
    return nullptr;
  }

  auto& worklist = compressionWorklist(lock);
  UniquePtr<SourceCompressionTask> task = std::move(worklist.back());
  worklist.popBack();
  return task.release();
}

bool GlobalHelperThreadState::canStartCompressionTask(
    const AutoLockHelperThreadState& lock) {
  return !compressionWorklist(lock).empty() &&
         checkTaskThreadLimit(THREAD_TYPE_COMPRESS, maxCompressionThreads(),
                              lock);
}

void GlobalHelperThreadState::startHandlingCompressionTasks(
    ScheduleCompressionTask schedule, JSRuntime* maybeRuntime,
    const AutoLockHelperThreadState& lock) {
  MOZ_ASSERT((schedule == ScheduleCompressionTask::GC) ==
             (maybeRuntime != nullptr));

  auto& pending = compressionPendingList(lock);

  for (size_t i = 0; i < pending.length(); i++) {
    UniquePtr<SourceCompressionTask>& task = pending[i];
    if (schedule == ScheduleCompressionTask::API ||
        (task->runtimeMatches(maybeRuntime) && task->shouldStart())) {
      // OOMing during appending results in the task not being scheduled
      // and deleted.
      (void)submitTask(std::move(task), lock);
      remove(pending, &i);
    }
  }
}

bool GlobalHelperThreadState::submitTask(
    UniquePtr<SourceCompressionTask> task,
    const AutoLockHelperThreadState& locked) {
  if (!compressionWorklist(locked).append(std::move(task))) {
    return false;
  }

  dispatch(DispatchReason::NewTask, locked);
  return true;
}

bool GlobalHelperThreadState::submitTask(
    GCParallelTask* task, const AutoLockHelperThreadState& locked) {
  gcParallelWorklist().insertBack(task, locked);
  dispatch(DispatchReason::NewTask, locked);
  return true;
}

HelperThreadTask* GlobalHelperThreadState::maybeGetGCParallelTask(
    const AutoLockHelperThreadState& lock) {
  if (!canStartGCParallelTask(lock)) {
    return nullptr;
  }

  return gcParallelWorklist().popFirst(lock);
}

bool GlobalHelperThreadState::canStartGCParallelTask(
    const AutoLockHelperThreadState& lock) {
  return !gcParallelWorklist().isEmpty(lock) &&
         checkTaskThreadLimit(THREAD_TYPE_GCPARALLEL,
                              maxGCParallelThreads(lock), lock);
}

ParseTask* GlobalHelperThreadState::removeFinishedParseTask(
    JSContext* cx, JS::OffThreadToken* token) {
  // The token is really a ParseTask* which should be in the finished list.
  auto task = static_cast<ParseTask*>(token);

  // The token was passed in from the browser. Check that the pointer is likely
  // a valid parse task of the expected kind.
  MOZ_RELEASE_ASSERT(task->runtime == cx->runtime());

  // Remove the task from the finished list.
  AutoLockHelperThreadState lock;
  MOZ_ASSERT(parseFinishedList(lock).contains(task));
  task->remove();
  return task;
}

UniquePtr<ParseTask> GlobalHelperThreadState::finishParseTaskCommon(
    JSContext* cx, JS::OffThreadToken* token) {
  MOZ_ASSERT(cx->realm());

  UniquePtr<ParseTask> parseTask(removeFinishedParseTask(cx, token));

  if (!parseTask->fc_.convertToRuntimeError(cx)) {
    return nullptr;
  }

  if (cx->isExceptionPending()) {
    return nullptr;
  }

  return parseTask;
}

already_AddRefed<frontend::CompilationStencil>
GlobalHelperThreadState::finishStencilTask(JSContext* cx,
                                           JS::OffThreadToken* token,
                                           JS::InstantiationStorage* storage) {
  UniquePtr<ParseTask> parseTask(finishParseTaskCommon(cx, token));
  if (!parseTask) {
    return nullptr;
  }

  MOZ_ASSERT(parseTask->stencil_.get());

  if (storage) {
    MOZ_ASSERT(parseTask->options.allocateInstantiationStorage);
    parseTask->moveInstantiationStorageInto(*storage);
  }

  return parseTask->stencil_.forget();
}

void GlobalHelperThreadState::cancelParseTask(JSRuntime* rt,
                                              JS::OffThreadToken* token) {
  AutoLockHelperThreadState lock;
  MOZ_ASSERT(token);

  ParseTask* task = static_cast<ParseTask*>(token);

  GlobalHelperThreadState::ParseTaskVector& worklist =
      HelperThreadState().parseWorklist(lock);
  for (size_t i = 0; i < worklist.length(); i++) {
    if (task == worklist[i]) {
      MOZ_ASSERT(task->runtimeMatches(rt));
      HelperThreadState().remove(worklist, &i);
      return;
    }
  }

  // If task is currently running, wait for it to complete.
  while (true) {
    bool foundTask = false;
    for (auto* helper : HelperThreadState().helperTasks(lock)) {
      if (helper->is<ParseTask>() && helper->as<ParseTask>() == task) {
        MOZ_ASSERT(helper->as<ParseTask>()->runtimeMatches(rt));
        foundTask = true;
        break;
      }
    }

    if (!foundTask) {
      break;
    }

    HelperThreadState().wait(lock);
  }

  auto& finished = HelperThreadState().parseFinishedList(lock);
  for (auto* t : finished) {
    if (task == t) {
      MOZ_ASSERT(task->runtimeMatches(rt));
      task->remove();
      HelperThreadState().destroyParseTask(rt, task);
      return;
    }
  }
}

void GlobalHelperThreadState::destroyParseTask(JSRuntime* rt,
                                               ParseTask* parseTask) {
  MOZ_ASSERT(!parseTask->isInList());
  js_delete(parseTask);
}

bool js::EnqueueOffThreadCompression(JSContext* cx,
                                     UniquePtr<SourceCompressionTask> task) {
  AutoLockHelperThreadState lock;

  auto& pending = HelperThreadState().compressionPendingList(lock);
  if (!pending.append(std::move(task))) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

void js::StartHandlingCompressionsOnGC(JSRuntime* runtime) {
  AutoLockHelperThreadState lock;
  HelperThreadState().startHandlingCompressionTasks(
      GlobalHelperThreadState::ScheduleCompressionTask::GC, runtime, lock);
}

template <typename T>
static void ClearCompressionTaskList(T& list, JSRuntime* runtime) {
  for (size_t i = 0; i < list.length(); i++) {
    if (list[i]->runtimeMatches(runtime)) {
      HelperThreadState().remove(list, &i);
    }
  }
}

void js::CancelOffThreadCompressions(JSRuntime* runtime) {
  if (!CanUseExtraThreads()) {
    return;
  }

  AutoLockHelperThreadState lock;

  // Cancel all pending compression tasks.
  ClearCompressionTaskList(HelperThreadState().compressionPendingList(lock),
                           runtime);
  ClearCompressionTaskList(HelperThreadState().compressionWorklist(lock),
                           runtime);

  // Cancel all in-process compression tasks and wait for them to join so we
  // clean up the finished tasks.
  while (true) {
    bool inProgress = false;
    for (auto* helper : HelperThreadState().helperTasks(lock)) {
      if (!helper->is<SourceCompressionTask>()) {
        continue;
      }

      if (helper->as<SourceCompressionTask>()->runtimeMatches(runtime)) {
        inProgress = true;
      }
    }

    if (!inProgress) {
      break;
    }

    HelperThreadState().wait(lock);
  }

  // Clean up finished tasks.
  ClearCompressionTaskList(HelperThreadState().compressionFinishedList(lock),
                           runtime);
}

void js::AttachFinishedCompressions(JSRuntime* runtime,
                                    AutoLockHelperThreadState& lock) {
  auto& finished = HelperThreadState().compressionFinishedList(lock);
  for (size_t i = 0; i < finished.length(); i++) {
    if (finished[i]->runtimeMatches(runtime)) {
      UniquePtr<SourceCompressionTask> compressionTask(std::move(finished[i]));
      HelperThreadState().remove(finished, &i);
      compressionTask->complete();
    }
  }
}

void js::SweepPendingCompressions(AutoLockHelperThreadState& lock) {
  auto& pending = HelperThreadState().compressionPendingList(lock);
  for (size_t i = 0; i < pending.length(); i++) {
    if (pending[i]->shouldCancel()) {
      HelperThreadState().remove(pending, &i);
    }
  }
}

void js::RunPendingSourceCompressions(JSRuntime* runtime) {
  if (!CanUseExtraThreads()) {
    return;
  }

  AutoLockHelperThreadState lock;

  HelperThreadState().startHandlingCompressionTasks(
      GlobalHelperThreadState::ScheduleCompressionTask::API, nullptr, lock);

  // Wait until all tasks have started compression.
  while (!HelperThreadState().compressionWorklist(lock).empty()) {
    HelperThreadState().wait(lock);
  }

  // Wait for all in-process compression tasks to complete.
  HelperThreadState().waitForAllTasksLocked(lock);

  AttachFinishedCompressions(runtime, lock);
}

void PromiseHelperTask::executeAndResolveAndDestroy(JSContext* cx) {
  execute();
  run(cx, JS::Dispatchable::NotShuttingDown);
}

void PromiseHelperTask::runHelperThreadTask(AutoLockHelperThreadState& lock) {
  {
    AutoUnlockHelperThreadState unlock(lock);
    execute();
  }

  // Don't release the lock between dispatching the resolve and destroy
  // operation (which may start immediately on another thread) and returning
  // from this method.

  dispatchResolveAndDestroy(lock);
}

bool js::StartOffThreadPromiseHelperTask(JSContext* cx,
                                         UniquePtr<PromiseHelperTask> task) {
  // Execute synchronously if there are no helper threads.
  if (!CanUseExtraThreads()) {
    task.release()->executeAndResolveAndDestroy(cx);
    return true;
  }

  if (!HelperThreadState().submitTask(task.get())) {
    ReportOutOfMemory(cx);
    return false;
  }

  (void)task.release();
  return true;
}

bool js::StartOffThreadPromiseHelperTask(PromiseHelperTask* task) {
  MOZ_ASSERT(CanUseExtraThreads());

  return HelperThreadState().submitTask(task);
}

bool GlobalHelperThreadState::submitTask(PromiseHelperTask* task) {
  AutoLockHelperThreadState lock;

  if (!promiseHelperTasks(lock).append(task)) {
    return false;
  }

  dispatch(DispatchReason::NewTask, lock);
  return true;
}

void GlobalHelperThreadState::trace(JSTracer* trc) {
  AutoLockHelperThreadState lock;

#ifdef DEBUG
  // Since we hold the helper thread lock here we must disable GCMarker's
  // checking of the atom marking bitmap since that also relies on taking the
  // lock.
  GCMarker* marker = nullptr;
  if (trc->isMarkingTracer()) {
    marker = GCMarker::fromTracer(trc);
    marker->setCheckAtomMarking(false);
  }
  auto reenableAtomMarkingCheck = mozilla::MakeScopeExit([marker] {
    if (marker) {
      marker->setCheckAtomMarking(true);
    }
  });
#endif

  for (auto task : ionWorklist(lock)) {
    task->alloc().lifoAlloc()->setReadWrite();
    task->trace(trc);
    task->alloc().lifoAlloc()->setReadOnly();
  }
  for (auto task : ionFinishedList(lock)) {
    task->trace(trc);
  }

  for (auto* helper : HelperThreadState().helperTasks(lock)) {
    if (helper->is<jit::IonCompileTask>()) {
      helper->as<jit::IonCompileTask>()->trace(trc);
    }
  }

  JSRuntime* rt = trc->runtime();
  if (auto* jitRuntime = rt->jitRuntime()) {
    jit::IonCompileTask* task = jitRuntime->ionLazyLinkList(rt).getFirst();
    while (task) {
      task->trace(trc);
      task = task->getNext();
    }
  }
}

// Definition of helper thread tasks.
//
// Priority is determined by the order they're listed here.
const GlobalHelperThreadState::Selector GlobalHelperThreadState::selectors[] = {
    &GlobalHelperThreadState::maybeGetGCParallelTask,
    &GlobalHelperThreadState::maybeGetIonCompileTask,
    &GlobalHelperThreadState::maybeGetWasmTier1CompileTask,
    &GlobalHelperThreadState::maybeGetPromiseHelperTask,
    &GlobalHelperThreadState::maybeGetParseTask,
    &GlobalHelperThreadState::maybeGetFreeDelazifyTask,
    &GlobalHelperThreadState::maybeGetDelazifyTask,
    &GlobalHelperThreadState::maybeGetCompressionTask,
    &GlobalHelperThreadState::maybeGetLowPrioIonCompileTask,
    &GlobalHelperThreadState::maybeGetIonFreeTask,
    &GlobalHelperThreadState::maybeGetWasmTier2CompileTask,
    &GlobalHelperThreadState::maybeGetWasmTier2GeneratorTask};

bool GlobalHelperThreadState::canStartTasks(
    const AutoLockHelperThreadState& lock) {
  return canStartGCParallelTask(lock) || canStartIonCompileTask(lock) ||
         canStartWasmTier1CompileTask(lock) ||
         canStartPromiseHelperTask(lock) || canStartParseTask(lock) ||
         canStartFreeDelazifyTask(lock) || canStartDelazifyTask(lock) ||
         canStartCompressionTask(lock) || canStartIonFreeTask(lock) ||
         canStartWasmTier2CompileTask(lock) ||
         canStartWasmTier2GeneratorTask(lock);
}

void JS::RunHelperThreadTask() {
  MOZ_ASSERT(CanUseExtraThreads());

  AutoLockHelperThreadState lock;

  if (!gHelperThreadState || HelperThreadState().isTerminating(lock)) {
    return;
  }

  HelperThreadState().runOneTask(lock);
}

void GlobalHelperThreadState::runOneTask(AutoLockHelperThreadState& lock) {
  MOZ_ASSERT(tasksPending_ > 0);
  tasksPending_--;

  // The selectors may depend on the HelperThreadState not changing between task
  // selection and task execution, in particular, on new tasks not being added
  // (because of the lifo structure of the work lists). Unlocking the
  // HelperThreadState between task selection and execution is not well-defined.
  HelperThreadTask* task = findHighestPriorityTask(lock);
  if (task) {
    runTaskLocked(task, lock);
    dispatch(DispatchReason::FinishedTask, lock);
  }

  notifyAll(lock);
}

HelperThreadTask* GlobalHelperThreadState::findHighestPriorityTask(
    const AutoLockHelperThreadState& locked) {
  // Return the highest priority task that is ready to start, or nullptr.

  for (const auto& selector : selectors) {
    if (auto* task = (this->*(selector))(locked)) {
      return task;
    }
  }

  return nullptr;
}

void GlobalHelperThreadState::runTaskLocked(HelperThreadTask* task,
                                            AutoLockHelperThreadState& locked) {
  JS::AutoSuppressGCAnalysis nogc;

  HelperThreadState().helperTasks(locked).infallibleEmplaceBack(task);

  ThreadType threadType = task->threadType();
  js::oom::SetThreadType(threadType);
  runningTaskCount[threadType]++;
  totalCountRunningTasks++;

  task->runHelperThreadTask(locked);

  // Delete task from helperTasks.
  HelperThreadState().helperTasks(locked).eraseIfEqual(task);

  totalCountRunningTasks--;
  runningTaskCount[threadType]--;

  js::oom::SetThreadType(js::THREAD_TYPE_NONE);
}
