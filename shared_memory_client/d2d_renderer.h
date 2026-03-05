#pragma once
// ═══════════════════════════════════════════════════════════════
//  Direct2D / DirectWrite Overlay Renderer
//  Hardware-accelerated transparent overlay via D3D11 + DComposition
//  Replaces GDI/GDI+ rendering for the ESP overlay
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

    // ── DComposition ──
    ComPtr<IDCompositionDevice>  dcompDevice;
    ComPtr<IDCompositionTarget>  dcompTarget;
    ComPtr<IDCompositionVisual>  dcompVisual;

    // ── Reusable brush ──
    ComPtr<ID2D1SolidColorBrush> brush;

    // ── Cached stroke styles (avoid per-frame COM object creation) ──
    ComPtr<ID2D1StrokeStyle> styleRoundCap;
    ComPtr<ID2D1StrokeStyle> styleDashed;

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
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
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
        // MUST use grayscale — ClearType requires opaque background and hangs on premultiplied alpha surfaces
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        // 9. Reusable brush
        dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush);

        // 9b. Cache stroke styles (creating these per-frame causes GPU driver stalls)
        D2D1_STROKE_STYLE_PROPERTIES sspRound = {};
        sspRound.startCap = D2D1_CAP_STYLE_ROUND;
        sspRound.endCap = D2D1_CAP_STYLE_ROUND;
        d2dFactory->CreateStrokeStyle(sspRound, nullptr, 0, &styleRoundCap);

        D2D1_STROKE_STYLE_PROPERTIES sspDash = {};
        sspDash.dashStyle = D2D1_DASH_STYLE_DASH;
        d2dFactory->CreateStrokeStyle(sspDash, nullptr, 0, &styleDashed);

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
    //  FRAME MANAGEMENT
    // ════════════════════════════════════════

    bool BeginFrame() {
        if (!initialized) return false;
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0)); // fully transparent
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
        swapChain->Present1(1, 0, &pp); // vsync — prevents GPU queue saturation
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
        dc->DrawLine({ x1, y1 }, { x2, y2 }, brush.Get(), w, styleRoundCap.Get());
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

    // ── Gradient Rounded Rectangle (approximated with solid fill to avoid COM alloc) ──
    void GradientRoundRect(float x, float y, float w, float h, float r,
        COLORREF top, COLORREF bot, COLORREF stroke = 0, float sw = 0,
        float topA = 1.0f, float botA = 1.0f)
    {
        auto rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r);
        // Use averaged color instead of per-call gradient COM objects
        int mr = (GetRValue(top) + GetRValue(bot)) / 2;
        int mg = (GetGValue(top) + GetGValue(bot)) / 2;
        int mb = (GetBValue(top) + GetBValue(bot)) / 2;
        float ma = (topA + botA) / 2.0f;
        SetBrush(RGB(mr, mg, mb), ma);
        dc->FillRoundedRectangle(rr, brush.Get());

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
        dc->DrawEllipse(D2D1::Ellipse({ cx, cy }, rx, ry), brush.Get(), sw, styleDashed.Get());
    }

    // ── Pill (capsule shape) ──
    void Pill(float x, float y, float w, float h, COLORREF fill, float a = 1.0f) {
        float r = h / 2.0f;
        SetBrush(fill, a);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), brush.Get());
    }

    // ── Glow effect: single expanded rounded rect (lightweight) ──
    void Glow(float x, float y, float w, float h, float r, COLORREF c, int spread = 6, float peakAlpha = 0.12f) {
        // Single outer glow ring instead of multi-pass — avoids GPU overdraw stalls
        float ex = x - (float)spread, ey = y - (float)spread;
        float ew = w + (float)spread * 2, eh = h + (float)spread * 2;
        float er = r + (float)spread;
        SetBrush(c, peakAlpha * 0.6f);
        dc->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(ex, ey, ex + ew, ey + eh), er, er),
            brush.Get(), 2.0f);
    }

    // ── Radial glow at a point (single translucent circle) ──
    void RadialGlow(float cx, float cy, float r, COLORREF c, float peakAlpha = 0.2f) {
        // Single filled circle instead of concentric rings — avoids GPU overdraw stalls
        SetBrush(c, peakAlpha * 0.5f);
        dc->FillEllipse(D2D1::Ellipse({ cx, cy }, r, r), brush.Get());
    }

    // ── Gradient fills (use cached brush, only recreate when colors change) ──
    ComPtr<ID2D1LinearGradientBrush> cachedGradH;
    COLORREF cachedGradH_L = 0, cachedGradH_R = 0;
    ComPtr<ID2D1LinearGradientBrush> cachedGradV;
    COLORREF cachedGradV_T = 0, cachedGradV_B = 0;

    void GradientH(float x, float y, float w, float h, COLORREF left, COLORREF right) {
        if (!cachedGradH || cachedGradH_L != left || cachedGradH_R != right) {
            cachedGradH.Reset();
            D2D1_GRADIENT_STOP stops[2] = { {0.0f, Col(left)}, {1.0f, Col(right)} };
            ComPtr<ID2D1GradientStopCollection> sc;
            dc->CreateGradientStopCollection(stops, 2, &sc);
            dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties({ 0, 0 }, { 1, 0 }), sc.Get(), &cachedGradH);
            cachedGradH_L = left; cachedGradH_R = right;
        }
        if (cachedGradH) {
            cachedGradH->SetStartPoint({ x, y });
            cachedGradH->SetEndPoint({ x + w, y });
            dc->FillRectangle(D2D1::RectF(x, y, x + w, y + h), cachedGradH.Get());
        }
    }

    void GradientV(float x, float y, float w, float h, COLORREF top, COLORREF bot) {
        if (!cachedGradV || cachedGradV_T != top || cachedGradV_B != bot) {
            cachedGradV.Reset();
            D2D1_GRADIENT_STOP stops[2] = { {0.0f, Col(top)}, {1.0f, Col(bot)} };
            ComPtr<ID2D1GradientStopCollection> sc;
            dc->CreateGradientStopCollection(stops, 2, &sc);
            dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties({ 0, 0 }, { 0, 1 }), sc.Get(), &cachedGradV);
            cachedGradV_T = top; cachedGradV_B = bot;
        }
        if (cachedGradV) {
            cachedGradV->SetStartPoint({ x, y });
            cachedGradV->SetEndPoint({ x, y + h });
            dc->FillRectangle(D2D1::RectF(x, y, x + w, y + h), cachedGradV.Get());
        }
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
};
