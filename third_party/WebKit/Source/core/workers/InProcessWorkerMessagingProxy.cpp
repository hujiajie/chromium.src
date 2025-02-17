/*
 * Copyright (C) 2008 Apple Inc. All Rights Reserved.
 * Copyright (C) 2009 Google Inc. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "core/workers/InProcessWorkerMessagingProxy.h"

#include <memory>
#include "third_party/node-nw/src/node_webkit.h"
#define BLINK_HOOK_MAP(type, sym, fn) BLINK_EXPORT type fn = nullptr;
#if defined(COMPONENT_BUILD) && defined(WIN32)
#define NW_HOOK_MAP(type, sym, fn) BASE_EXPORT type fn;
#else
#define NW_HOOK_MAP(type, sym, fn) extern type fn;
#endif
#include "content/nw/src/common/node_hooks.h"
#undef NW_HOOK_MAP

#include "base/command_line.h"

#include "core/dom/Document.h"
#include "core/dom/SecurityContext.h"
#include "core/dom/TaskRunnerHelper.h"
#include "core/events/ErrorEvent.h"
#include "core/events/MessageEvent.h"
#include "core/frame/LocalFrame.h"
#include "core/frame/csp/ContentSecurityPolicy.h"
#include "core/loader/DocumentLoadTiming.h"
#include "core/loader/DocumentLoader.h"
#include "core/origin_trials/OriginTrialContext.h"
#include "core/workers/InProcessWorkerBase.h"
#include "core/workers/InProcessWorkerObjectProxy.h"
#include "core/workers/WorkerClients.h"
#include "core/workers/WorkerGlobalScope.h"
#include "core/workers/WorkerInspectorProxy.h"
#include "core/workers/WorkerThreadStartupData.h"
#include "platform/CrossThreadFunctional.h"
#include "platform/WebTaskRunner.h"
#include "platform/wtf/WTF.h"

namespace blink {

struct InProcessWorkerMessagingProxy::QueuedTask {
  RefPtr<SerializedScriptValue> message;
  MessagePortChannelArray channels;
};

InProcessWorkerMessagingProxy::InProcessWorkerMessagingProxy(
    InProcessWorkerBase* worker_object,
    WorkerClients* worker_clients)
    : InProcessWorkerMessagingProxy(worker_object->GetExecutionContext(),
                                    worker_object,
                                    worker_clients) {
  DCHECK(worker_object_);
}

InProcessWorkerMessagingProxy::InProcessWorkerMessagingProxy(
    ExecutionContext* execution_context,
    InProcessWorkerBase* worker_object,
    WorkerClients* worker_clients)
    : ThreadedMessagingProxyBase(execution_context),
      worker_object_(worker_object),
      worker_clients_(worker_clients),
      weak_ptr_factory_(this) {
  worker_object_proxy_ = InProcessWorkerObjectProxy::Create(
      weak_ptr_factory_.CreateWeakPtr(), GetParentFrameTaskRunners());
}

InProcessWorkerMessagingProxy::~InProcessWorkerMessagingProxy() {
  DCHECK(!worker_object_);
}

void InProcessWorkerMessagingProxy::StartWorkerGlobalScope(
    const KURL& script_url,
    const String& user_agent,
    const String& source_code,
    const String& referrer_policy) {
  DCHECK(IsParentContextThread());
  if (AskedToTerminate()) {
    // Worker.terminate() could be called from JS before the thread was
    // created.
    return;
  }

  Document* document = ToDocument(GetExecutionContext());
  SecurityOrigin* starter_origin = document->GetSecurityOrigin();
  const base::CommandLine& command_line = *base::CommandLine::ForCurrentProcess();

  bool isNodeJS = document->GetFrame() && document->GetFrame()->isNodeJS() && command_line.HasSwitch("enable-node-worker");
  std::string main_script;
  if (g_web_worker_start_thread_fn) {
    (*g_web_worker_start_thread_fn)(document->GetFrame(), (void*)script_url.GetPath().Utf8().data(), &main_script, &isNodeJS);
  }

  ContentSecurityPolicy* csp = document->GetContentSecurityPolicy();
  DCHECK(csp);

  WorkerThreadStartMode start_mode =
      GetWorkerInspectorProxy()->WorkerStartMode(document);
  std::unique_ptr<WorkerSettings> worker_settings =
      WTF::WrapUnique(new WorkerSettings(document->GetSettings()));
  WorkerV8Settings worker_v8_settings(WorkerV8Settings::Default());
  worker_v8_settings.heap_limit_mode_ =
      ToIsolate(document)->IsHeapLimitIncreasedForDebugging()
          ? WorkerV8Settings::HeapLimitMode::kIncreasedForDebugging
          : WorkerV8Settings::HeapLimitMode::kDefault;
  worker_v8_settings.atomics_wait_mode_ =
      IsAtomicsWaitAllowed() ? WorkerV8Settings::AtomicsWaitMode::kAllow
                             : WorkerV8Settings::AtomicsWaitMode::kDisallow;
  std::unique_ptr<WorkerThreadStartupData> startup_data =
      WorkerThreadStartupData::Create(isNodeJS, main_script,
          script_url, user_agent, source_code, nullptr, start_mode,
          csp->Headers().get(), referrer_policy, starter_origin,
          worker_clients_.Release(), document->AddressSpace(),
          OriginTrialContext::GetTokens(document).get(),
          std::move(worker_settings), worker_v8_settings);

  InitializeWorkerThread(std::move(startup_data));
  GetWorkerInspectorProxy()->WorkerThreadCreated(document, GetWorkerThread(),
                                                 script_url);
}

void InProcessWorkerMessagingProxy::PostMessageToWorkerObject(
    PassRefPtr<SerializedScriptValue> message,
    MessagePortChannelArray channels) {
  DCHECK(IsParentContextThread());
  if (!worker_object_ || AskedToTerminate())
    return;

  MessagePortArray* ports =
      MessagePort::EntanglePorts(*GetExecutionContext(), std::move(channels));
  worker_object_->DispatchEvent(
      MessageEvent::Create(ports, std::move(message)));
}

void InProcessWorkerMessagingProxy::PostMessageToWorkerGlobalScope(
    PassRefPtr<SerializedScriptValue> message,
    MessagePortChannelArray channels) {
  DCHECK(IsParentContextThread());
  if (AskedToTerminate())
    return;

  if (GetWorkerThread()) {
    // A message event is an activity and may initiate another activity.
    worker_global_scope_has_pending_activity_ = true;
    ++unconfirmed_message_count_;
    std::unique_ptr<WTF::CrossThreadClosure> task = CrossThreadBind(
        &InProcessWorkerObjectProxy::ProcessMessageFromWorkerObject,
        CrossThreadUnretained(&WorkerObjectProxy()), std::move(message),
        WTF::Passed(std::move(channels)),
        CrossThreadUnretained(GetWorkerThread()));
    TaskRunnerHelper::Get(TaskType::kPostedMessage, GetWorkerThread())
        ->PostTask(BLINK_FROM_HERE, std::move(task));
  } else {
    queued_early_tasks_.push_back(
        QueuedTask{std::move(message), std::move(channels)});
  }
}

void InProcessWorkerMessagingProxy::DispatchErrorEvent(
    const String& error_message,
    std::unique_ptr<SourceLocation> location,
    int exception_id) {
  DCHECK(IsParentContextThread());
  if (!worker_object_)
    return;

  // We don't bother checking the askedToTerminate() flag here, because
  // exceptions should *always* be reported even if the thread is terminated.
  // This is intentionally different than the behavior in MessageWorkerTask,
  // because terminated workers no longer deliver messages (section 4.6 of the
  // WebWorker spec), but they do report exceptions.

  ErrorEvent* event =
      ErrorEvent::Create(error_message, location->Clone(), nullptr);
  if (worker_object_->DispatchEvent(event) != DispatchEventResult::kNotCanceled)
    return;

  // The HTML spec requires to queue an error event using the DOM manipulation
  // task source.
  // https://html.spec.whatwg.org/multipage/workers.html#runtime-script-errors-2
  TaskRunnerHelper::Get(TaskType::kDOMManipulation, GetWorkerThread())
      ->PostTask(BLINK_FROM_HERE,
                 CrossThreadBind(
                     &InProcessWorkerObjectProxy::ProcessUnhandledException,
                     CrossThreadUnretained(worker_object_proxy_.get()),
                     exception_id, CrossThreadUnretained(GetWorkerThread())));
}

void InProcessWorkerMessagingProxy::WorkerThreadCreated() {
  DCHECK(IsParentContextThread());
  ThreadedMessagingProxyBase::WorkerThreadCreated();

  // Worker initialization means a pending activity.
  worker_global_scope_has_pending_activity_ = true;

  DCHECK_EQ(0u, unconfirmed_message_count_);
  unconfirmed_message_count_ = queued_early_tasks_.size();
  for (auto& queued_task : queued_early_tasks_) {
    std::unique_ptr<WTF::CrossThreadClosure> task = CrossThreadBind(
        &InProcessWorkerObjectProxy::ProcessMessageFromWorkerObject,
        CrossThreadUnretained(&WorkerObjectProxy()),
        queued_task.message.Release(),
        WTF::Passed(std::move(queued_task.channels)),
        CrossThreadUnretained(GetWorkerThread()));
    TaskRunnerHelper::Get(TaskType::kPostedMessage, GetWorkerThread())
        ->PostTask(BLINK_FROM_HERE, std::move(task));
  }
  queued_early_tasks_.clear();
}

void InProcessWorkerMessagingProxy::ParentObjectDestroyed() {
  DCHECK(IsParentContextThread());

  // parentObjectDestroyed() is called in InProcessWorkerBase's destructor.
  // Thus it should be guaranteed that a weak pointer m_workerObject has been
  // cleared before this method gets called.
  DCHECK(!worker_object_);

  ThreadedMessagingProxyBase::ParentObjectDestroyed();
}

void InProcessWorkerMessagingProxy::ConfirmMessageFromWorkerObject() {
  DCHECK(IsParentContextThread());
  if (AskedToTerminate())
    return;
  DCHECK(worker_global_scope_has_pending_activity_);
  DCHECK_GT(unconfirmed_message_count_, 0u);
  --unconfirmed_message_count_;
}

void InProcessWorkerMessagingProxy::PendingActivityFinished() {
  DCHECK(IsParentContextThread());
  DCHECK(worker_global_scope_has_pending_activity_);
  if (unconfirmed_message_count_ > 0) {
    // Ignore the report because an inflight message event may initiate a
    // new activity.
    return;
  }
  worker_global_scope_has_pending_activity_ = false;
}

bool InProcessWorkerMessagingProxy::HasPendingActivity() const {
  DCHECK(IsParentContextThread());
  if (AskedToTerminate())
    return false;
  return worker_global_scope_has_pending_activity_;
}

}  // namespace blink
