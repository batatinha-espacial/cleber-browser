/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/DebugOnly.h"

#include "nsXULAppAPI.h"

#include <stdlib.h>
#if defined(MOZ_WIDGET_GTK)
#  include <glib.h>
#endif

#include "prenv.h"

#include "nsIAppShell.h"
#include "nsIToolkitProfile.h"

#ifdef XP_WIN
#  include <process.h>
#  include <shobjidl.h>
#  include "mozilla/ipc/WindowsMessageLoop.h"
#  ifdef MOZ_SANDBOX
#    include "mozilla/RandomNum.h"
#  endif
#  include "mozilla/ScopeExit.h"
#  include "mozilla/WinDllServices.h"
#  include "mozilla/WindowsBCryptInitialization.h"
#  include "WinUtils.h"
#endif

#include "nsAppRunner.h"
#include "nsExceptionHandler.h"
#include "mozilla/RuntimeExceptionModule.h"
#include "nsThreadUtils.h"
#include "nsJSUtils.h"
#include "nsWidgetsCID.h"
#include "nsXREDirProvider.h"
#ifdef MOZ_ASAN_REPORTER
#  include "CmdLineAndEnvUtils.h"
#  include "nsIFile.h"
#endif

#include "mozilla/Omnijar.h"
#if defined(XP_MACOSX)
#  include <mach/mach.h>
#  include <servers/bootstrap.h>
#  include "nsVersionComparator.h"
#  include "chrome/common/mach_ipc_mac.h"
#  include "gfxPlatformMac.h"
#endif
#include "nsX11ErrorHandler.h"
#include "nsGDKErrorHandler.h"
#include "base/at_exit.h"
#include "base/message_loop.h"
#include "base/process_util.h"
#if defined(MOZ_WIDGET_ANDROID)
#  include "chrome/common/ipc_channel.h"
#  include "mozilla/jni/Utils.h"
#  include "mozilla/ipc/ProcessUtils.h"
#endif  //  defined(MOZ_WIDGET_ANDROID)

#include "mozilla/AbstractThread.h"
#include "mozilla/FilePreferences.h"
#include "mozilla/IOInterposer.h"
#include "mozilla/ProcessType.h"
#include "mozilla/RDDProcessImpl.h"
#include "mozilla/ipc/UtilityProcessImpl.h"
#include "mozilla/UniquePtr.h"

#include "mozilla/ipc/BrowserProcessSubThread.h"
#include "mozilla/ipc/IOThreadChild.h"
#include "mozilla/ipc/ProcessChild.h"

#include "mozilla/dom/ContentProcess.h"
#include "mozilla/dom/ContentParent.h"

#include "mozilla/ipc/TestShellParent.h"
#if defined(XP_WIN)
#  include "mozilla/WindowsConsole.h"
#  include "mozilla/WindowsDllBlocklist.h"
#endif

#include "GMPProcessChild.h"
#include "mozilla/gfx/GPUProcessImpl.h"
#include "mozilla/net/SocketProcessImpl.h"

#include "ProfilerControl.h"

#if defined(MOZ_SANDBOX) && defined(XP_WIN)
#  include "mozilla/sandboxTarget.h"
#  include "mozilla/sandboxing/loggingCallbacks.h"
#  include "mozilla/RemoteSandboxBrokerProcessChild.h"
#endif

#if defined(MOZ_SANDBOX)
#  include "XREChildData.h"
#  include "mozilla/SandboxSettings.h"
#endif

#if defined(XP_LINUX) && defined(MOZ_SANDBOX)
#  include "mozilla/Sandbox.h"
#endif

#if defined(XP_LINUX)
#  include <sys/prctl.h>
#  ifndef PR_SET_PTRACER
#    define PR_SET_PTRACER 0x59616d61
#  endif
#  ifndef PR_SET_PTRACER_ANY
#    define PR_SET_PTRACER_ANY ((unsigned long)-1)
#  endif
#endif

