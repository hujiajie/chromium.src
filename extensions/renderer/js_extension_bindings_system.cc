// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/renderer/js_extension_bindings_system.h"

#include "base/command_line.h"
#include "base/strings/string_split.h"
#include "content/public/child/v8_value_converter.h"
#include "content/public/common/content_switches.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_api.h"
#include "extensions/common/extension_urls.h"
#include "extensions/common/extensions_client.h"
#include "extensions/common/features/feature.h"
#include "extensions/common/features/feature_provider.h"
#include "extensions/common/manifest_constants.h"
#include "extensions/common/manifest_handlers/externally_connectable.h"
#include "extensions/renderer/binding_generating_native_handler.h"
#include "extensions/renderer/event_bindings.h"
#include "extensions/renderer/renderer_extension_registry.h"
#include "extensions/renderer/resource_bundle_source_map.h"
#include "extensions/renderer/script_context.h"
#include "gin/converter.h"
#include "v8/include/v8.h"

namespace extensions {

namespace {

// Gets |field| from |object| or creates it as an empty object if it doesn't
// exist.
v8::Local<v8::Object> GetOrCreateObject(const v8::Local<v8::Object>& object,
                                        const std::string& field,
                                        v8::Isolate* isolate) {
  v8::Local<v8::String> key = v8::String::NewFromUtf8(isolate, field.c_str());
  // If the object has a callback property, it is assumed it is an unavailable
  // API, so it is safe to delete. This is checked before GetOrCreateObject is
  // called.
  if (object->HasRealNamedCallbackProperty(key)) {
    object->Delete(key);
  } else if (object->HasRealNamedProperty(key)) {
    v8::Local<v8::Value> value = object->Get(key);
    CHECK(value->IsObject());
    return v8::Local<v8::Object>::Cast(value);
  }

  v8::Local<v8::Object> new_object = v8::Object::New(isolate);
  object->Set(key, new_object);
  return new_object;
}

// Returns the global value for "chrome" from |context|. If one doesn't exist
// creates a new object for it. If a chrome property exists on the window
// already (as in the case when a script did `window.chrome = true`), returns
// an empty object.
v8::Local<v8::Object> GetOrCreateChrome(ScriptContext* context, bool hidden, const char* name = nullptr) {
  v8::Local<v8::String> chrome_string(
                                      v8::String::NewFromUtf8(context->isolate(), name ? name : "chrome"));
  v8::Local<v8::Object> global(context->v8_context()->Global());
  if (!hidden) {
  v8::Local<v8::Value> chrome(global->Get(chrome_string));
  if (chrome->IsUndefined()) {
    chrome = v8::Object::New(context->isolate());
    global->Set(chrome_string, chrome);
  }
  return chrome->IsObject() ? chrome.As<v8::Object>() : v8::Local<v8::Object>();
  } else { //hidden
    // MUST MATCH Private() in module_system.cc
    v8::Local<v8::Value> privates;
    if (!context->module_system()->GetPrivate(global, "privates", &privates) || !privates->IsObject()) {
      privates = v8::Object::New(context->isolate());
      context->module_system()->SetPrivate(global, "privates", privates);
    }
    v8::Local<v8::Object> priv_obj = privates->ToObject();
    v8::Local<v8::Value> chrome(priv_obj->Get(chrome_string));
    if (chrome->IsUndefined()) {
      chrome = v8::Object::New(context->isolate());
      v8::Local<v8::String> hidden_key(
       v8::String::NewFromUtf8(context->isolate(), "__nw_is_hidden"));
      chrome->ToObject()->Set(hidden_key, v8::Boolean::New(context->isolate(), true));
      priv_obj->Set(chrome_string, chrome);
    }
    return chrome->IsObject() ? chrome.As<v8::Object>() : v8::Local<v8::Object>();
  }
}

v8::Local<v8::Object> GetOrCreateBindObjectIfAvailable(
    const std::string& api_name,
    std::string* bind_name,
    ScriptContext* context, bool hidden = false) {
  std::vector<std::string> split = base::SplitString(
      api_name, ".", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);

  v8::Local<v8::Object> bind_object;

  // Check if this API has an ancestor. If the API's ancestor is available and
  // the API is not available, don't install the bindings for this API. If
  // the API is available and its ancestor is not, delete the ancestor and
  // install the bindings for the API. This is to prevent loading the ancestor
  // API schema if it will not be needed.
  //
  // For example:
  //  If app is available and app.window is not, just install app.
  //  If app.window is available and app is not, delete app and install
  //  app.window on a new object so app does not have to be loaded.
  const FeatureProvider* api_feature_provider =
      FeatureProvider::GetAPIFeatures();
  std::string ancestor_name;
  bool only_ancestor_available = false;

  const char* prefix = nullptr;
  int start = 0;
  if (split[0] == "nw") {
    prefix = "nw";
    start = 1;
  }
  for (size_t i = start; i < split.size() - 1; ++i) {
    ancestor_name += (i ? "." : "") + split[i];
    if (api_feature_provider->GetFeature(ancestor_name) && !hidden &&
        context->GetAvailability(ancestor_name).is_available() &&
        !context->GetAvailability(api_name).is_available()) {
      only_ancestor_available = true;
      break;
    }

    if (bind_object.IsEmpty()) {
      bind_object = GetOrCreateChrome(context, hidden, prefix);
      if (bind_object.IsEmpty())
        return v8::Local<v8::Object>();
    }
    bind_object = GetOrCreateObject(bind_object, split[i], context->isolate());
  }

  if (only_ancestor_available)
    return v8::Local<v8::Object>();

  DCHECK(bind_name);
  *bind_name = split.back();

  return bind_object.IsEmpty() ? GetOrCreateChrome(context, hidden, prefix) : bind_object;
}

// Determines if a ScriptContext can connect to any externally_connectable-
// enabled extension.
bool IsRuntimeAvailableToContext(ScriptContext* context) {
  for (const auto& extension :
       *RendererExtensionRegistry::Get()->GetMainThreadExtensionSet()) {
    ExternallyConnectableInfo* info = static_cast<ExternallyConnectableInfo*>(
        extension->GetManifestData(manifest_keys::kExternallyConnectable));
    if (info && info->matches.MatchesURL(context->url()))
      return true;
  }
  return false;
}

// Creates the event bindings if necessary for the given |context|.
void MaybeCreateEventBindings(ScriptContext* context) {
  // chrome.Event is part of the public API (although undocumented). Make it
  // lazily evalulate to Event from event_bindings.js. For extensions only
  // though, not all webpages!
  if (!context->extension())
    return;
  v8::Local<v8::Object> chrome = GetOrCreateChrome(context, false);
  if (chrome.IsEmpty())
    return;
  context->module_system()->SetLazyField(chrome, "Event", kEventBindings,
                                         "Event");
}

}  // namespace

JsExtensionBindingsSystem::JsExtensionBindingsSystem(
    ResourceBundleSourceMap* source_map,
    std::unique_ptr<RequestSender> request_sender)
    : source_map_(source_map), request_sender_(std::move(request_sender)) {}

JsExtensionBindingsSystem::~JsExtensionBindingsSystem() {}

void JsExtensionBindingsSystem::DidCreateScriptContext(ScriptContext* context) {
  MaybeCreateEventBindings(context);
}

void JsExtensionBindingsSystem::WillReleaseScriptContext(
    ScriptContext* context) {
  // TODO(kalman): Make |request_sender| use |context->AddInvalidationObserver|.
  // In fact |request_sender_| should really be owned by ScriptContext.
  request_sender_->InvalidateSource(context);
}

void JsExtensionBindingsSystem::UpdateBindingsForContext(
    ScriptContext* context) {
  v8::HandleScope handle_scope(context->isolate());
  v8::Context::Scope context_scope(context->v8_context());

  bool nodejs_enabled = false;
  if (context->extension()) {
    nodejs_enabled = context->extension()->is_nwjs_app();
    context->extension()->manifest()->GetBoolean(manifest_keys::kNWJSEnableNode, &nodejs_enabled);
  }

  // TODO(kalman): Make the bindings registration have zero overhead then run
  // the same code regardless of context type.
  switch (context->context_type()) {
    case Feature::UNSPECIFIED_CONTEXT:
    case Feature::WEB_PAGE_CONTEXT:
    case Feature::BLESSED_WEB_PAGE_CONTEXT:
      // Hard-code registration of any APIs that are exposed to webpage-like
      // contexts, because it's too expensive to run the full bindings code.
      // All of the same permission checks will still apply.
      if (context->GetAvailability("app").is_available())
        RegisterBinding("app", "app", context);
      if (!context->GetAvailability("app.window").is_available()) {
        RegisterBinding("app.window", "app.window", context, true);
        RegisterBinding("nw.Window", "nw.Window", context, true);
        RegisterBinding("runtime", "runtime", context, true);
      }
      if (context->GetAvailability("webstore").is_available())
        RegisterBinding("webstore", "webstore", context);
      if (context->GetAvailability("dashboardPrivate").is_available())
        RegisterBinding("dashboardPrivate", "dashboardPrivate", context);
      if (IsRuntimeAvailableToContext(context))
        RegisterBinding("runtime", "runtime", context);
      break;

    case Feature::SERVICE_WORKER_CONTEXT:
      DCHECK(ExtensionsClient::Get()
                 ->ExtensionAPIEnabledInExtensionServiceWorkers());
    // Intentional fallthrough.
    case Feature::BLESSED_EXTENSION_CONTEXT:
    case Feature::UNBLESSED_EXTENSION_CONTEXT:
    case Feature::CONTENT_SCRIPT_CONTEXT:
    case Feature::WEBUI_CONTEXT: {
      // Extension context; iterate through all the APIs and bind the available
      // ones.
      const FeatureProvider* api_feature_provider =
          FeatureProvider::GetAPIFeatures();
      for (const auto& map_entry : api_feature_provider->GetAllFeatures()) {
        if (map_entry.first.substr(0, 3) == "nw." && !nodejs_enabled)
          continue;
        // Internal APIs are included via require(api_name) from internal code
        // rather than chrome[api_name].
        if (map_entry.second->IsInternal())
          continue;

        // If this API has a parent feature (and isn't marked 'noparent'),
        // then this must be a function or event, so we should not register.
        if (api_feature_provider->GetParent(map_entry.second.get()) != nullptr)
          continue;

        // Skip chrome.test if this isn't a test.
        if (map_entry.first == "test" &&
            !base::CommandLine::ForCurrentProcess()->HasSwitch(
                ::switches::kTestType)) {
          continue;
        }

        if (context->IsAnyFeatureAvailableToContext(
                *map_entry.second, CheckAliasStatus::NOT_ALLOWED)) {
          // Check if the API feature is indeed an alias. If it is, the API
          // should use source API bindings as its own.
          const std::string& source = map_entry.second->source();
          // TODO(lazyboy): RegisterBinding() uses |source_map_|, any thread
          // safety issue?
          RegisterBinding(source.empty() ? map_entry.first : source,
                          map_entry.first, context);
        }
      }
      break;
    }
  }
}

void JsExtensionBindingsSystem::HandleResponse(int request_id,
                                               bool success,
                                               const base::ListValue& response,
                                               const std::string& error) {
  request_sender_->HandleResponse(request_id, success, response, error);
}

RequestSender* JsExtensionBindingsSystem::GetRequestSender() {
  return request_sender_.get();
}

void JsExtensionBindingsSystem::DispatchEventInContext(
    const std::string& event_name,
    const base::ListValue* event_args,
    const base::DictionaryValue* filtering_info,
    ScriptContext* context) {
  EventBindings::DispatchEventInContext(event_name, event_args, filtering_info,
                                        context);
}

void JsExtensionBindingsSystem::RegisterBinding(
    const std::string& api_name,
    const std::string& api_bind_name,
    ScriptContext* context, bool hidden) {
  std::string bind_name;
  v8::Local<v8::Object> bind_object =
    GetOrCreateBindObjectIfAvailable(api_bind_name, &bind_name, context, hidden);

  // Empty if the bind object failed to be created, probably because the
  // extension overrode chrome with a non-object, e.g. window.chrome = true.
  if (bind_object.IsEmpty())
    return;

  v8::Local<v8::String> v8_bind_name =
      v8::String::NewFromUtf8(context->isolate(), bind_name.c_str());
  if (bind_object->HasRealNamedProperty(v8_bind_name)) {
    // The bind object may already have the property if the API has been
    // registered before (or if the extension has put something there already,
    // but, whatevs).
    //
    // In the former case, we need to re-register the bindings for the APIs
    // which the extension now has permissions for (if any), but not touch any
    // others so that we don't destroy state such as event listeners.
    //
    // TODO(kalman): Only register available APIs to make this all moot.
    if (bind_object->HasRealNamedCallbackProperty(v8_bind_name))
      return;  // lazy binding still there, nothing to do
    if (bind_object->Get(v8_bind_name)->IsObject())
      return;  // binding has already been fully installed
  }

  ModuleSystem* module_system = context->module_system();
  if (!source_map_->Contains(api_name)) {
    module_system->RegisterNativeHandler(
        api_bind_name,
        std::unique_ptr<NativeHandler>(
            new BindingGeneratingNativeHandler(context, api_name, "binding")));
    module_system->SetNativeLazyField(bind_object, bind_name, api_bind_name,
                                      "binding");
  } else {
    module_system->SetLazyField(bind_object, bind_name, api_name, "binding");
  }
}

}  // namespace extensions
