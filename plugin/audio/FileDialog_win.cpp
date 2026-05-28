// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "FileDialog.h"

#include <windows.h>
#include <shobjidl.h>

namespace mc
{
namespace
{
std::string WideToUtf8(const wchar_t* w)
{
  if (!w) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string out(static_cast<size_t>(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
  return out;
}
} // namespace

std::vector<std::string> PromptForAudioFiles(void* nativeParent)
{
  std::vector<std::string> result;

  const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const bool comInitNeedsRelease = SUCCEEDED(comInit);

  IFileOpenDialog* dialog = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog));
  if (SUCCEEDED(hr) && dialog)
  {
    const COMDLG_FILTERSPEC filters[] = {
      { L"Audio Files", L"*.wav;*.flac;*.mp3;*.ogg;*.m4a;*.aac;*.mp4;*.ape" },
      { L"All Files (*.*)", L"*.*" },
    };
    dialog->SetFileTypes(2, filters);
    dialog->SetFileTypeIndex(1);

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST
                       | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_ALLOWMULTISELECT);

    hr = dialog->Show(static_cast<HWND>(nativeParent));
    if (SUCCEEDED(hr))
    {
      IShellItemArray* items = nullptr;
      if (SUCCEEDED(dialog->GetResults(&items)) && items)
      {
        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i)
        {
          IShellItem* item = nullptr;
          if (SUCCEEDED(items->GetItemAt(i, &item)) && item)
          {
            PWSTR pathW = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pathW)) && pathW)
            {
              result.push_back(WideToUtf8(pathW));
              CoTaskMemFree(pathW);
            }
            item->Release();
          }
        }
        items->Release();
      }
    }
    dialog->Release();
  }

  if (comInitNeedsRelease) CoUninitialize();
  return result;
}

namespace
{
bool runFileDialog(void* nativeParent, bool save, std::string& outPathUtf8)
{
  const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const bool comInitNeedsRelease = SUCCEEDED(comInit);
  bool ok = false;

  IFileDialog* dialog = nullptr;
  const CLSID clsid = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
  const IID   iid   = save ? IID_IFileSaveDialog  : IID_IFileOpenDialog;
  if (SUCCEEDED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid,
                                 reinterpret_cast<void**>(&dialog))) && dialog)
  {
    const COMDLG_FILTERSPEC filters[] = {
      { L"Playlist (*.m3u8)", L"*.m3u8" },
      { L"All Files (*.*)", L"*.*" },
    };
    dialog->SetFileTypes(2, filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"m3u8");
    if (save) dialog->SetFileName(L"playlist.m3u8");

    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    options |= save ? FOS_OVERWRITEPROMPT : (FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dialog->SetOptions(options);

    if (SUCCEEDED(dialog->Show(static_cast<HWND>(nativeParent))))
    {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item)) && item)
      {
        PWSTR pathW = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pathW)) && pathW)
        {
          outPathUtf8 = WideToUtf8(pathW);
          CoTaskMemFree(pathW);
          ok = !outPathUtf8.empty();
        }
        item->Release();
      }
    }
    dialog->Release();
  }
  if (comInitNeedsRelease) CoUninitialize();
  return ok;
}
} // namespace

bool PromptForSavePlaylist(void* nativeParent, std::string& outPathUtf8)
{
  return runFileDialog(nativeParent, true, outPathUtf8);
}

bool PromptForOpenPlaylist(void* nativeParent, std::string& outPathUtf8)
{
  return runFileDialog(nativeParent, false, outPathUtf8);
}

} // namespace mc
