﻿#include "sdk.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <process.h>
#include <sstream>
#include <thread>

#include "framework.h"
#include <tlhelp32.h>

#include "injector.h"
#include "util.h"

static bool injected    = false;
static HANDLE wcProcess = NULL;
static HMODULE spyBase  = NULL;
static std::string spyDllPath;

constexpr std::string_view WCFSDKDLL            = "sdk.dll";
constexpr std::string_view WCFSPYDLL            = "spy.dll";
constexpr std::string_view WCFSPYDLL_DEBUG      = "spy_debug.dll";
constexpr std::string_view DISCLAIMER_FLAG      = ".license_accepted.flag";
constexpr std::string_view DISCLAIMER_TEXT_FILE = "DISCLAIMER.md";

static std::optional<std::string> read_disclaimer_text(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt; // 文件打开失败
    }

    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

static bool show_disclaimer()
{
    if (std::filesystem::exists(DISCLAIMER_FLAG)) {
        return true;
    }

    auto disclaimerTextOpt = read_disclaimer_text(std::string(DISCLAIMER_TEXT_FILE));
    if (!disclaimerTextOpt || disclaimerTextOpt->empty()) {
        MessageBoxA(NULL, "免责声明文件为空或读取失败。", "错误", MB_ICONERROR);
        return false;
    }

    int result
        = MessageBoxA(NULL, disclaimerTextOpt->c_str(), "免责声明", MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2);
    if (result == IDCANCEL) {
        MessageBoxA(NULL, "您拒绝了免责声明，程序将退出。", "提示", MB_ICONINFORMATION);
        return false;
    }

    std::ofstream flagFile(std::string(DISCLAIMER_FLAG), std::ios::out | std::ios::trunc);
    if (!flagFile) {
        MessageBoxA(NULL, "无法创建协议标志文件。", "错误", MB_ICONERROR);
        return false;
    }
    flagFile << "User accepted the license agreement.";

    return true;
}

static std::string get_dll_path(bool debug)
{
    char buffer[MAX_PATH] = { 0 };
    GetModuleFileNameA(GetModuleHandleA(WCFSDKDLL), buffer, MAX_PATH);

    std::filesystem::path path(buffer);
    path.remove_filename(); // 只保留目录路径
    path /= debug ? WCFSPYDLL_DEBUG : WCFSPYDLL;

    if (!std::filesystem::exists(path)) {
        MessageBoxA(NULL, path.string().c_str(), "文件不存在", MB_ICONERROR);
        return "";
    }

    return path.string();
}

int WxInitSDK(bool debug, int port)
{
    if (!show_disclaimer()) {
        exit(-1); // 用户拒绝协议，退出程序
    }

    int status  = 0;
    DWORD wcPid = 0;

    spyDllPath = get_dll_path(debug);
    if (spyDllPath.empty()) {
        return ERROR_FILE_NOT_FOUND; // DLL 文件路径不存在
    }

    status = util::open_wechat(&wcPid);
    if (status != 0) {
        MessageBoxA(NULL, "打开微信失败", "WxInitSDK", 0);
        return status;
    }

    std::this_thread::sleep_for(std::chrono::seconds(2)); // 等待微信打开
    wcProcess = inject_dll(wcPid, spyDllPath, &spyBase);
    if (wcProcess == NULL) {
        MessageBoxA(NULL, "注入失败", "WxInitSDK", 0);
        return -1;
    }

    util::PortPath pp = { 0 };
    pp.port           = port;
    snprintf(pp.path, MAX_PATH, "%s", std::filesystem::current_path().string().c_str());

    if (!call_dll_func_ex(wcProcess, spyDllPath, spyBase, "InitSpy", (LPVOID)&pp, sizeof(PortPath_t), NULL)) {
        MessageBoxA(NULL, "初始化失败", "WxInitSDK", 0);
        return -1;
    }

    injected = true;
    return 0;
}

int WxDestroySDK()
{
    if (!injected) {
        return 1; // 未注入
    }

    if (!call_dll_func(wcProcess, spyDllPath, spyBase, "CleanupSpy", NULL)) {
        return -1;
    }

    if (!eject_dll(wcProcess, spyBase)) {
        return -2;
    }

    return 0;
}
