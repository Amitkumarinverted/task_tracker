// Inverted TaskFlow Server - Windows service support
//   TaskFlowServer.exe --install-service     (run as Administrator; starts automatically with Windows)
//   TaskFlowServer.exe --uninstall-service
// The service runs as NT AUTHORITY\LocalService from the folder that holds TaskFlowServer.exe and server.ini.
#ifdef _WIN32
#include <windows.h>

#include <functional>
#include <iostream>
#include <string>

namespace tf {

static const wchar_t* kSvcName = L"InvertedTaskFlow";
static SERVICE_STATUS_HANDLE g_status_handle = nullptr;
static SERVICE_STATUS g_status{};
static std::function<int()> g_run;
static std::function<void()> g_stop;

static void set_state(DWORD state, DWORD exit_code = 0) {
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = state;
  g_status.dwControlsAccepted = state == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
  g_status.dwWin32ExitCode = exit_code ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
  g_status.dwServiceSpecificExitCode = exit_code;
  g_status.dwWaitHint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : 30000;
  if (g_status_handle) SetServiceStatus(g_status_handle, &g_status);
}

static DWORD WINAPI ctrl_handler(DWORD ctrl, DWORD, LPVOID, LPVOID) {
  if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
    set_state(SERVICE_STOP_PENDING);
    if (g_stop) g_stop();
    return NO_ERROR;
  }
  if (ctrl == SERVICE_CONTROL_INTERROGATE) return NO_ERROR;
  return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI service_main(DWORD, LPWSTR*) {
  g_status_handle = RegisterServiceCtrlHandlerExW(kSvcName, ctrl_handler, nullptr);
  if (!g_status_handle) return;
  set_state(SERVICE_START_PENDING);
  set_state(SERVICE_RUNNING);
  int rc = g_run ? g_run() : 1;
  set_state(SERVICE_STOPPED, (DWORD)rc);
}

int run_as_service(std::function<int()> run, std::function<void()> stop) {
  g_run = std::move(run);
  g_stop = std::move(stop);
  SERVICE_TABLE_ENTRYW table[] = {{(LPWSTR)kSvcName, service_main}, {nullptr, nullptr}};
  if (!StartServiceCtrlDispatcherW(table)) {
    std::cerr << "  --service is used by Windows only. To install the service run:  TaskFlowServer.exe --install-service\n";
    return 1;
  }
  return 0;
}

static std::wstring exe_path() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
  return std::wstring(buf, n);
}

int install_service() {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (!scm) {
    std::cerr << "  Cannot open the service manager - run this command in an Administrator command prompt.\n";
    return 1;
  }
  std::wstring cmd = L"\"" + exe_path() + L"\" --service";
  SC_HANDLE svc = CreateServiceW(scm, kSvcName, L"Inverted TaskFlow Server", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                 SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(), nullptr, nullptr, nullptr,
                                 L"NT AUTHORITY\\LocalService", nullptr);
  if (!svc) {
    DWORD e = GetLastError();
    CloseServiceHandle(scm);
    if (e == ERROR_SERVICE_EXISTS) {
      std::cout << "  The service is already installed.\n";
      return 0;
    }
    std::cerr << "  Could not create the service (error " << e << ").\n";
    return 1;
  }
  SERVICE_DESCRIPTIONW desc{(LPWSTR)L"Projects, tasks and timelines for Inverted Energy - API server for the desktop app."};
  ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
  // restart automatically after a crash: 10 s, 30 s, 60 s
  SC_ACTION actions[3] = {{SC_ACTION_RESTART, 10000}, {SC_ACTION_RESTART, 30000}, {SC_ACTION_RESTART, 60000}};
  SERVICE_FAILURE_ACTIONSW fa{};
  fa.dwResetPeriod = 86400;
  fa.cActions = 3;
  fa.lpsaActions = actions;
  ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
  SERVICE_DELAYED_AUTO_START_INFO delayed{TRUE};
  ChangeServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  std::cout << "  Service 'InvertedTaskFlow' installed (automatic start, restarts itself after a failure).\n";
  return 0;
}

int uninstall_service() {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) {
    std::cerr << "  Cannot open the service manager - run this command in an Administrator command prompt.\n";
    return 1;
  }
  SC_HANDLE svc = OpenServiceW(scm, kSvcName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
  if (!svc) {
    CloseServiceHandle(scm);
    std::cout << "  The service is not installed.\n";
    return 0;
  }
  SERVICE_STATUS st{};
  ControlService(svc, SERVICE_CONTROL_STOP, &st);
  for (int i = 0; i < 60; i++) {
    if (!QueryServiceStatus(svc, &st) || st.dwCurrentState == SERVICE_STOPPED) break;
    Sleep(500);
  }
  BOOL ok = DeleteService(svc);
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  std::cout << (ok ? "  Service removed.\n" : "  Could not remove the service.\n");
  return ok ? 0 : 1;
}

}  // namespace tf
#endif