#ifdef MOZ_JPROF
#  include "jprof.h"
#endif

#if defined(XP_WIN) && defined(MOZ_SANDBOX)
#  include "mozilla/sandboxing/SandboxInitialization.h"
#  include "mozilla/sandboxing/sandboxLogging.h"
#endif

#if defined(MOZ_ENABLE_FORKSERVER)
#  include "mozilla/ipc/ForkServer.h"
#endif

#if defined(MOZ_X11)
#  include <X11/Xlib.h>
#endif

#include "VRProcessChild.h"

using namespace mozilla;

using mozilla::ipc::BrowserProcessSubThread;
using mozilla::ipc::GeckoChildProcessHost;
using mozilla::ipc::IOThreadChild;
using mozilla::ipc::ProcessChild;

using mozilla::dom::ContentParent;
using mozilla::dom::ContentProcess;

using mozilla::gmp::GMPProcessChild;

using mozilla::ipc::TestShellCommandParent;
using mozilla::ipc::TestShellParent;

namespace mozilla::_ipdltest {
// Set in IPDLUnitTest.cpp when running gtests.
UniquePtr<mozilla::ipc::ProcessChild> (*gMakeIPDLUnitTestProcessChild)(
    base::ProcessId, const nsID&) = nullptr;
}  // namespace mozilla::_ipdltest

static NS_DEFINE_CID(kAppShellCID, NS_APPSHELL_CID);

const char* XRE_GeckoProcessTypeToString(GeckoProcessType aProcessType) {
  switch (aProcessType) {
#define GECKO_PROCESS_TYPE(enum_value, enum_name, string_name, proc_typename, \
                           process_bin_type, procinfo_typename,               \
                           webidl_typename, allcaps_name)                     \
  case GeckoProcessType::GeckoProcessType_##enum_name:                        \
    return string_name;
#include "mozilla/GeckoProcessTypes.h"
#undef GECKO_PROCESS_TYPE
    default:
      return "invalid";
  }
}

const char* XRE_ChildProcessTypeToAnnotation(GeckoProcessType aProcessType) {
  switch (aProcessType) {
    case GeckoProcessType_GMPlugin:
      return "plugin";
    case GeckoProcessType_Default:
      return "";
    case GeckoProcessType_Content:
      return "content";
    default:
      return XRE_GeckoProcessTypeToString(aProcessType);
  }
}

#if defined(MOZ_WIDGET_ANDROID)
void XRE_SetAndroidChildFds(JNIEnv* env, const XRE_AndroidChildFds& fds) {
  mozilla::jni::SetGeckoThreadEnv(env);
  mozilla::ipc::SetPrefsFd(fds.mPrefsFd);
  mozilla::ipc::SetPrefMapFd(fds.mPrefMapFd);
  IPC::Channel::SetClientChannelFd(fds.mIpcFd);
  CrashReporter::SetNotificationPipeForChild(fds.mCrashFd);
}
#endif  // defined(MOZ_WIDGET_ANDROID)

void XRE_SetProcessType(const char* aProcessTypeString) {
  SetGeckoProcessType(aProcessTypeString);
}

#if defined(XP_WIN)
void SetTaskbarGroupId(const nsString& aId) {
  if (FAILED(SetCurrentProcessExplicitAppUserModelID(aId.get()))) {
    NS_WARNING(
        "SetCurrentProcessExplicitAppUserModelID failed for child process.");
  }
}
#endif

#if defined(MOZ_SANDBOX)
void AddContentSandboxLevelAnnotation() {
  if (XRE_GetProcessType() == GeckoProcessType_Content) {
    int level = GetEffectiveContentSandboxLevel();
    CrashReporter::AnnotateCrashReport(
        CrashReporter::Annotation::ContentSandboxLevel, level);
  } else if (XRE_GetProcessType() == GeckoProcessType_GPU) {
    int level = GetEffectiveGpuSandboxLevel();
    CrashReporter::AnnotateCrashReport(
        CrashReporter::Annotation::GpuSandboxLevel, level);
  }
}
#endif /* MOZ_SANDBOX */

