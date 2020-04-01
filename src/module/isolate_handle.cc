#include "isolate_handle.h"
#include "context_handle.h"
#include "external_copy_handle.h"
#include "script_handle.h"
#include "module_handle.h"
#include "session_handle.h"
#include "external_copy/external_copy.h"
#include "lib/lockable.h"
#include "isolate/allocator.h"
#include "isolate/functor_runners.h"
#include "isolate/platform_delegate.h"
#include "isolate/remote_handle.h"
#include "isolate/three_phase_task.h"
#include "isolate/v8_version.h"
#include "module/evaluation.h"
#include "v8-platform.h"
#include <deque>
#include <memory>

using namespace v8;
using std::shared_ptr;
using std::unique_ptr;

namespace ivm {

/**
 * IsolateHandle implementation
 */
IsolateHandle::IsolateHandleTransferable::IsolateHandleTransferable(shared_ptr<IsolateHolder> isolate) : isolate(std::move(isolate)) {}

Local<Value> IsolateHandle::IsolateHandleTransferable::TransferIn() {
	return ClassHandle::NewInstance<IsolateHandle>(isolate);
}

IsolateHandle::IsolateHandle(shared_ptr<IsolateHolder> isolate) : isolate(std::move(isolate)) {}

Local<FunctionTemplate> IsolateHandle::Definition() {
	return Inherit<TransferableHandle>(MakeClass(
	 "Isolate", ConstructorFunction<decltype(&New), &New>{},
		"createSnapshot", FreeFunction<decltype(&CreateSnapshot), &CreateSnapshot>{},
		"compileScript", MemberFunction<decltype(&IsolateHandle::CompileScript<1>), &IsolateHandle::CompileScript<1>>{},
		"compileScriptSync", MemberFunction<decltype(&IsolateHandle::CompileScript<0>), &IsolateHandle::CompileScript<0>>{},
		"compileModule", MemberFunction<decltype(&IsolateHandle::CompileModule<1>), &IsolateHandle::CompileModule<1>>{},
		"compileModuleSync", MemberFunction<decltype(&IsolateHandle::CompileModule<0>), &IsolateHandle::CompileModule<0>>{},
		"cpuTime", MemberAccessor<decltype(&IsolateHandle::GetCpuTime), &IsolateHandle::GetCpuTime>{},
		"createContext", MemberFunction<decltype(&IsolateHandle::CreateContext<1>), &IsolateHandle::CreateContext<1>>{},
		"createContextSync", MemberFunction<decltype(&IsolateHandle::CreateContext<0>), &IsolateHandle::CreateContext<0>>{},
		"createInspectorSession", MemberFunction<decltype(&IsolateHandle::CreateInspectorSession), &IsolateHandle::CreateInspectorSession>{},
		"dispose", MemberFunction<decltype(&IsolateHandle::Dispose), &IsolateHandle::Dispose>{},
		"getHeapStatistics", MemberFunction<decltype(&IsolateHandle::GetHeapStatistics<1>), &IsolateHandle::GetHeapStatistics<1>>{},
		"getHeapStatisticsSync", MemberFunction<decltype(&IsolateHandle::GetHeapStatistics<0>), &IsolateHandle::GetHeapStatistics<0>>{},
		"isDisposed", MemberAccessor<decltype(&IsolateHandle::IsDisposedGetter), &IsolateHandle::IsDisposedGetter>{},
		"referenceCount", MemberAccessor<decltype(&IsolateHandle::GetReferenceCount), &IsolateHandle::GetReferenceCount>{},
		"wallTime", MemberAccessor<decltype(&IsolateHandle::GetWallTime), &IsolateHandle::GetWallTime>{}
	));
}

/**
 * Create a new Isolate. It all starts here!
 */
unique_ptr<ClassHandle> IsolateHandle::New(MaybeLocal<Object> maybe_options) {
	shared_ptr<void> snapshot_blob;
	size_t snapshot_blob_length = 0;
	size_t memory_limit = 128;
	bool inspector = false;

	// Parse options
	Local<Object> options;
	if (maybe_options.ToLocal(&options)) {

		// Check memory limits
		memory_limit = ReadOption<double>(options, "memoryLimit", 128);
		if (memory_limit < 8) {
			throw RuntimeGenericError("`memoryLimit` must be at least 8");
		}

		// Set snapshot
		auto maybe_snapshot = ReadOption<MaybeLocal<Object>>(options, StringTable::Get().snapshot, {});
		Local<Object> snapshot_handle;
		if (maybe_snapshot.ToLocal(&snapshot_handle)) {
			auto copy_handle = ClassHandle::Unwrap<ExternalCopyHandle>(snapshot_handle.As<Object>());
			if (copy_handle != nullptr) {
				ExternalCopyArrayBuffer* copy_ptr = dynamic_cast<ExternalCopyArrayBuffer*>(copy_handle->GetValue().get());
				if (copy_ptr != nullptr) {
					snapshot_blob = copy_ptr->Acquire();
					snapshot_blob_length = copy_ptr->Length();
				}
			}
			if (!snapshot_blob) {
				throw RuntimeTypeError("`snapshot` must be an ExternalCopy to ArrayBuffer");
			}
		}

		// Check inspector flag
		inspector = ReadOption<bool>(options, StringTable::Get().inspector, false);
	}

	// Return isolate handle
	auto isolate = IsolateEnvironment::New(memory_limit, std::move(snapshot_blob), snapshot_blob_length);
	if (inspector) {
		isolate->GetIsolate()->EnableInspectorAgent();
	}
	return std::make_unique<IsolateHandle>(isolate);
}

unique_ptr<Transferable> IsolateHandle::TransferOut() {
	return std::make_unique<IsolateHandleTransferable>(isolate);
}

/**
 * Create a new v8::Context in this isolate and returns a ContextHandle
 */
struct CreateContextRunner : public ThreePhaseTask {
	bool enable_inspector = false;
	RemoteHandle<Context> context;
	RemoteHandle<Value> global;

