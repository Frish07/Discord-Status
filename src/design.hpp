#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <map>
#include <stdexcept>
#include <string>

namespace design {
constexpr UINT32 Background = 0x0c1019, Panel = 0x131a27, Input = 0x0d1420;
constexpr UINT32 Border = 0x283348, Muted = 0x98a7bf, Text = 0xf0f4fc;
constexpr UINT32 Accent = 0x9184ff, Green = 0x64dfb6;
template<class T> void release(T*& item) { if (item) { item->Release(); item = nullptr; } }
inline D2D1_COLOR_F color(UINT32 hex) { return D2D1::ColorF(hex); }
struct Box { float x=0,y=0,w=0,h=0; };

// All geometry and DirectWrite font sizes use DIPs, never stretched bitmaps.
class Canvas {
    ID2D1Factory* factory = nullptr;
    IDWriteFactory* write = nullptr;
    ID2D1DCRenderTarget* target = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    std::map<int, IDWriteTextFormat*> formats;
public:
    Canvas() {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,&factory)) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&write))))
            throw std::runtime_error("Не удалось запустить DirectWrite / Direct2D.");
    }
    ~Canvas() { for (auto& pair : formats) release(pair.second); discard(); release(write); release(factory); }
    void discard() { release(brush); release(target); }
    bool begin(HDC dc, RECT bounds, UINT dpi) {
        if (!target) {
            auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE));
            if (FAILED(factory->CreateDCRenderTarget(&properties,&target))) return false;
            if (FAILED(target->CreateSolidColorBrush(color(Text),&brush))) { discard(); return false; }
        }
        if (FAILED(target->BindDC(dc,&bounds))) return false;
        target->SetDpi(static_cast<float>(dpi),static_cast<float>(dpi));
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        target->BeginDraw(); return true;
    }
    void end() { if (target->EndDraw() == D2DERR_RECREATE_TARGET) discard(); }
    void clear(UINT32 fill = Background) { target->Clear(color(fill)); }
    void offset(float y) { target->SetTransform(D2D1::Matrix3x2F::Translation(0,y)); }
    void box(Box b, UINT32 fill, float radius = 12, UINT32 stroke = 0) {
        auto rect = D2D1::RoundedRect(D2D1::RectF(b.x,b.y,b.x+b.w,b.y+b.h),radius,radius);
        brush->SetColor(color(fill)); target->FillRoundedRectangle(rect,brush);
        if (stroke) { brush->SetColor(color(stroke)); target->DrawRoundedRectangle(rect,brush,1); }
    }
    void dot(float x,float y,float radius,UINT32 fill) {
        brush->SetColor(color(fill)); target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),radius,radius),brush);
    }
    void line(float x,float y,float x2,float y2,UINT32 fill,float width=1) {
        brush->SetColor(color(fill)); target->DrawLine(D2D1::Point2F(x,y),D2D1::Point2F(x2,y2),brush,width);
    }
    void text(const std::wstring& text, Box b, float size=15, UINT32 fill=Text, bool bold=false,
              DWRITE_TEXT_ALIGNMENT align=DWRITE_TEXT_ALIGNMENT_LEADING, bool centered=false) {
        int key = static_cast<int>(size*10) + (bold ? 10000 : 0);
        auto& format = formats[key];
        if (!format) {
            if (FAILED(write->CreateTextFormat(L"Segoe UI",nullptr,bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"ru-RU",&format))) return;
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        }
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(write->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),format,b.w,b.h,&layout))) return;
        layout->SetTextAlignment(align);
        if (centered) layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};
        IDWriteInlineObject* ellipsis = nullptr;
        if (SUCCEEDED(write->CreateEllipsisTrimmingSign(format,&ellipsis))) { layout->SetTrimming(&trimming,ellipsis); release(ellipsis); }
        brush->SetColor(color(fill));
        target->DrawTextLayout(D2D1::Point2F(b.x,b.y),layout,brush,D2D1_DRAW_TEXT_OPTIONS_CLIP);
        release(layout);
    }
    void mark(float x,float y,float size) {
        box({x,y,size,size},0x7865e9,size*0.28f);
        const float u = size/48;
        box({x+12*u,y+12*u,24*u,15*u},0xffffff,5*u);
        box({x+12*u,y+20*u,7*u,16*u},0xffffff,2*u);
        box({x+19*u,y+17*u,10*u,5*u},0x7865e9,2*u);
        dot(x+size-2*u,y+size-2*u,7*u,Background);
        dot(x+size-2*u,y+size-2*u,4*u,Green);
    }
};
}