namespace {

int GetDebugChildPauseTime() {
  auto pauseStr = PR_GetEnv("MOZ_DEBUG_CHILD_PAUSE");
  if (pauseStr && *pauseStr) {
    int pause = atoi(pauseStr);
    if (pause != 1) {  // must be !=1 since =1 enables the default pause time
#if defined(XP_WIN)
      pause *= 1000;  // convert to ms
#endif
      return pause;
    }
  }
#ifdef XP_UNIX
  return 30;  // seconds
#elif defined(XP_WIN)
  return 10000;  // milliseconds
#else
  return 0;
#endif
}

static bool IsCrashReporterEnabled(const char* aArg) {
  // on windows and mac, |aArg| is the named pipe on which the server is
  // listening for requests, or "-" if crash reporting is disabled.
#if defined(XP_MACOSX) || defined(XP_WIN)
  return 0 != strcmp("-", aArg);
#else
  // on POSIX, |aArg| is "true" if crash reporting is enabled, false otherwise
  return 0 != strcmp("false", aArg);
#endif
}

}  // namespace

nsresult XRE_InitChildProcess(int aArgc, char* aArgv[],
                              const XREChildData* aChildData) {
  NS_ENSURE_ARG_MIN(aArgc, 2);
  NS_ENSURE_ARG_POINTER(aArgv);
  NS_ENSURE_ARG_POINTER(aArgv[0]);
  MOZ_ASSERT(aChildData);

  NS_SetCurrentThreadName("MainThread");

#ifdef MOZ_ASAN_REPORTER
  // In ASan reporter builds, we need to set ASan's log_path as early as
  // possible, so it dumps its errors into files there instead of using
  // the default stderr location. Since this is crucial for ASan reporter
  // to work at all (and we don't want people to use a non-functional
  // ASan reporter build), all failures while setting log_path are fatal.
  //
  // We receive this log_path via the ASAN_REPORTER_PATH environment variable
  // because there is no other way to generically get the necessary profile
  // directory in all child types without adding support for that in each
  // child process type class (at the risk of missing this in a child).
  //
  // In certain cases (e.g. child startup through xpcshell or gtests), this
  // code needs to remain disabled, as no ASAN_REPORTER_PATH would be available.
  if (!PR_GetEnv("MOZ_DISABLE_ASAN_REPORTER") && !PR_GetEnv("MOZ_RUN_GTEST")) {
    nsCOMPtr<nsIFile> asanReporterPath = GetFileFromEnv("ASAN_REPORTER_PATH");
    if (!asanReporterPath) {
      MOZ_CRASH("Child did not receive ASAN_REPORTER_PATH!");
    }
    setASanReporterPath(asanReporterPath);
  }
#endif

#if defined(XP_LINUX) && defined(MOZ_SANDBOX)
  // This has to happen before glib thread pools are started.
  mozilla::SandboxEarlyInit();
  // This just needs to happen before sandboxing, to initialize the
  // cached value, but libmozsandbox can't see this symbol.
  mozilla::GetNumberOfProcessors();
#endif

#ifdef MOZ_JPROF
  // Call the code to install our handler
  setupProfilingStuff();
#endif

#if defined(XP_WIN)
  // From the --attach-console support in nsNativeAppSupportWin.cpp, but
  // here we are a content child process, so we always attempt to attach
  // to the parent's (ie, the browser's) console.
  // Try to attach console to the parent process.
  // It will succeed when the parent process is a command line,
  // so that stdio will be displayed in it.
  UseParentConsole();

#  if defined(MOZ_SANDBOX)
  if (aChildData->sandboxTargetServices) {
    SandboxTarget::Instance()->SetTargetServices(
        aChildData->sandboxTargetServices);
  }
#  endif
#endif

  // NB: This must be called before profiler_init
  ScopedLogging logger;

  mozilla::LogModule::Init(aArgc, aArgv);

  AUTO_BASE_PROFILER_LABEL("XRE_InitChildProcess (around Gecko Profiler)",
                           OTHER);
  AUTO_PROFILER_INIT;
  AUTO_PROFILER_LABEL("XRE_InitChildProcess", OTHER);

#ifdef XP_MACOSX
  gfxPlatformMac::RegisterSupplementalFonts();
#endif

  // Ensure AbstractThread is minimally setup, so async IPC messages
  // work properly.
  AbstractThread::InitTLS();

  // Complete 'task_t' exchange for Mac OS X. This structure has the same size
  // regardless of architecture so we don't have any cross-arch issues here.
#ifdef XP_MACOSX
  if (aArgc < 1) return NS_ERROR_FAILURE;

#  if defined(MOZ_SANDBOX)
  // Save the original number of arguments to pass to the sandbox
  // setup routine which also uses the crash server argument.
  int allArgc = aArgc;
#  endif /* MOZ_SANDBOX */

  // Acquire the mach bootstrap port name from our command line, and send our
  // task_t to the parent process.
  const char* const mach_port_name = aArgv[--aArgc];

  const int kTimeoutMs = 1000;

  UniqueMachSendRight task_sender;
  kern_return_t kr = bootstrap_look_up(bootstrap_port, mach_port_name,
                                       getter_Transfers(task_sender));
  if (kr != KERN_SUCCESS) {
    NS_WARNING(nsPrintfCString("child bootstrap_look_up failed: %s",
                               mach_error_string(kr))
                   .get());
    return NS_ERROR_FAILURE;
  }

  kr = MachSendPortSendRight(task_sender.get(), mach_task_self(),
                             Some(kTimeoutMs));
  if (kr != KERN_SUCCESS) {
    NS_WARNING(nsPrintfCString("child MachSendPortSendRight failed: %s",
                               mach_error_string(kr))
                   .get());
    return NS_ERROR_FAILURE;
  }

#  if defined(MOZ_SANDBOX)
  std::string sandboxError;
  if (!GeckoChildProcessHost::StartMacSandbox(allArgc, aArgv, sandboxError)) {
    printf_stderr("Sandbox error: %s\n", sandboxError.c_str());
    MOZ_CRASH("Sandbox initialization failed");
  }
#  endif /* MOZ_SANDBOX */

#endif /* XP_MACOSX */

  SetupErrorHandling(aArgv[0]);

  bool exceptionHandlerIsSet = false;
  if (!CrashReporter::IsDummy()) {
#if defined(XP_WIN)
    if (aArgc < 1) {
      return NS_ERROR_FAILURE;
    }
    // Pop the first argument, this is used by the WER runtime exception module
    // which reads it from the command-line so we can just discard it here.
    --aArgc;
#endif

    if (aArgc < 1) return NS_ERROR_FAILURE;
    const char* const crashReporterArg = aArgv[--aArgc];

    if (IsCrashReporterEnabled(crashReporterArg)) {
      exceptionHandlerIsSet =
          CrashReporter::SetRemoteExceptionHandler(crashReporterArg);
      MOZ_ASSERT(exceptionHandlerIsSet,
                 "Should have been able to set remote exception handler");

      if (!exceptionHandlerIsSet) {
        // Bug 684322 will add better visibility into this condition
        NS_WARNING("Could not setup crash reporting\n");
      }
    } else {
      // We might have registered a runtime exception module very early in
      // process startup to catch early crashes. This is before we process the
      // crash reporter arg, so unregister here if it turns out the crash
      // reporter is disabled.
      CrashReporter::UnregisterRuntimeExceptionModule();
    }
  }

  gArgv = aArgv;
  gArgc = aArgc;

#ifdef MOZ_X11
  XInitThreads();
#endif
#ifdef MOZ_WIDGET_GTK
  // Setting the name here avoids the need to pass this through to gtk_init().
  g_set_prgname(aArgv[0]);
#endif

#ifdef XP_UNIX
  if (PR_GetEnv("MOZ_DEBUG_CHILD_PROCESS") ||
      PR_GetEnv("MOZ_DEBUG_CHILD_PAUSE")) {
#  if defined(XP_LINUX) && defined(DEBUG)
    if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) != 0) {
      printf_stderr("Could not allow ptrace from any process.\n");
    }
