#pragma once

#include "MainPage.g.h"

using namespace Microsoft::WRL;
using namespace Windows::Graphics::Imaging;
using namespace Windows::UI::Xaml::Media::Imaging;

namespace VSISSample
{
    public ref class MainPage sealed
    {
    public:
        MainPage();
        void UpdatesNeeded();

    private:
        // DirectX рутина
        void CreateDeviceResources();
        void CreateDeviceIndependentResources();
        void HandleDeviceLost();

        // Создает VirtualSurfaceImageSource
        // и устанавлиет его в Image
        void CreateVSIS();

        // Рисует указанный регион
        void RenderRegion(const RECT& updateRect);

    private:
        float dpi;
        ComPtr<ID2D1Factory1> d2dFactory;
        ComPtr<ID2D1Device> d2dDevice;
        ComPtr<ID2D1DeviceContext> d2dDeviceContext;
        ComPtr<IDXGIDevice> dxgiDevice;

        // Наше изображение
        BitmapFrame^ bitmapFrame;
        // Ссылка на данный VirtualSurfaceImageSource
        VirtualSurfaceImageSource^ vsis;
        // Ссылка на IVirtualSurfaceImageSourceNative
        ComPtr<IVirtualSurfaceImageSourceNative> vsisNative;
    };
}

namespace DX
{
    inline void ThrowIfFailed(_In_ HRESULT hr)
    {
        if (FAILED(hr))
        {
            // Set a breakpoint on this line to catch DX API errors.
            throw Platform::Exception::CreateException(hr);
        }
    }
}