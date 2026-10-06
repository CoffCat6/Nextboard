// nbcore 冒烟宿主(M3 验收):验证 DLL 加载、init、链路调用、panic 隔离与内存释放。
// 编译:cl /std:c++20 /EHsc smoke_host.cpp /link /LIBPATH:..\..\target\release nbcore.dll.lib
// 运行前把 nbcore.dll 放在本 exe 同目录。

#include <windows.h>
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

using NbInit = int(__cdecl*)(const char*);
using NbCall = int(__cdecl*)(const char*, const char*, char**);
using NbFreeString = void(__cdecl*)(char*);
using NbAbiVersion = unsigned(__cdecl*)();
using NbVersion = unsigned(__cdecl*)();
using NbShutdown = void(__cdecl*)();

static int g_failures = 0;

static void check(bool ok, const char* name, const std::string& detail = "") {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name;
    if (!detail.empty()) std::cout << " -- " << detail;
    std::cout << "\n";
    if (!ok) ++g_failures;
}

// 极简 JSON 字段提取(冒烟专用,不追求完备):找 "key":"value" 或 "key": value
static std::string json_find(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return "";
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        auto end = json.find('"', pos + 1);
        return end == std::string::npos ? "" : json.substr(pos + 1, end - pos - 1);
    }
    auto end = json.find_first_of(",}\r\n", pos);
    return end == std::string::npos ? "" : json.substr(pos, end - pos);
}

int main() {
    HMODULE dll = LoadLibraryA("nbcore.dll");
    if (!dll) {
        std::cout << "LoadLibraryA(nbcore.dll) failed: " << GetLastError() << "\n";
        return 1;
    }
    auto nb_abi = reinterpret_cast<NbAbiVersion>(GetProcAddress(dll, "nb_abi_version"));
    auto nb_version = reinterpret_cast<NbVersion>(GetProcAddress(dll, "nb_version"));
    auto nb_init = reinterpret_cast<NbInit>(GetProcAddress(dll, "nb_init"));
    auto nb_call = reinterpret_cast<NbCall>(GetProcAddress(dll, "nb_call"));
    auto nb_free = reinterpret_cast<NbFreeString>(GetProcAddress(dll, "nb_free_string"));
    auto nb_shutdown = reinterpret_cast<NbShutdown>(GetProcAddress(dll, "nb_shutdown"));
    if (!nb_abi || !nb_init || !nb_call || !nb_free || !nb_shutdown) {
        std::cout << "[FAIL] exports missing\n";
        return 1;
    }
    check(nb_abi() == 1, "nb_abi_version == 1");
    std::cout << "  nb_version = " << nb_version() << "\n";

    // 工作数据目录:临时目录。JSON 字符串里反斜杠必须转义,这里统一换成正斜杠
    fs::path dataDir = fs::temp_directory_path() / "nbcore-smoke";
    fs::create_directories(dataDir);
    std::string dir = (dataDir / "data").string();
    for (auto& c : dir) if (c == '\\') c = '/';
    std::string cfg = "{\"data_dir\":\"" + dir + "\",\"notify_hwnd\":0}";

    char* resp = nullptr;
    int rc = nb_init(cfg.c_str());
    check(rc == 0, "nb_init", rc == 0 ? "" : "rc=" + std::to_string(rc));

    // 1. 空历史列表
    rc = nb_call("list_entries", "{\"limit\":10,\"offset\":0}", &resp);
    check(rc == 0 && resp && json_find(resp, "total") == "0", "list_entries empty",
          resp ? resp : "no resp");
    nb_free(resp);

    // 2. 设置读写回环
    rc = nb_call("get_settings", nullptr, &resp);
    std::string theme = resp ? json_find(resp, "theme") : "";
    check(rc == 0 && theme == "system", "get_settings default theme=system", theme);
    nb_free(resp);

    // 3. 剪贴板文本写入 + 读回(真机系统剪贴板)
    rc = nb_call("copy_text_to_clipboard", "{\"text\":\"Nextboard smoke 测试\"}", &resp);
    check(rc == 0, "copy_text_to_clipboard", resp ? resp : "");
    nb_free(resp);
    rc = nb_call("read_clipboard_text", nullptr, &resp);
    std::string text = resp ? json_find(resp, "data") : "";
    check(rc == 0 && text.find("Nextboard smoke") != std::string::npos, "read_clipboard_text", text);
    nb_free(resp);

    // 4. 翻译历史(初始为空;字段顺序无关,只看 data 数组为空)
    rc = nb_call("translate_history", nullptr, &resp);
    check(rc == 0 && resp && std::string(resp).find("\"ok\":true") != std::string::npos &&
              std::string(resp).find("[]") != std::string::npos,
          "translate_history empty",
          resp ? resp : "");
    nb_free(resp);

    // 5. panic 隔离:非法参数必须返回错误码与 JSON 错误体,而非崩溃
    rc = nb_call("set_pinned", "{\"id\":\"no-such-id\",\"pinned\":true}", &resp);
    bool errBody = resp && std::string(resp).find("条目不存在") != std::string::npos;
    check(rc != 0 && errBody, "panic/error isolation (set_pinned missing id)",
          resp ? resp : "no resp");
    nb_free(resp);

    // 6. 未知方法
    rc = nb_call("no_such_method", nullptr, &resp);
    check(rc != 0 && resp && json_find(resp, "message").find("未知方法") != std::string::npos,
          "unknown method rejected", resp ? resp : "");
    nb_free(resp);

    // 7. OCR 探测(离线引擎,只验证调用不崩)
    rc = nb_call("ocr_probe", nullptr, &resp);
    check(rc == 0, "ocr_probe", resp ? resp : "");
    nb_free(resp);

    // 8. 关闭后调用应报"未初始化"
    nb_shutdown();
    rc = nb_call("get_settings", nullptr, &resp);
    check(rc != 0 && resp && json_find(resp, "message").find("未初始化") != std::string::npos,
          "nb_call after shutdown rejected", resp ? resp : "");
    nb_free(resp);

    FreeLibrary(dll);
    std::cout << (g_failures == 0 ? "\nALL SMOKE TESTS PASSED\n" : "\nFAILURES: " + std::to_string(g_failures) + "\n");
    return g_failures == 0 ? 0 : 2;
}
