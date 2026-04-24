// Windows implementation of Safira::FileDialog — uses the modern
// IFileOpenDialog COM API (Windows Vista and later).

#include "FileDialog.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <string>

#pragma comment(lib, "Ole32.lib")

namespace {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace

namespace Safira {

std::optional<std::string> FileDialog::OpenImage() {
    return Open("Select Avatar Image", {
        { "Image Files", { "png", "jpg", "jpeg", "bmp", "gif", "webp" } }
    });
}

std::optional<std::string>
FileDialog::Open(const std::string& title,
                 const std::vector<FileDialogFilter>& filters) {
    using Microsoft::WRL::ComPtr;

    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool owns_com = SUCCEEDED(hrInit);

    std::optional<std::string> result;

    ComPtr<IFileOpenDialog> dialog;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog)))) {
        std::wstring wtitle = Utf8ToWide(title);
        dialog->SetTitle(wtitle.c_str());

        // Build COMDLG_FILTERSPEC array from filters.
        std::vector<std::wstring> storage; // keeps wide strings alive for pointers below
        std::vector<COMDLG_FILTERSPEC> specs;
        storage.reserve(filters.size() * 2);
        specs.reserve(filters.size());
        for (const auto& f : filters) {
            storage.emplace_back(Utf8ToWide(f.Name));
            std::wstring pattern;
            for (size_t i = 0; i < f.Extensions.size(); ++i) {
                if (i) pattern += L';';
                pattern += L"*.";
                pattern += Utf8ToWide(f.Extensions[i]);
            }
            storage.emplace_back(std::move(pattern));
            COMDLG_FILTERSPEC spec{};
            spec.pszName = storage[storage.size() - 2].c_str();
            spec.pszSpec = storage[storage.size() - 1].c_str();
            specs.push_back(spec);
        }
        if (!specs.empty()) {
            dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
            dialog->SetFileTypeIndex(1);
        }

        if (SUCCEEDED(dialog->Show(nullptr))) {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = WideToUtf8(path);
                    CoTaskMemFree(path);
                }
            }
        }
    }

    if (owns_com) CoUninitialize();
    return result;
}

} // namespace Safira

#endif // _WIN32