	explicit CreateContextRunner(MaybeLocal<Object>& maybe_options) {
		enable_inspector = ReadOption<bool>(maybe_options, StringTable::Get().inspector, false);
	}

	void Phase2() final {
		// Use custom deleter on the shared_ptr which will notify the isolate when the context is gone
		struct ContextDeleter {
			bool has_inspector;

			explicit ContextDeleter(bool has_inspector) : has_inspector{has_inspector} {}

			void operator() (Persistent<Context>& context) {
				auto& env = *IsolateEnvironment::GetCurrent();
				{
					HandleScope handle_scope{env.GetIsolate()};
					auto context_local = Local<Context>::New(env.GetIsolate(), context);
					context_local->DetachGlobal();
					if (has_inspector) {
						env.GetInspectorAgent()->ContextDestroyed(context_local);
					}
					context.Reset();
				}
				env.GetIsolate()->ContextDisposedNotification();
			}
		};

		auto env = IsolateEnvironment::GetCurrent();

		// Sanity check before we build the context
		if (enable_inspector && env->GetInspectorAgent() == nullptr) {
			Context::Scope context_scope(env->DefaultContext()); // TODO: This is needed to throw, but is stupid and sloppy
			throw RuntimeGenericError("Inspector is not enabled for this isolate");
		}

		// Make a new context and setup shared pointers
		IsolateEnvironment::HeapCheck heap_check{*env, true};
		Local<Context> context_handle = env->NewContext();
		if (enable_inspector) {
			env->GetInspectorAgent()->ContextCreated(context_handle, "<isolated-vm>");
		}
		context = RemoteHandle<Context>{context_handle, ContextDeleter{enable_inspector}};
		global = RemoteHandle<Value>{context_handle->Global()};
		heap_check.Epilogue();
	}

	Local<Value> Phase3() final {
		// Make a new Context{} JS class
		return ClassHandle::NewInstance<ContextHandle>(std::move(context), std::move(global));
	}
};
template <int async>
Local<Value> IsolateHandle::CreateContext(MaybeLocal<Object> maybe_options) {
	return ThreePhaseTask::Run<async, CreateContextRunner>(*isolate, maybe_options);
}

/**
 * Compiles a script in this isolate and returns a ScriptHandle
 */
struct CompileScriptRunner : public CodeCompilerHolder, public ThreePhaseTask {
	RemoteHandle<UnboundScript> script;