#  endif
    printf_stderr(
        "\n\nCHILDCHILDCHILDCHILD (process type %s)\n  debug me @ %d\n\n",
        XRE_GetProcessTypeString(), base::GetCurrentProcId());
    sleep(GetDebugChildPauseTime());
  }
#elif defined(XP_WIN)
  if (PR_GetEnv("MOZ_DEBUG_CHILD_PROCESS")) {
    NS_DebugBreak(NS_DEBUG_BREAK,
                  "Invoking NS_DebugBreak() to debug child process", nullptr,
                  __FILE__, __LINE__);
  } else if (PR_GetEnv("MOZ_DEBUG_CHILD_PAUSE")) {
    printf_stderr(
        "\n\nCHILDCHILDCHILDCHILD (process type %s)\n  debug me @ %lu\n\n",
        XRE_GetProcessTypeString(), base::GetCurrentProcId());
    ::Sleep(GetDebugChildPauseTime());
  }
#endif

#ifdef MOZ_WIDGET_ANDROID
  // The parent process already did this, but Gecko child processes on
  // Android aren't descendants of the parent process, so they don't
  // inherit its rlimits.
  mozilla::startup::IncreaseDescriptorLimits();
#endif

  // child processes launched by GeckoChildProcessHost get this magic
  // argument appended to their command lines
  const char* const parentPIDString = aArgv[aArgc - 1];
  MOZ_ASSERT(parentPIDString, "NULL parent PID");
  --aArgc;

  char* end = 0;
  base::ProcessId parentPID = strtol(parentPIDString, &end, 10);
  MOZ_ASSERT(!*end, "invalid parent PID");

  // They also get the initial message channel ID passed in the same manner.
  const char* const messageChannelIdString = aArgv[aArgc - 1];
  MOZ_ASSERT(messageChannelIdString, "NULL MessageChannel Id");
  --aArgc;

  nsID messageChannelId{};
  if (!messageChannelId.Parse(messageChannelIdString)) {
    return NS_ERROR_FAILURE;
  }

