// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/chrome_extension_web_contents_observer.h"

#include "base/command_line.h"
#include "base/metrics/field_trial.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/extensions/error_console/error_console.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/extensions/window_controller.h"
#include "chrome/browser/metrics/chrome_metrics_service_accessor.h"
#include "chrome/common/extensions/chrome_extension_messages.h"
#include "chrome/common/extensions/extension_process_policy.h"
#include "chrome/common/url_constants.h"
#include "components/rappor/rappor_service_impl.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/common/content_switches.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_system.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension_messages.h"
#include "extensions/common/extension_urls.h"
#include "extensions/common/switches.h"

#include "components/zoom/zoom_controller.h"
#include "content/public/browser/web_contents.h"

using content::BrowserContext;

DEFINE_WEB_CONTENTS_USER_DATA_KEY(
    extensions::ChromeExtensionWebContentsObserver);

namespace extensions {

ChromeExtensionWebContentsObserver::ChromeExtensionWebContentsObserver(
    content::WebContents* web_contents)
    : ExtensionWebContentsObserver(web_contents) {
  // Since ZoomController is also a WebContentsObserver, we need to be careful
  // about disconnecting from it since the relative order of destruction of
  // WebContentsObservers is not guaranteed. ZoomController silently clears
  // its ZoomObserver list during WebContentsDestroyed() so there's no need
  // to explicitly remove ourselves on destruction.
  zoom::ZoomController* zoom_controller =
      zoom::ZoomController::FromWebContents(web_contents);
  // There may not always be a ZoomController, e.g. in tests.
  if (zoom_controller)
    zoom_controller->AddObserver(this);
}

void ChromeExtensionWebContentsObserver::OnZoomChanged(
    const zoom::ZoomController::ZoomChangedEventData& data) {
  ProcessManager* const process_manager = ProcessManager::Get(browser_context());
  const Extension* const extension =
      process_manager->GetExtensionForWebContents(web_contents());
  if (extension) {
    base::ListValue args;
    args.AppendDouble(data.old_zoom_level);
    args.AppendDouble(data.new_zoom_level);

    content::RenderFrameHost* rfh = web_contents()->GetMainFrame();
    rfh->Send(new ExtensionMsg_MessageInvoke(
      rfh->GetRoutingID(), extension->id(), "nw.Window",
      "updateAppWindowZoom", args));
  }
}

ChromeExtensionWebContentsObserver::~ChromeExtensionWebContentsObserver() {}

void ChromeExtensionWebContentsObserver::RenderViewCreated(
    content::RenderViewHost* render_view_host) {
  ReloadIfTerminated(render_view_host);
  ExtensionWebContentsObserver::RenderViewCreated(render_view_host);

  const Extension* extension = GetExtension(render_view_host);
  if (!extension)
    return;

  int process_id = render_view_host->GetProcess()->GetID();
  auto* policy = content::ChildProcessSecurityPolicy::GetInstance();

  // Components of chrome that are implemented as extensions or platform apps
  // are allowed to use chrome://resources/ and chrome://theme/ URLs.
  if ((extension->is_extension() || extension->is_platform_app()) &&
      (Manifest::IsComponentLocation(extension->location()) ||
       extension->is_nwjs_app())) {
    policy->GrantOrigin(process_id,
                        url::Origin(GURL(content::kChromeUIResourcesURL)));
    policy->GrantOrigin(process_id,
                        url::Origin(GURL(chrome::kChromeUIThemeURL)));
  }

  // Extensions, legacy packaged apps, and component platform apps are allowed
  // to use chrome://favicon/ and chrome://extension-icon/ URLs. Hosted apps are
  // not allowed because they are served via web servers (and are generally
  // never given access to Chrome APIs).
  if (extension->is_extension() ||
      extension->is_legacy_packaged_app() ||
      extension->is_nwjs_app() ||
      (extension->is_platform_app() &&
       Manifest::IsComponentLocation(extension->location()))) {
    policy->GrantOrigin(process_id,
                        url::Origin(GURL(chrome::kChromeUIFaviconURL)));
    policy->GrantOrigin(process_id,
                        url::Origin(GURL(chrome::kChromeUIExtensionIconURL)));
  }
}

void ChromeExtensionWebContentsObserver::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  ExtensionWebContentsObserver::DidFinishNavigation(navigation_handle);
}

bool ChromeExtensionWebContentsObserver::OnMessageReceived(
    const IPC::Message& message,
    content::RenderFrameHost* render_frame_host) {
  if (ExtensionWebContentsObserver::OnMessageReceived(message,
                                                      render_frame_host)) {
    return true;
  }

  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP_WITH_PARAM(ChromeExtensionWebContentsObserver, message,
                                   render_frame_host)
    IPC_MESSAGE_HANDLER(ExtensionHostMsg_DetailedConsoleMessageAdded,
                        OnDetailedConsoleMessageAdded)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

void ChromeExtensionWebContentsObserver::OnDetailedConsoleMessageAdded(
    content::RenderFrameHost* render_frame_host,
    const base::string16& message,
    const base::string16& source,
    const StackTrace& stack_trace,
    int32_t severity_level) {
  if (!IsSourceFromAnExtension(source))
    return;

  std::string extension_id = GetExtensionIdFromFrame(render_frame_host);
  if (extension_id.empty())
    extension_id = GURL(source).host();

  ErrorConsole::Get(browser_context())
      ->ReportError(std::unique_ptr<ExtensionError>(new RuntimeError(
          extension_id, browser_context()->IsOffTheRecord(), source, message,
          stack_trace, web_contents()->GetLastCommittedURL(),
          static_cast<logging::LogSeverity>(severity_level),
          render_frame_host->GetRoutingID(),
          render_frame_host->GetProcess()->GetID())));
}

void ChromeExtensionWebContentsObserver::InitializeRenderFrame(
    content::RenderFrameHost* render_frame_host) {
  ExtensionWebContentsObserver::InitializeRenderFrame(render_frame_host);
  WindowController* controller = dispatcher()->GetExtensionWindowController();
  if (controller) {
    render_frame_host->Send(new ExtensionMsg_UpdateBrowserWindowId(
        render_frame_host->GetRoutingID(), controller->GetWindowId()));
  }
}

void ChromeExtensionWebContentsObserver::ReloadIfTerminated(
    content::RenderViewHost* render_view_host) {
  std::string extension_id = GetExtensionId(render_view_host);
  if (extension_id.empty())
    return;

  ExtensionRegistry* registry = ExtensionRegistry::Get(browser_context());

  // Reload the extension if it has crashed.
  // TODO(yoz): This reload doesn't happen synchronously for unpacked
  //            extensions. It seems to be fast enough, but there is a race.
  //            We should delay loading until the extension has reloaded.
  if (registry->GetExtensionById(extension_id, ExtensionRegistry::TERMINATED)) {
    ExtensionSystem::Get(browser_context())->
        extension_service()->ReloadExtension(extension_id);
  }
}

}  // namespace extensions