	CompileScriptRunner(const Local<String>& code_handle, const MaybeLocal<Object>& maybe_options) :
		CodeCompilerHolder{code_handle, maybe_options, false} {}

	void Phase2() final {
		// Compile in second isolate and return UnboundScript persistent
		auto isolate = IsolateEnvironment::GetCurrent();
		Context::Scope context_scope(isolate->DefaultContext());
		IsolateEnvironment::HeapCheck heap_check{*isolate, true};
		auto source = GetSource();
		ScriptCompiler::CompileOptions compile_options = ScriptCompiler::kNoCompileOptions;
		if (DidSupplyCachedData()) {
			compile_options = ScriptCompiler::kConsumeCodeCache;
		}
		script = RemoteHandle<UnboundScript>{RunWithAnnotatedErrors(
			[&isolate, &source, compile_options]() { return Unmaybe(ScriptCompiler::CompileUnboundScript(*isolate, source.get(), compile_options)); }
		)};

		// Check cached data flags
		if (DidSupplyCachedData()) {
			SetCachedDataRejected(source->GetCachedData()->rejected);
		}
		if (ShouldProduceCachedData()) {
			ScriptCompiler::CachedData* cached_data // continued next line
#if V8_AT_LEAST(6, 8, 11)
			// `code` parameter removed in v8 commit a440efb27
			= ScriptCompiler::CreateCodeCache(script.Deref());
#else
			// Added in v8 commit dae20b064
			= ScriptCompiler::CreateCodeCache(script.Deref(), GetSourceString());
#endif
			assert(cached_data != nullptr);
			SaveCachedData(cached_data);
		}
		ResetSource();
		heap_check.Epilogue();
	}

	Local<Value> Phase3() final {
		// Wrap UnboundScript in JS Script{} class
		Local<Object> value = ClassHandle::NewInstance<ScriptHandle>(std::move(script));
		WriteCompileResults(value);
		return value;
	}
};

template <int async>
Local<Value> IsolateHandle::CompileScript(Local<String> code_handle, MaybeLocal<Object> maybe_options) {
	return ThreePhaseTask::Run<async, CompileScriptRunner>(*this->isolate, code_handle, maybe_options);
}

/**
* Compiles a module in this isolate and returns a ModuleHandle
*/
struct CompileModuleRunner : public CodeCompilerHolder, public ThreePhaseTask {
	shared_ptr<ModuleInfo> module_info;

	CompileModuleRunner(const Local<String>& code_handle, const MaybeLocal<Object>& maybe_options) :
		CodeCompilerHolder{code_handle, maybe_options, true} {}

	void Phase2() final {
		auto isolate = IsolateEnvironment::GetCurrent();
		Context::Scope context_scope(isolate->DefaultContext());
		IsolateEnvironment::HeapCheck heap_check{*isolate, true};
		auto source = GetSource();
		Local<Module> module_handle = Unmaybe(ScriptCompiler::CompileModule(*isolate, source.get()));

#if V8_AT_LEAST(6, 9, 37)
		// v8 6.8.214 [8ec92f51] adds support for producing cached data for modules, but support for
		// actually consuming the cached data wasn't added until 6.9.37 [70b5fd3b].
		if (DidSupplyCachedData()) {
			SetCachedDataRejected(source->GetCachedData()->rejected);
		}
		if (ShouldProduceCachedData()) {
			ScriptCompiler::CachedData* cached_data = ScriptCompiler::CreateCodeCache(module_handle->GetUnboundModuleScript());
			assert(cached_data != nullptr);
			SaveCachedData(cached_data);
		}
#else
		if (DidSupplyCachedData()) {
			SetCachedDataRejected(true);
		}
#endif

		ResetSource();
		module_info = std::make_shared<ModuleInfo>(module_handle);
		heap_check.Epilogue();
	}