#if defined(XP_WIN)
  // On Win7+, when not running as an MSIX package, register the application
  // user model id passed in by parent. This ensures windows created by the
  // container properly group with the parent app on the Win7 taskbar.
  // MSIX packages explicitly do not support setting the appid from within
  // the app, as it is set in the package manifest instead.
  const char* const appModelUserId = aArgv[--aArgc];
  if (appModelUserId && !mozilla::widget::WinUtils::HasPackageIdentity()) {
    // '-' implies no support
    if (*appModelUserId != '-') {
      nsString appId;
      CopyASCIItoUTF16(nsDependentCString(appModelUserId), appId);
      // The version string is encased in quotes
      appId.Trim("\"");
      // Set the id
      SetTaskbarGroupId(appId);
    }
  }
#endif

  base::AtExitManager exitManager;

  nsresult rv = XRE_InitCommandLine(aArgc, aArgv);
  if (NS_FAILED(rv)) {
    return NS_ERROR_FAILURE;
  }

  MessageLoop::Type uiLoopType;
  switch (XRE_GetProcessType()) {
    case GeckoProcessType_Content:
    case GeckoProcessType_GMPlugin:
    case GeckoProcessType_GPU:
    case GeckoProcessType_IPDLUnitTest:
    case GeckoProcessType_VR:
    case GeckoProcessType_RDD:
    case GeckoProcessType_Socket:
    case GeckoProcessType_Utility:
      // Content processes need the XPCOM/chromium frankenventloop
      uiLoopType = MessageLoop::TYPE_MOZILLA_CHILD;
      break;
    case GeckoProcessType_RemoteSandboxBroker:
      uiLoopType = MessageLoop::TYPE_DEFAULT;
      break;
    default:
      uiLoopType = MessageLoop::TYPE_UI;
      break;
  }

