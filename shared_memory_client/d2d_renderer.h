#pragma once
// ═══════════════════════════════════════════════════════════════
//  Direct2D / DirectWrite Overlay Renderer
//  Hardware-accelerated rendering via D3D11 + DComposition / SwapChain
//  Used for both the ESP overlay (DComp) and the menu window (HWND)
// ═══════════════════════════════════════════════════════════════

#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <string>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;

// ── D2DOverlay: manages the entire DirectX rendering pipeline ──
class D2DOverlay {
public:
    HWND hwnd = nullptr;
    int width = 1920, height = 1080;

    // ── D3D11 ──
    ComPtr<ID3D11Device>        d3dDevice;
    ComPtr<IDXGISwapChain1>     swapChain;

    // ── D2D1 ──
    ComPtr<ID2D1Factory1>       d2dFactory;
    ComPtr<ID2D1Device>         d2dDevice;
    ComPtr<ID2D1DeviceContext>   dc;
    ComPtr<ID2D1Bitmap1>        targetBitmap;

    // ── DirectWrite ──
    ComPtr<IDWriteFactory>      dwFactory;
    ComPtr<IDWriteTextFormat>   fontESP;        // 12pt, general ESP text
    ComPtr<IDWriteTextFormat>   fontItem;       // 14pt semibold, item names
    ComPtr<IDWriteTextFormat>   fontItemDist;   // 11pt, distances
    ComPtr<IDWriteTextFormat>   fontLandmark;   // 12pt, landmark names
    ComPtr<IDWriteTextFormat>   fontRadarLabel; // 11pt semibold, radar labels
    ComPtr<IDWriteTextFormat>   fontHitHeader;  // 14pt semibold, hit feed header
    ComPtr<IDWriteTextFormat>   fontHitBody;    // 11pt, hit feed body

    // ── Menu-specific fonts (created by InitForHwnd) ──
    ComPtr<IDWriteTextFormat>   fontMenu;       // 14pt Segoe UI, menu normal
    ComPtr<IDWriteTextFormat>   fontMenuBold;   // 14pt Segoe UI Semibold, menu bold
    ComPtr<IDWriteTextFormat>   fontMenuSmall;  // 11pt Segoe UI, menu small
    ComPtr<IDWriteTextFormat>   fontMenuTitle;  // 17pt Segoe UI Bold, page titles
    ComPtr<IDWriteTextFormat>   fontMenuSub;    // 12pt Segoe UI Semibold, card headers
    ComPtr<IDWriteTextFormat>   fontMenuHeader; // 22pt Segoe UI Light, brand header

    // ── DComposition ──
    ComPtr<IDCompositionDevice>  dcompDevice;
    ComPtr<IDCompositionTarget>  dcompTarget;
    ComPtr<IDCompositionVisual>  dcompVisual;

    // ── Reusable brush ──
    ComPtr<ID2D1SolidColorBrush> brush;

    bool initialized = false;

    // ════════════════════════════════════════
    //  INITIALIZATION
    // ════════════════════════════════════════