	Local<Value> Phase3() final {
		Local<Object> value = ClassHandle::NewInstance<ModuleHandle>(std::move(module_info));
		WriteCompileResults(value);
		return value;
	}
};

template <int async>
Local<Value> IsolateHandle::CompileModule(Local<String> code_handle, MaybeLocal<Object> maybe_options) {
	return ThreePhaseTask::Run<async, CompileModuleRunner>(*this->isolate, code_handle, maybe_options);
}

/**
 * Create a new channel for debugging on the inspector
 */
Local<Value> IsolateHandle::CreateInspectorSession() {
	if (IsolateEnvironment::GetCurrentHolder() == isolate) {
		throw RuntimeGenericError("An isolate is not debuggable from within itself");
	}
	shared_ptr<IsolateEnvironment> env = isolate->GetIsolate();
	if (!env) {
		throw RuntimeGenericError("Isolate is diposed");
	}
	if (env->GetInspectorAgent() == nullptr) {
		throw RuntimeGenericError("Inspector is not enabled for this isolate");
	}
	return ClassHandle::NewInstance<SessionHandle>(*env);
}

/**
 * Dispose an isolate
 */
Local<Value> IsolateHandle::Dispose() {
	if (!isolate->Dispose()) {
		throw RuntimeGenericError("Isolate is already disposed");
	}
	return Undefined(Isolate::GetCurrent());
}

/**
 * Get heap statistics from v8
 */
struct HeapStatRunner : public ThreePhaseTask {
	HeapStatistics heap;
	size_t externally_allocated_size = 0;
	size_t adjustment = 0;

	// Dummy constructor to workaround gcc bug
	explicit HeapStatRunner(int /* unused */) {}

	void Phase2() final {
		IsolateEnvironment& isolate = *IsolateEnvironment::GetCurrent();
		isolate->GetHeapStatistics(&heap);
		adjustment = heap.heap_size_limit() - isolate.GetInitialHeapSizeLimit();
		externally_allocated_size = isolate.GetExtraAllocatedMemory();
	}