#if defined(XP_WIN)
#  if defined(MOZ_SANDBOX)
  if (aChildData->sandboxBrokerServices) {
    SandboxBroker::Initialize(aChildData->sandboxBrokerServices);
    SandboxBroker::GeckoDependentInitialize();
  }
#  endif  // defined(MOZ_SANDBOX)

  {
    DebugOnly<bool> result = mozilla::WindowsBCryptInitialization();
    MOZ_ASSERT(result);
  }
#endif  // defined(XP_WIN)

  {
    // This is a lexical scope for the MessageLoop below.  We want it
    // to go out of scope before NS_LogTerm() so that we don't get
    // spurious warnings about XPCOM objects being destroyed from a
    // static context.

    AutoIOInterposer ioInterposerGuard;

    // Associate this thread with a UI MessageLoop
    MessageLoop uiMessageLoop(uiLoopType);
    {
      UniquePtr<ProcessChild> process;
      switch (XRE_GetProcessType()) {
        case GeckoProcessType_Default:
          MOZ_CRASH("This makes no sense");
          break;

        case GeckoProcessType_Content:
          ioInterposerGuard.Init();
          process = MakeUnique<ContentProcess>(parentPID, messageChannelId);
          break;

        case GeckoProcessType_IPDLUnitTest:
          MOZ_RELEASE_ASSERT(mozilla::_ipdltest::gMakeIPDLUnitTestProcessChild,
                             "xul-gtest not loaded!");
          process = mozilla::_ipdltest::gMakeIPDLUnitTestProcessChild(
              parentPID, messageChannelId);
          break;

        case GeckoProcessType_GMPlugin:
          process =
              MakeUnique<gmp::GMPProcessChild>(parentPID, messageChannelId);
          break;

        case GeckoProcessType_GPU:
          process =
              MakeUnique<gfx::GPUProcessImpl>(parentPID, messageChannelId);
          break;

        case GeckoProcessType_VR:
          process =
              MakeUnique<gfx::VRProcessChild>(parentPID, messageChannelId);
          break;

        case GeckoProcessType_RDD:
          process = MakeUnique<RDDProcessImpl>(parentPID, messageChannelId);
          break;

        case GeckoProcessType_Socket:
          ioInterposerGuard.Init();
          process =
              MakeUnique<net::SocketProcessImpl>(parentPID, messageChannelId);
          break;

        case GeckoProcessType_Utility:
          process =
              MakeUnique<ipc::UtilityProcessImpl>(parentPID, messageChannelId);
          break;

#if defined(MOZ_SANDBOX) && defined(XP_WIN)
        case GeckoProcessType_RemoteSandboxBroker:
          process = MakeUnique<RemoteSandboxBrokerProcessChild>(
              parentPID, messageChannelId);
          break;
#endif

#if defined(MOZ_ENABLE_FORKSERVER)
        case GeckoProcessType_ForkServer:
          MOZ_CRASH("Fork server should not go here");
          break;
#endif
        default:
          MOZ_CRASH("Unknown main thread class");
      }

      if (!process->Init(aArgc, aArgv)) {
        return NS_ERROR_FAILURE;
      }

#if defined(XP_WIN)
      // Set child processes up such that they will get killed after the
      // chrome process is killed in cases where the user shuts the system
      // down or logs off.
      ::SetProcessShutdownParameters(0x280 - 1, SHUTDOWN_NORETRY);

      RefPtr<DllServices> dllSvc(DllServices::Get());
      auto dllSvcDisable =
          MakeScopeExit([&dllSvc]() { dllSvc->DisableFull(); });
#endif

#if defined(MOZ_SANDBOX) && defined(XP_WIN)
      // We need to do this after the process has been initialised, as
      // InitLoggingIfRequired may need access to prefs.
      mozilla::sandboxing::InitLoggingIfRequired(
          aChildData->ProvideLogFunction);
#endif
      if (XRE_GetProcessType() != GeckoProcessType_RemoteSandboxBroker) {
        // Remote sandbox launcher process doesn't have prerequisites for
        // these...
        mozilla::FilePreferences::InitDirectoriesAllowlist();
        mozilla::FilePreferences::InitPrefs();
        OverrideDefaultLocaleIfNeeded();
      }

#if defined(MOZ_SANDBOX)
      AddContentSandboxLevelAnnotation();
#endif

      // Run the UI event loop on the main thread.
      uiMessageLoop.MessageLoop::Run();

      // Allow ProcessChild to clean up after itself before going out of
      // scope and being deleted
      process->CleanUp();
      mozilla::Omnijar::CleanUp();
    }
  }

  CrashReporter::UnsetRemoteExceptionHandler(exceptionHandlerIsSet);

  return XRE_DeinitCommandLine();
}

