#include "ui.hpp"
#include "diagnostics.hpp"
#include "startup_guard.hpp"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <shellapi.h>
#include <fstream>

using Microsoft::WRL::ComPtr;
static ComPtr<ID3D11Device> device;
static ComPtr<ID3D11DeviceContext> context;
static ComPtr<IDXGISwapChain> swapChain;
static ComPtr<ID3D11RenderTargetView> target;
static UINT resizeWidth=0,resizeHeight=0;
static aion::App* currentApp=nullptr;
static bool closeRequested=false;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
static LRESULT WINAPI windowProc(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(ImGui_ImplWin32_WndProcHandler(window,message,w,l)) return true;
    if(message==WM_SIZE) {if(w!=SIZE_MINIMIZED){resizeWidth=LOWORD(l);resizeHeight=HIWORD(l);} return 0;}
    if(message==WM_SYSCOMMAND && (w&0xfff0)==SC_KEYMENU) return 0;
    if(message==WM_DESTROY) {PostQuitMessage(0); return 0;}
    if(message==WM_CLOSE && currentApp && !currentApp->canClose()){closeRequested=true;ShowWindow(window,SW_RESTORE);return 0;}
    return DefWindowProcW(window,message,w,l);
}
static bool createTarget() {
    ComPtr<ID3D11Texture2D> back;
    if(FAILED(swapChain->GetBuffer(0,IID_PPV_ARGS(&back)))) return false;
    return SUCCEEDED(device->CreateRenderTargetView(back.Get(),nullptr,&target));
}
static bool screenshot() {
    ComPtr<ID3D11Texture2D> back,staging;
    if(FAILED(swapChain->GetBuffer(0,IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC desc{}; back->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
    if(FAILED(device->CreateTexture2D(&desc,nullptr,&staging))) return false;
    context->CopyResource(staging.Get(),back.Get()); D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return false;
    BITMAPFILEHEADER file{}; file.bfType=0x4d42; file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER); file.bfSize=file.bfOffBits+desc.Width*desc.Height*4;
    BITMAPINFOHEADER info{}; info.biSize=sizeof(info); info.biWidth=LONG(desc.Width); info.biHeight=-LONG(desc.Height); info.biPlanes=1; info.biBitCount=32; info.biCompression=BI_RGB;
    std::ofstream out("ui-smoke.bmp",std::ios::binary); out.write(reinterpret_cast<char*>(&file),sizeof(file)); out.write(reinterpret_cast<char*>(&info),sizeof(info));
    std::vector<uint8_t> row(desc.Width*4);
    for(UINT y=0;y<desc.Height;++y) {
        const auto* pixel=static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch;
        for(UINT x=0;x<desc.Width;++x) {row[x*4]=pixel[x*4+2];row[x*4+1]=pixel[x*4+1];row[x*4+2]=pixel[x*4];row[x*4+3]=255;}
        out.write(reinterpret_cast<char*>(row.data()),std::streamsize(row.size()));
    }
    context->Unmap(staging.Get(),0); out.close(); return bool(out);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR command,int) {
    // Own the singleton before starting diagnostics, proxy listeners or MQTT.
    struct InstanceGuard { HANDLE handle{}; ~InstanceGuard(){if(handle)CloseHandle(handle);} } guard;
    guard.handle=CreateMutexW(nullptr,FALSE,L"Local\\Aion2Pipe.Desktop.Instance");
    const auto instanceError=GetLastError();
    const auto existingWindow=FindWindowW(L"Aion2PipeWindow",nullptr);
    if(existingWindow || (guard.handle && instanceError==ERROR_ALREADY_EXISTS)) {
        if(existingWindow){ShowWindow(existingWindow,SW_RESTORE);SetForegroundWindow(existingWindow);}
        else MessageBoxW(nullptr,L"Aion2Pipe 已启动，正在初始化。请勿重复启动。",L"Aion2Pipe",MB_OK|MB_ICONINFORMATION);
        return 0;
    }
    if(!guard.handle){MessageBoxW(nullptr,L"无法取得程序运行锁，请检查是否已有 Aion2Pipe 正在运行。",L"Aion2Pipe",MB_OK|MB_ICONERROR);return 1;}
    if(const auto other=aion::otherPipeProcess()) {
        const auto message=L"检测到尚未退出的 Aion2Pipe（PID "+std::to_wstring(other)+L"）。请等待它退出后再启动。";
        MessageBoxW(nullptr,message.c_str(),L"Aion2Pipe",MB_OK|MB_ICONINFORMATION);return 0;
    }
    if(const auto bridge=aion::cleanOrphanChatBridges()) {
        const auto message=L"聊天组件仍由其他程序管理或无法清理（PID "+std::to_wstring(bridge)+L"），已阻止重复启动。";
        MessageBoxW(nullptr,message.c_str(),L"Aion2Pipe",MB_OK|MB_ICONINFORMATION);return 1;
    }
    aion::initializeDiagnostics();
    WSADATA wsa{};auto wsaResult=WSAStartup(MAKEWORD(2,2),&wsa);if(wsaResult){aion::diagnostics().write("startup_error","WSAStartup="+std::to_string(wsaResult));return 1;}
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{sizeof(wc),CS_CLASSDC,windowProc,0,0,instance,nullptr,nullptr,nullptr,nullptr,L"Aion2PipeWindow",nullptr};
    RegisterClassExW(&wc); HWND window=CreateWindowW(wc.lpszClassName,L"Aion2Pipe — 二进制协议分析",WS_OVERLAPPEDWINDOW,100,100,1440,960,nullptr,nullptr,instance,nullptr);
    DXGI_SWAP_CHAIN_DESC desc{}; desc.BufferCount=2; desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow=window; desc.SampleDesc.Count=1; desc.Windowed=TRUE; desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL level; const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0};
    auto create=[&](D3D_DRIVER_TYPE type){return D3D11CreateDeviceAndSwapChain(nullptr,type,nullptr,0,levels,2,D3D11_SDK_VERSION,&desc,&swapChain,&device,&level,&context);};
    auto hardwareResult=create(D3D_DRIVER_TYPE_HARDWARE);auto warpResult=S_OK;
    if(FAILED(hardwareResult))warpResult=create(D3D_DRIVER_TYPE_WARP);
    aion::diagnostics().write("graphics_init","hardware_hresult="+std::to_string(hardwareResult)+" warp_hresult="+std::to_string(warpResult));
    if(FAILED(hardwareResult) && FAILED(warpResult)) {MessageBoxW(window,L"无法初始化 Direct3D 11。",L"Aion2Pipe",MB_ICONERROR); return 2;}
    if(!createTarget()){aion::diagnostics().write("startup_error","createTarget failed");return 3;}
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); auto& io=ImGui::GetIO(); io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard; io.IniFilename=nullptr;
    aion::applyTheme();
    const float scale=ImGui_ImplWin32_GetDpiScaleForHwnd(window); ImGui::GetStyle().ScaleAllSizes(scale);
    wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows,MAX_PATH);
    auto font=std::filesystem::path(windows)/L"Fonts"/L"msyh.ttc";
    if(std::filesystem::exists(font)) io.Fonts->AddFontFromFileTTF(font.string().c_str(),17.0f*scale,nullptr,io.Fonts->GetGlyphRangesChineseFull());
    else io.Fonts->AddFontDefault();
    ImGui_ImplWin32_Init(window); ImGui_ImplDX11_Init(device.Get(),context.Get());
    const bool smoke=wcsstr(command,L"--smoke")!=nullptr;
    // Hidden smoke mode renders the actual native UI into a saved GPU backbuffer.
    if(!smoke) {ShowWindow(window,SW_SHOWDEFAULT); UpdateWindow(window);}
    int result=0;
    {
        aion::App app;currentApp=&app;
        if(!smoke && !wcsstr(command,L"--demo"))app.autoStart();
        if(smoke || wcsstr(command,L"--demo")) app.demo();
        int argc=0; auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        if(argv) {for(int i=1;i+1<argc;++i) if(wcscmp(argv[i],L"--open")==0) {app.open(argv[++i]);}}
        if(wcsstr(command,L"--smoke-frames")) app.smokeFrames();
        if(wcsstr(command,L"--smoke-protocol") || wcsstr(command,L"--protocol")) app.smokeProtocol();
        if(wcsstr(command,L"--smoke-proxy-tx")) app.smokeOutgoing();
        if(wcsstr(command,L"--smoke-login")) app.smokeProtocol(13700);
        if(wcsstr(command,L"--nearby")) app.showNearby();
        if(wcsstr(command,L"--guild-page")) app.showGuild();
        if(wcsstr(command,L"--queries")) app.showQueries();
        if(wcsstr(command,L"--debug-page")) app.showDebug();
        if(wcsstr(command,L"--report-page")) app.showReport();
        if(argv) {for(int i=1;i+1<argc;++i) if(wcscmp(argv[i],L"--state")==0) {app.openCipherState(argv[++i]);}LocalFree(argv);}
        if(wcsstr(command,L"--capture") || wcsstr(command,L"--proxy")) app.startCapture();
        bool done=false; int frame=0;uint64_t nextCloseCheck=0;
        while(!done) {
            MSG msg; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&msg); DispatchMessageW(&msg); if(msg.message==WM_QUIT) done=true;}
            if(done) break;
            if(closeRequested && GetTickCount64()>=nextCloseCheck){
                nextCloseCheck=GetTickCount64()+250;
                if(app.canClose()){DestroyWindow(window);continue;}
            }
            if(resizeWidth && resizeHeight) {
                context->OMSetRenderTargets(0,nullptr,nullptr); target.Reset();
                if(FAILED(swapChain->ResizeBuffers(0,resizeWidth,resizeHeight,DXGI_FORMAT_UNKNOWN,0)) || !createTarget()) {result=4; break;}
                resizeWidth=resizeHeight=0;
            }
            if(!smoke && IsIconic(window)) {app.pump();Sleep(30); continue;}
            ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame(); app.draw(); ImGui::Render();
            const float clear[]={.04f,.05f,.08f,1}; context->OMSetRenderTargets(1,target.GetAddressOf(),nullptr); context->ClearRenderTargetView(target.Get(),clear); ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if(smoke && ++frame==((wcsstr(command,L"--capture") || wcsstr(command,L"--proxy"))?120:4)) {result=screenshot()?0:5; break;}
            auto hr=swapChain->Present(1,0); if(hr==DXGI_ERROR_DEVICE_REMOVED || hr==DXGI_ERROR_DEVICE_RESET) {result=6; break;}
            if(smoke) Sleep(16);
        }
        currentApp=nullptr;
    }
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); target.Reset(); swapChain.Reset(); context.Reset(); device.Reset();
    DestroyWindow(window); UnregisterClassW(wc.lpszClassName,instance); WSACleanup();aion::diagnostics().write("exit","code="+std::to_string(result));return result;
}