	Local<Value> Phase3() final {
		Isolate* isolate = Isolate::GetCurrent();
		Local<Context> context = isolate->GetCurrentContext();
		Local<Object> ret = Object::New(isolate);
		auto strings = StringTable::Get();
		Unmaybe(ret->Set(context, strings.total_heap_size, Number::New(isolate, heap.total_heap_size())));
		Unmaybe(ret->Set(context, strings.total_heap_size_executable, Number::New(isolate, heap.total_heap_size_executable())));
		Unmaybe(ret->Set(context, strings.total_physical_size, Number::New(isolate, heap.total_physical_size())));
		Unmaybe(ret->Set(context, strings.total_available_size, Number::New(isolate, static_cast<double>(heap.total_available_size()) - adjustment)));
		Unmaybe(ret->Set(context, strings.used_heap_size, Number::New(isolate, heap.used_heap_size())));
		Unmaybe(ret->Set(context, strings.heap_size_limit, Number::New(isolate, static_cast<double>(heap.heap_size_limit()) - adjustment)));
		Unmaybe(ret->Set(context, strings.malloced_memory, Number::New(isolate, heap.malloced_memory())));
		Unmaybe(ret->Set(context, strings.peak_malloced_memory, Number::New(isolate, heap.peak_malloced_memory())));
		Unmaybe(ret->Set(context, strings.does_zap_garbage, Number::New(isolate, heap.does_zap_garbage())));
		Unmaybe(ret->Set(context, strings.externally_allocated_size, Number::New(isolate, externally_allocated_size)));
		return ret;
	}
};
template <int async>
Local<Value> IsolateHandle::GetHeapStatistics() {
	return ThreePhaseTask::Run<async, HeapStatRunner>(*isolate, 0);
}

/**
 * Timers
 */
Local<Value> IsolateHandle::GetCpuTime() {
	auto env = this->isolate->GetIsolate();
	if (!env) {
		throw RuntimeGenericError("Isolate is disposed");
	}
	uint64_t time = env->GetCpuTime().count();
	Isolate* isolate = Isolate::GetCurrent();
	Local<Context> context = isolate->GetCurrentContext();
	constexpr auto kNanos = (uint64_t)1e9;
	Local<Array> ret = Array::New(isolate, 2);
	Unmaybe(ret->Set(context, 0, Uint32::New(isolate, (uint32_t)(time / kNanos))));
	Unmaybe(ret->Set(context, 1, Uint32::New(isolate, (uint32_t)(time - (time / kNanos) * kNanos))));
	return ret;
}

Local<Value> IsolateHandle::GetWallTime() {
	auto env = this->isolate->GetIsolate();
	if (!env) {
		throw RuntimeGenericError("Isolate is disposed");
	}
	uint64_t time = env->GetWallTime().count();
	Isolate* isolate = Isolate::GetCurrent();
	Local<Context> context = isolate->GetCurrentContext();
	constexpr auto kNanos = (uint64_t)1e9;
	Local<Array> ret = Array::New(isolate, 2);
	Unmaybe(ret->Set(context, 0, Uint32::New(isolate, (uint32_t)(time / kNanos))));
	Unmaybe(ret->Set(context, 1, Uint32::New(isolate, (uint32_t)(time - (time / kNanos) * kNanos))));
	return ret;
}

/**
 * Reference count
 */
Local<Value> IsolateHandle::GetReferenceCount() {
	auto env = this->isolate->GetIsolate();
	if (!env) {
		throw RuntimeGenericError("Isolate is disposed");
	}
	return Number::New(Isolate::GetCurrent(), env->GetRemotesCount());
}

/**
 * Simple disposal checker
 */
Local<Value> IsolateHandle::IsDisposedGetter() {
	return Boolean::New(Isolate::GetCurrent(), !isolate->GetIsolate());
}

/**
* Create a snapshot from some code and return it as an external ArrayBuffer
*/
static StartupData SerializeInternalFieldsCallback(Local<Object> /*holder*/, int /*index*/, void* /*data*/) {
	return {nullptr, 0};
}

Local<Value> IsolateHandle::CreateSnapshot(ArrayRange script_handles, MaybeLocal<String> warmup_handle) {

	// Simple platform delegate and task queue
	using TaskDeque = lockable_t<std::deque<std::unique_ptr<v8::Task>>>;
	class SnapshotPlatformDelegate :
			public IsolatePlatformDelegate, public TaskRunner,
			public std::enable_shared_from_this<SnapshotPlatformDelegate> {

		public:
			explicit SnapshotPlatformDelegate(TaskDeque& tasks) : tasks{tasks} {}

			// v8 will continually post delayed tasks so we cut it off when work is done
			void DoneWithWork() {
				done = true;
			}

			// Methods for IsolatePlatformDelegate
			auto GetForegroundTaskRunner() -> std::shared_ptr<v8::TaskRunner> final {
			 return shared_from_this();
			}
			bool IdleTasksEnabled() final {
				return false;
			}

			// Methods for v8::TaskRunner
			void PostTask(std::unique_ptr<v8::Task> task) final {
				tasks.write()->push_back(std::move(task));
			}
			void PostDelayedTask(std::unique_ptr<v8::Task> task, double /*delay_in_seconds*/) final {
				if (!done) {
					PostTask(std::move(task));
				}
			}

		private:
			lockable_t<std::deque<std::unique_ptr<v8::Task>>>& tasks;
			bool done = false;
	};

	TaskDeque tasks;
	auto delegate = std::make_shared<SnapshotPlatformDelegate>(tasks);

	// Copy embed scripts and warmup script from outer isolate
	std::vector<std::pair<ExternalCopyString, ScriptOriginHolder>> scripts;
	Isolate* isolate = Isolate::GetCurrent();
	Local<Context> context = isolate->GetCurrentContext();
	scripts.reserve(std::distance(script_handles.begin(), script_handles.end()));
	for (auto value : script_handles) {
		auto script_handle = HandleCast<Local<Object>>(value);
		Local<Value> script = Unmaybe(script_handle.As<Object>()->Get(context, StringTable::Get().code));
		if (!script->IsString()) {
			throw RuntimeTypeError("`code` property is required");
		}
		scripts.emplace_back(ExternalCopyString{script.As<String>()}, ScriptOriginHolder{script_handle});
	}
	ExternalCopyString warmup_script;
	if (!warmup_handle.IsEmpty()) {
		warmup_script = ExternalCopyString{warmup_handle.ToLocalChecked().As<String>()};
	}

	// Create the snapshot
	StartupData snapshot {};
	unique_ptr<const char> snapshot_data_ptr;
	shared_ptr<ExternalCopy> error;
	{
		Isolate* isolate;
#if V8_AT_LEAST(6, 8, 57)
		isolate = Isolate::Allocate();
		PlatformDelegate::RegisterIsolate(isolate, delegate.get());
		SnapshotCreator snapshot_creator{isolate};
#else
		SnapshotCreator snapshot_creator;
		isolate = snapshot_creator.GetIsolate();
		PlatformDelegate::RegisterIsolate(isolate, delegate.get());
#endif
		{
			Locker locker(isolate);
			Isolate::Scope isolate_scope(isolate);
			HandleScope handle_scope(isolate);
			Local<Context> context = Context::New(isolate);
			snapshot_creator.SetDefaultContext(context, {&SerializeInternalFieldsCallback, nullptr});
			FunctorRunners::RunCatchExternal(context, [&]() {
				HandleScope handle_scope(isolate);
				Local<Context> context_dirty = Context::New(isolate);
				for (auto& script : scripts) {
					Local<String> code = script.first.CopyInto().As<String>();
					ScriptOrigin script_origin = ScriptOrigin{script.second};
					ScriptCompiler::Source source{code, script_origin};
					Local<UnboundScript> unbound_script;
					{
						Context::Scope context_scope{context};
						Local<Script> compiled_script = RunWithAnnotatedErrors(
							[&context, &source]() {
								const auto compiled_script = Unmaybe(ScriptCompiler::Compile(context, &source, ScriptCompiler::kNoCompileOptions));
								Unmaybe(compiled_script->Run(context));
								return compiled_script;
							}
						);			
						unbound_script = compiled_script->GetUnboundScript();
					}
					if (warmup_script) {
						Context::Scope context_scope{context_dirty};
						Unmaybe(unbound_script->BindToCurrentContext()->Run(context_dirty));
					}
				}
				if (warmup_script) {
					Context::Scope context_scope{context_dirty};
					MaybeLocal<Object> tmp;
					ScriptOriginHolder script_origin{tmp};
					ScriptCompiler::Source source{warmup_script.CopyInto().As<String>(), ScriptOrigin{script_origin}};
					RunWithAnnotatedErrors([&context_dirty, &source]() {
						Unmaybe(Unmaybe(ScriptCompiler::Compile(context_dirty, &source, ScriptCompiler::kNoCompileOptions))->Run(context_dirty));
					});
				}
			}, [ &error ](unique_ptr<ExternalCopy> error_inner) {
				error = std::move(error_inner);
			});
			isolate->ContextDisposedNotification(false);

			// Run all queued tasks
			delegate->DoneWithWork();
			while (true) {
				auto task = [&]() -> std::unique_ptr<v8::Task> {
					auto lock = tasks.write();
					if (lock->empty()) {
						return nullptr;
					}
					auto task = std::move(lock->front());
					lock->pop_front();
					return task;
				}();
				if (task) {
					task->Run();
				} else {
					break;
				}
			}
		}
		// nb: Snapshot must be created even in the error case, because `~SnapshotCreator` will crash if
		// you don't
		snapshot = snapshot_creator.CreateBlob(SnapshotCreator::FunctionCodeHandling::kKeep);
		snapshot_data_ptr.reset(snapshot.data);
		PlatformDelegate::UnregisterIsolate(isolate);
	}

	// Export to outer scope
	if (error) {
		Isolate::GetCurrent()->ThrowException(error->CopyInto());
		return Undefined(Isolate::GetCurrent());
	} else if (snapshot.raw_size == 0) {
		throw RuntimeGenericError("Failure creating snapshot");
	}
	auto buffer = std::make_shared<ExternalCopyArrayBuffer>((void*)snapshot.data, snapshot.raw_size);
	return ClassHandle::NewInstance<ExternalCopyHandle>(buffer);
}

} // namespace ivm