MessageLoop* XRE_GetIOMessageLoop() {
  if (GetGeckoProcessType() == GeckoProcessType_Default) {
    return BrowserProcessSubThread::GetMessageLoop(BrowserProcessSubThread::IO);
  }
  return IOThreadChild::message_loop();
}

nsresult XRE_RunAppShell() {
  nsCOMPtr<nsIAppShell> appShell(do_GetService(kAppShellCID));
  NS_ENSURE_TRUE(appShell, NS_ERROR_FAILURE);
#if defined(XP_MACOSX)
  if (XRE_UseNativeEventProcessing()) {
    // In content processes that want XPCOM (and hence want
    // AppShell), we usually run our hybrid event loop through
    // MessagePump::Run(), by way of nsBaseAppShell::Run().  The
    // Cocoa nsAppShell impl, however, implements its own Run()
    // that's unaware of MessagePump.  That's all rather suboptimal,
    // but oddly enough not a problem... usually.
    //
    // The problem with this setup comes during startup.
    // XPCOM-in-subprocesses depends on IPC, e.g. to init the pref
    // service, so we have to init IPC first.  But, IPC also
    // indirectly kinda-depends on XPCOM, because MessagePump
    // schedules work from off-main threads (e.g. IO thread) by
    // using NS_DispatchToMainThread().  If the IO thread receives a
    // Message from the parent before nsThreadManager is
    // initialized, then DispatchToMainThread() will fail, although
    // MessagePump will remember the task.  This race condition
    // isn't a problem when appShell->Run() ends up in
    // MessagePump::Run(), because MessagePump will immediate see it
    // has work to do.  It *is* a problem when we end up in [NSApp
    // run], because it's not aware that MessagePump has work that
    // needs to be processed; that was supposed to be signaled by
    // nsIRunnable(s).
    //
    // So instead of hacking Cocoa nsAppShell or rewriting the
    // event-loop system, we compromise here by processing any tasks
    // that might have been enqueued on MessagePump, *before*
    // MessagePump::ScheduleWork was able to successfully
    // DispatchToMainThread().
    MessageLoop* loop = MessageLoop::current();
    bool couldNest = loop->NestableTasksAllowed();

    loop->SetNestableTasksAllowed(true);
    RefPtr<Runnable> task = new MessageLoop::QuitTask();
    loop->PostTask(task.forget());
    loop->Run();

    loop->SetNestableTasksAllowed(couldNest);
  }
#endif  // XP_MACOSX
  return appShell->Run();
}