    bool CreateOverlayWindow(int w, int h) {
        width = w; height = h;
        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "DX_ESP_OVL";
        RegisterClassExA(&wc);

        // WS_EX_NOREDIRECTIONBITMAP: DWM won't allocate a surface, we provide our own via DComp
        // WS_EX_TRANSPARENT: click-through
        // WS_EX_TOPMOST: always on top
        hwnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW,
            "DX_ESP_OVL", "", WS_POPUP,
            0, 0, w, h, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) return false;
        return true;
    }

    bool Init() {
        HRESULT hr;

        // 1. D3D11 device with BGRA support (required for D2D interop)
        D3D_FEATURE_LEVEL featureLevel;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION, &d3dDevice, &featureLevel, nullptr);
        if (FAILED(hr)) return false;

        // 2. Get DXGI device
        ComPtr<IDXGIDevice> dxgiDevice;
        d3dDevice.As(&dxgiDevice);

        // 3. D2D1 factory
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1), nullptr, (void**)d2dFactory.GetAddressOf());
        if (FAILED(hr)) return false;

        // 4. D2D1 device from DXGI device
        hr = d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
        if (FAILED(hr)) return false;

        // 5. D2D1 device context
        hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        if (FAILED(hr)) return false;

        // 6. DXGI swap chain for composition (per-pixel alpha)
        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width = width;
        scd.Height = height;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

        ComPtr<IDXGIFactory2> dxgiFactory;
        ComPtr<IDXGIAdapter> dxgiAdapter;
        dxgiDevice->GetAdapter(&dxgiAdapter);
        dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&dxgiFactory);

        hr = dxgiFactory->CreateSwapChainForComposition(d3dDevice.Get(), &scd, nullptr, &swapChain);
        if (FAILED(hr)) return false;

        // 7. Create D2D bitmap target from swap chain back buffer
        if (!CreateTargetBitmap()) return false;

        // 8. Antialiasing settings
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

        // 9. Reusable brush
        dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush);

        // 10. DirectComposition for transparent overlay
        hr = DCompositionCreateDevice(dxgiDevice.Get(),
            __uuidof(IDCompositionDevice), (void**)&dcompDevice);
        if (FAILED(hr)) return false;

        hr = dcompDevice->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget);
        if (FAILED(hr)) return false;

        hr = dcompDevice->CreateVisual(&dcompVisual);
        if (FAILED(hr)) return false;

        dcompVisual->SetContent(swapChain.Get());
        dcompTarget->SetRoot(dcompVisual.Get());
        dcompDevice->Commit();

        // 11. DirectWrite factory
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), (IUnknown**)dwFactory.GetAddressOf());
        if (FAILED(hr)) return false;

        // 12. Create font formats
        CreateFonts();

        initialized = true;
        return true;
    }

    void Shutdown() {
        initialized = false;
        if (hwnd) { DestroyWindow(hwnd); hwnd = nullptr; }
        UnregisterClassA("DX_ESP_OVL", GetModuleHandleA(nullptr));
        // ComPtr handles release automatically
    }

    // ════════════════════════════════════════
    //  INIT FOR HWND (Menu window — D3D11 swap chain, no DComposition)
    //  Renders directly to an existing HWND via swap chain
    // ════════════════════════════════════════

    bool InitForHwnd(HWND targetHwnd, int w, int h) {
        hwnd = targetHwnd;
        width = w; height = h;
        HRESULT hr;

        // 1. D3D11 device with BGRA support
        D3D_FEATURE_LEVEL featureLevel;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION, &d3dDevice, &featureLevel, nullptr);
        if (FAILED(hr)) return false;

        // 2. DXGI device
        ComPtr<IDXGIDevice> dxgiDevice;
        d3dDevice.As(&dxgiDevice);

        // 3. D2D1 factory
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1), nullptr, (void**)d2dFactory.GetAddressOf());
        if (FAILED(hr)) return false;

        // 4. D2D1 device
        hr = d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
        if (FAILED(hr)) return false;

        // 5. D2D1 device context
        hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        if (FAILED(hr)) return false;

        // 6. Swap chain for HWND (opaque — no per-pixel alpha needed for menu)
        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width = width;
        scd.Height = height;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        ComPtr<IDXGIFactory2> dxgiFactory;
        ComPtr<IDXGIAdapter> dxgiAdapter;
        dxgiDevice->GetAdapter(&dxgiAdapter);
        dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&dxgiFactory);

        hr = dxgiFactory->CreateSwapChainForHwnd(d3dDevice.Get(), hwnd, &scd, nullptr, nullptr, &swapChain);
        if (FAILED(hr)) return false;

        // 7. Bitmap target from swap chain
        if (!CreateTargetBitmap()) return false;

        // 8. Rendering settings
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

        // 9. Reusable brush
        dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush);

        // 10. DirectWrite
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), (IUnknown**)dwFactory.GetAddressOf());
        if (FAILED(hr)) return false;

        // 11. Fonts (ESP + menu)
        CreateFonts();
        CreateMenuFonts();

        initialized = true;
        return true;
    }

    // ════════════════════════════════════════
    //  FRAME MANAGEMENT
    // ════════════════════════════════════════

    bool BeginFrame() {
        if (!initialized) return false;
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0)); // fully transparent
        return true;
    }

    // Begin frame with opaque background (for menu window)
    bool BeginFrameOpaque(COLORREF bg) {
        if (!initialized) return false;
        dc->BeginDraw();
        dc->Clear(Col(bg));
        return true;
    }

    void EndFrame() {
        HRESULT hr = dc->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            // Device lost — recreate
            CreateTargetBitmap();
            return;
        }
        DXGI_PRESENT_PARAMETERS pp = {};
        swapChain->Present1(0, 0, &pp);
    }

    // Present with vsync control (menu uses vsync=1 for smoothness)
    void Present(int vsync = 1) {
        HRESULT hr = dc->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            CreateTargetBitmap();
            return;
        }
        DXGI_PRESENT_PARAMETERS pp = {};
        swapChain->Present1(vsync, 0, &pp);
    }

    // Resize swap chain (call on WM_SIZE)
    void Resize(int newW, int newH) {
        if (!initialized || !swapChain) return;
        width = newW; height = newH;
        dc->SetTarget(nullptr);
        targetBitmap.Reset();
        swapChain->ResizeBuffers(0, newW, newH, DXGI_FORMAT_UNKNOWN, 0);
        CreateTargetBitmap();
    }

    // ════════════════════════════════════════
    //  COLOR CONVERSION
    // ════════════════════════════════════════

    static inline D2D1_COLOR_F Col(COLORREF c, float a = 1.0f) {
        return D2D1::ColorF(
            GetRValue(c) / 255.0f,
            GetGValue(c) / 255.0f,
            GetBValue(c) / 255.0f,
            a);
    }

    // Set brush color (reuses single brush)
    void SetBrush(COLORREF c, float a = 1.0f) {
        brush->SetColor(Col(c, a));
    }

    // ════════════════════════════════════════
    //  DRAWING PRIMITIVES
    // ════════════════════════════════════════

    // ── Lines ──
    void Line(float x1, float y1, float x2, float y2, COLORREF c, float w = 1.0f, float a = 1.0f) {
        SetBrush(c, a);
        dc->DrawLine({ x1, y1 }, { x2, y2 }, brush.Get(), w);
    }

    void LineRound(float x1, float y1, float x2, float y2, COLORREF c, float w = 1.0f, float a = 1.0f) {
        SetBrush(c, a);
        ComPtr<ID2D1StrokeStyle> style;
        D2D1_STROKE_STYLE_PROPERTIES ssp = {};
        ssp.startCap = D2D1_CAP_STYLE_ROUND;
        ssp.endCap = D2D1_CAP_STYLE_ROUND;
        d2dFactory->CreateStrokeStyle(ssp, nullptr, 0, &style);
        dc->DrawLine({ x1, y1 }, { x2, y2 }, brush.Get(), w, style.Get());
    }

    // ── Rectangles ──
    void FillRect(float x, float y, float w, float h, COLORREF c, float a = 1.0f) {
        SetBrush(c, a);
        dc->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush.Get());
    }

    void DrawRect(float x, float y, float w, float h, COLORREF stroke, float sw = 1.0f, float a = 1.0f) {
        SetBrush(stroke, a);
        dc->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), brush.Get(), sw);
    }

    // ── Rounded Rectangles ──
    void FillRoundRect(float x, float y, float w, float h, float r, COLORREF fill, float a = 1.0f) {
        SetBrush(fill, a);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), brush.Get());
    }

    void DrawRoundRect(float x, float y, float w, float h, float r, COLORREF stroke, float sw = 1.0f, float a = 1.0f) {
        SetBrush(stroke, a);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), brush.Get(), sw);
    }

    void RoundRect(float x, float y, float w, float h, float r, COLORREF fill, COLORREF stroke, float sw = 1.0f, float fa = 1.0f, float sa = 1.0f) {
        auto rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r);
        SetBrush(fill, fa);
        dc->FillRoundedRectangle(rr, brush.Get());
        if (sw > 0) {
            SetBrush(stroke, sa);
            dc->DrawRoundedRectangle(rr, brush.Get(), sw);
        }
    }

    // ── Gradient Rounded Rectangle ──
    void GradientRoundRect(float x, float y, float w, float h, float r,
        COLORREF top, COLORREF bot, COLORREF stroke = 0, float sw = 0,
        float topA = 1.0f, float botA = 1.0f)
    {
        auto rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r);

        D2D1_GRADIENT_STOP stops[2];
        stops[0] = { 0.0f, Col(top, topA) };
        stops[1] = { 1.0f, Col(bot, botA) };
        ComPtr<ID2D1GradientStopCollection> sc;
        dc->CreateGradientStopCollection(stops, 2, &sc);
        ComPtr<ID2D1LinearGradientBrush> gb;
        dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({ x, y }, { x, y + h }),
            sc.Get(), &gb);
        dc->FillRoundedRectangle(rr, gb.Get());

        if (sw > 0 && stroke) {
            SetBrush(stroke);
            dc->DrawRoundedRectangle(rr, brush.Get(), sw);
        }
    }

    // ── Circles / Ellipses ──
    void FillCircle(float cx, float cy, float r, COLORREF c, float a = 1.0f) {
        SetBrush(c, a);
        dc->FillEllipse(D2D1::Ellipse({ cx, cy }, r, r), brush.Get());
    }

    void DrawCircle(float cx, float cy, float r, COLORREF stroke, float sw = 1.0f, float a = 1.0f) {
        SetBrush(stroke, a);
        dc->DrawEllipse(D2D1::Ellipse({ cx, cy }, r, r), brush.Get(), sw);
    }

    void Circle(float cx, float cy, float r, COLORREF fill, COLORREF border = 0, float bw = 0, float a = 1.0f) {
        auto ell = D2D1::Ellipse({ cx, cy }, r, r);
        SetBrush(fill, a);
        dc->FillEllipse(ell, brush.Get());
        if (bw > 0) {
            SetBrush(border, a);
            dc->DrawEllipse(ell, brush.Get(), bw);
        }
    }

    void EllipseOutline(float cx, float cy, float rx, float ry, COLORREF c, float sw = 1.0f, float a = 1.0f) {
        SetBrush(c, a);
        dc->DrawEllipse(D2D1::Ellipse({ cx, cy }, rx, ry), brush.Get(), sw);
    }

    void EllipseFill(float cx, float cy, float rx, float ry, COLORREF c, float a = 1.0f) {
        SetBrush(c, a);
        dc->FillEllipse(D2D1::Ellipse({ cx, cy }, rx, ry), brush.Get());
    }

    void EllipseOutlineDashed(float cx, float cy, float rx, float ry, COLORREF c, float sw = 1.0f, float a = 1.0f) {
        SetBrush(c, a);
        D2D1_STROKE_STYLE_PROPERTIES ssp = {};
        ssp.dashStyle = D2D1_DASH_STYLE_DASH;
        ComPtr<ID2D1StrokeStyle> style;
        d2dFactory->CreateStrokeStyle(ssp, nullptr, 0, &style);
        dc->DrawEllipse(D2D1::Ellipse({ cx, cy }, rx, ry), brush.Get(), sw, style.Get());
    }

    // ── Pill (capsule shape) ──
    void Pill(float x, float y, float w, float h, COLORREF fill, float a = 1.0f) {
        float r = h / 2.0f;
        SetBrush(fill, a);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), brush.Get());
    }

    // ── Glow effect: expanding rounded rects with decreasing alpha ──
    void Glow(float x, float y, float w, float h, float r, COLORREF c, int spread = 6, float peakAlpha = 0.12f) {
        for (int i = spread; i > 0; i--) {
            float a = peakAlpha * (float)(spread - i + 1) / (float)spread;
            float ex = x - (float)i, ey = y - (float)i;
            float ew = w + (float)i * 2, eh = h + (float)i * 2;
            float er = r + (float)i;
            SetBrush(c, a);
            dc->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(ex, ey, ex + ew, ey + eh), er, er),
                brush.Get(), 1.0f);
        }
    }

    // ── Radial glow at a point ──
    void RadialGlow(float cx, float cy, float r, COLORREF c, float peakAlpha = 0.2f) {
        for (float i = r; i > 0; i -= 2.0f) {
            float a = peakAlpha * i / r;
            SetBrush(c, a);
            dc->FillEllipse(D2D1::Ellipse({ cx, cy }, i, i), brush.Get());
        }
    }

    // ── Gradient fills ──
    void GradientH(float x, float y, float w, float h, COLORREF left, COLORREF right) {
        D2D1_GRADIENT_STOP stops[2] = { {0.0f, Col(left)}, {1.0f, Col(right)} };
        ComPtr<ID2D1GradientStopCollection> sc;
        dc->CreateGradientStopCollection(stops, 2, &sc);
        ComPtr<ID2D1LinearGradientBrush> gb;
        dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({ x, y }, { x + w, y }), sc.Get(), &gb);
        dc->FillRectangle(D2D1::RectF(x, y, x + w, y + h), gb.Get());
    }

    void GradientV(float x, float y, float w, float h, COLORREF top, COLORREF bot) {
        D2D1_GRADIENT_STOP stops[2] = { {0.0f, Col(top)}, {1.0f, Col(bot)} };
        ComPtr<ID2D1GradientStopCollection> sc;
        dc->CreateGradientStopCollection(stops, 2, &sc);
        ComPtr<ID2D1LinearGradientBrush> gb;
        dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({ x, y }, { x, y + h }), sc.Get(), &gb);
        dc->FillRectangle(D2D1::RectF(x, y, x + w, y + h), gb.Get());
    }

    // ── Polygon ──
    void FillPolygon(D2D1_POINT_2F* pts, int count, COLORREF fill, float a = 1.0f) {
        if (count < 3) return;
        ComPtr<ID2D1PathGeometry> path;
        d2dFactory->CreatePathGeometry(&path);
        ComPtr<ID2D1GeometrySink> sink;
        path->Open(&sink);
        sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 1; i < count; i++) sink->AddLine(pts[i]);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        SetBrush(fill, a);
        dc->FillGeometry(path.Get(), brush.Get());
    }

    void DrawPolygon(D2D1_POINT_2F* pts, int count, COLORREF stroke, float sw = 1.0f, float a = 1.0f) {
        if (count < 3) return;
        ComPtr<ID2D1PathGeometry> path;
        d2dFactory->CreatePathGeometry(&path);
        ComPtr<ID2D1GeometrySink> sink;
        path->Open(&sink);
        sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
        for (int i = 1; i < count; i++) sink->AddLine(pts[i]);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        SetBrush(stroke, a);
        dc->DrawGeometry(path.Get(), brush.Get(), sw);
    }

    // Filled polygon with optional outline (combined for arrows etc.)
    void Polygon(D2D1_POINT_2F* pts, int count, COLORREF fill, COLORREF stroke = 0, float sw = 0, float a = 1.0f) {
        if (count < 3) return;
        ComPtr<ID2D1PathGeometry> path;
        d2dFactory->CreatePathGeometry(&path);
        ComPtr<ID2D1GeometrySink> sink;
        path->Open(&sink);
        sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 1; i < count; i++) sink->AddLine(pts[i]);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        SetBrush(fill, a);
        dc->FillGeometry(path.Get(), brush.Get());
        if (sw > 0) {
            SetBrush(stroke, a);
            dc->DrawGeometry(path.Get(), brush.Get(), sw);
        }
    }

    // ════════════════════════════════════════
    //  TEXT RENDERING
    // ════════════════════════════════════════

    void Text(const char* str, int len, float x, float y, COLORREF c, IDWriteTextFormat* font = nullptr, float a = 1.0f) {
        if (!str || len <= 0) return;
        if (!font) font = fontESP.Get();

        // Convert to wide string
        int wlen = MultiByteToWideChar(CP_ACP, 0, str, len, nullptr, 0);
        wchar_t wbuf[512];
        if (wlen > 511) wlen = 511;
        MultiByteToWideChar(CP_ACP, 0, str, len, wbuf, wlen);
        wbuf[wlen] = 0;

        SetBrush(c, a);
        dc->DrawText(wbuf, wlen, font, D2D1::RectF(x, y, x + 2000.0f, y + 200.0f), brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
    }

    void TextMeasure(const char* str, int len, IDWriteTextFormat* font, SIZE* out) {
        if (!str || len <= 0 || !out) { if (out) { out->cx = 0; out->cy = 0; } return; }
        if (!font) font = fontESP.Get();

        int wlen = MultiByteToWideChar(CP_ACP, 0, str, len, nullptr, 0);
        wchar_t wbuf[512];
        if (wlen > 511) wlen = 511;
        MultiByteToWideChar(CP_ACP, 0, str, len, wbuf, wlen);
        wbuf[wlen] = 0;

        ComPtr<IDWriteTextLayout> layout;
        dwFactory->CreateTextLayout(wbuf, wlen, font, 2000.0f, 200.0f, &layout);
        DWRITE_TEXT_METRICS metrics;
        layout->GetMetrics(&metrics);
        out->cx = (LONG)ceilf(metrics.widthIncludingTrailingWhitespace);
        out->cy = (LONG)ceilf(metrics.height);
    }

    // ── Text with drop shadow (outline-style) ──
    void TextShadow(const char* str, int len, float x, float y, COLORREF c, IDWriteTextFormat* font = nullptr, float a = 1.0f) {
        Text(str, len, x + 1, y + 1, RGB(0, 0, 0), font, a * 0.6f);
        Text(str, len, x, y, c, font, a);
    }

    // ── Text with word wrapping (for descriptions) — returns height used ──
    float TextWrapped(const char* str, float x, float y, float maxW, COLORREF c, IDWriteTextFormat* font = nullptr, float a = 1.0f) {
        if (!str) return 0;
        int len = (int)strlen(str);
        if (len <= 0) return 0;
        if (!font) font = fontMenu.Get() ? fontMenu.Get() : fontESP.Get();

        int wlen = MultiByteToWideChar(CP_ACP, 0, str, len, nullptr, 0);
        wchar_t wbuf[512];
        if (wlen > 511) wlen = 511;
        MultiByteToWideChar(CP_ACP, 0, str, len, wbuf, wlen);
        wbuf[wlen] = 0;

        ComPtr<IDWriteTextLayout> layout;
        dwFactory->CreateTextLayout(wbuf, wlen, font, maxW, 200.0f, &layout);
        DWRITE_TEXT_METRICS metrics;
        layout->GetMetrics(&metrics);

        SetBrush(c, a);
        dc->DrawTextLayout({ x, y }, layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
        return metrics.height;
    }

    // ── 3-stop vertical gradient (for header accent bars) ──
    void Gradient3V(float x, float y, float w, float h, COLORREF c1, COLORREF c2, COLORREF c3) {
        D2D1_GRADIENT_STOP stops[3] = {
            { 0.0f, Col(c1) }, { 0.5f, Col(c2) }, { 1.0f, Col(c3) }
        };
        ComPtr<ID2D1GradientStopCollection> sc;
        dc->CreateGradientStopCollection(stops, 3, &sc);
        ComPtr<ID2D1LinearGradientBrush> gb;
        dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({ x, y }, { x + w, y }), sc.Get(), &gb);
        dc->FillRectangle(D2D1::RectF(x, y, x + w, y + h), gb.Get());
    }

    // ── Neon line (line with soft glow halo) ──
    void NeonLine(float x1, float y1, float x2, float y2, COLORREF c, float w = 2.0f) {
        // Outer glow
        SetBrush(c, 0.08f);
        dc->DrawLine({ x1, y1 }, { x2, y2 }, brush.Get(), w + 8.0f);
        SetBrush(c, 0.15f);
        dc->DrawLine({ x1, y1 }, { x2, y2 }, brush.Get(), w + 4.0f);
        SetBrush(c, 0.3f);
        dc->DrawLine({ x1, y1 }, { x2, y2 }, brush.Get(), w + 2.0f);
        // Core
        SetBrush(c, 1.0f);
        dc->DrawLine({ x1, y1 }, { x2, y2 }, brush.Get(), w);
    }

    // ── Gradient horizontal line (accent bar) ──
    void GradientLine(float x1, float y, float x2, float h, COLORREF left, COLORREF right) {
        D2D1_GRADIENT_STOP stops[2] = { {0.0f, Col(left)}, {1.0f, Col(right)} };
        ComPtr<ID2D1GradientStopCollection> sc;
        dc->CreateGradientStopCollection(stops, 2, &sc);
        ComPtr<ID2D1LinearGradientBrush> gb;
        dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({ x1, y }, { x2, y }), sc.Get(), &gb);
        dc->FillRectangle(D2D1::RectF(x1, y, x2, y + h), gb.Get());
    }

    // ── Push/Pop axis-aligned clip ──
    void PushClip(float x, float y, float w, float h) {
        dc->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
    void PopClip() { dc->PopAxisAlignedClip(); }

private:
    bool CreateTargetBitmap() {
        ComPtr<IDXGISurface> dxgiSurface;
        HRESULT hr = swapChain->GetBuffer(0, __uuidof(IDXGISurface), (void**)&dxgiSurface);
        if (FAILED(hr)) return false;

        D2D1_BITMAP_PROPERTIES1 bmpProps = {};
        bmpProps.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bmpProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        bmpProps.dpiX = 96.0f;
        bmpProps.dpiY = 96.0f;
        bmpProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

        hr = dc->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), bmpProps, &targetBitmap);
        if (FAILED(hr)) return false;

        dc->SetTarget(targetBitmap.Get());
        return true;
    }

    void CreateFonts() {
        // ESP general text (12pt Segoe UI — upgrade from Times New Roman)
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &fontESP);

        // Item name (14pt Segoe UI Semibold)
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &fontItem);

        // Item distance (11pt Segoe UI)
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &fontItemDist);

        // Landmark (12pt Segoe UI)
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &fontLandmark);

        // Radar label (11pt Segoe UI Semibold)
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &fontRadarLabel);

        // Hit feed header (14pt Segoe UI Semibold)
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &fontHitHeader);

        // Hit feed body (11pt Segoe UI)
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &fontHitBody);

        // Set default alignment to top-left for all
        if (fontESP) fontESP->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        if (fontItem) fontItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        if (fontItemDist) fontItemDist->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        if (fontLandmark) fontLandmark->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        if (fontRadarLabel) fontRadarLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        if (fontHitHeader) fontHitHeader->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        if (fontHitBody) fontHitBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    void CreateMenuFonts() {
        // Menu normal (14pt Segoe UI)
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &fontMenu);

        // Menu bold (14pt Segoe UI Semibold)
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &fontMenuBold);

        // Menu small (11pt Segoe UI)
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &fontMenuSmall);

        // Menu title (17pt Segoe UI Bold)
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 17.0f, L"en-us", &fontMenuTitle);

        // Menu sub (12pt Segoe UI Semibold)
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &fontMenuSub);

        // Menu header/brand (22pt Segoe UI Light)
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_LIGHT, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 22.0f, L"en-us", &fontMenuHeader);

        // Set alignment
        IDWriteTextFormat* menuFonts[] = { fontMenu.Get(), fontMenuBold.Get(), fontMenuSmall.Get(),
            fontMenuTitle.Get(), fontMenuSub.Get(), fontMenuHeader.Get() };
        for (auto* f : menuFonts) {
            if (f) f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }
};