void XRE_ShutdownChildProcess() {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");

  mozilla::DebugOnly<MessageLoop*> ioLoop = XRE_GetIOMessageLoop();
  MOZ_ASSERT(!!ioLoop, "Bad shutdown order");

  // Quit() sets off the following chain of events
  //  (1) UI loop starts quitting
  //  (2) UI loop returns from Run() in XRE_InitChildProcess()
  //  (3) ProcessChild goes out of scope and terminates the IO thread
  //  (4) ProcessChild joins the IO thread
  //  (5) exit()
  MessageLoop::current()->Quit();

#if defined(XP_MACOSX)
  nsCOMPtr<nsIAppShell> appShell(do_GetService(kAppShellCID));
  if (appShell) {
    // On Mac, we might be only above nsAppShell::Run(), not
    // MessagePump::Run().  See XRE_RunAppShell(). To account for
    // that case, we fire off an Exit() here.  If we were indeed
    // above MessagePump::Run(), this Exit() is just superfluous.
    appShell->Exit();
  }
#endif  // XP_MACOSX
}

namespace {
ContentParent* gContentParent;  // long-lived, manually refcounted
TestShellParent* GetOrCreateTestShellParent() {
  if (!gContentParent) {
    // Use a "web" child process by default.  File a bug if you don't like
    // this and you're sure you wouldn't be better off writing a "browser"
    // chrome mochitest where you can have multiple types of content
    // processes.
    RefPtr<ContentParent> parent =
        ContentParent::GetNewOrUsedBrowserProcess(DEFAULT_REMOTE_TYPE);
    parent.forget(&gContentParent);
  } else if (gContentParent->IsShuttingDown()) {
    return nullptr;
  }
  TestShellParent* tsp = gContentParent->GetTestShellSingleton();
  if (!tsp) {
    tsp = gContentParent->CreateTestShell();
  }
  return tsp;
}

}  // namespace

bool XRE_SendTestShellCommand(JSContext* aCx, JSString* aCommand,
                              JS::Value* aCallback) {
  JS::Rooted<JSString*> cmd(aCx, aCommand);
  TestShellParent* tsp = GetOrCreateTestShellParent();
  NS_ENSURE_TRUE(tsp, false);

  nsAutoJSString command;
  NS_ENSURE_TRUE(command.init(aCx, cmd), false);

  if (!aCallback) {
    return tsp->SendExecuteCommand(command);
  }

  TestShellCommandParent* callback = static_cast<TestShellCommandParent*>(
      tsp->SendPTestShellCommandConstructor(command));
  NS_ENSURE_TRUE(callback, false);

  NS_ENSURE_TRUE(callback->SetCallback(aCx, *aCallback), false);

  return true;
}

bool XRE_ShutdownTestShell() {
  if (!gContentParent) {
    return true;
  }
  bool ret = true;
  if (gContentParent->IsAlive()) {
    ret = gContentParent->DestroyTestShell(
        gContentParent->GetTestShellSingleton());
  }
  NS_RELEASE(gContentParent);
  return ret;
}

#ifdef MOZ_X11
void XRE_InstallX11ErrorHandler() {
#  ifdef MOZ_WIDGET_GTK
  InstallGdkErrorHandler();
#  endif

  // Ensure our X11 error handler overrides the default GDK error handler such
  // that errors are ignored by default. GDK will install its own error handler
  // temporarily when pushing error traps internally as needed. This avoids us
  // otherwise having to frequently override the error handler merely to trap
  // errors in multiple places that would otherwise contend with GDK or other
  // libraries that might also override the handler.
  InstallX11ErrorHandler();
}

void XRE_CleanupX11ErrorHandler() { CleanupX11ErrorHandler(); }
#endif

#ifdef MOZ_ENABLE_FORKSERVER
int XRE_ForkServer(int* aArgc, char*** aArgv) {
  return mozilla::ipc::ForkServer::RunForkServer(aArgc, aArgv) ? 1 : 0;
}
#endif
