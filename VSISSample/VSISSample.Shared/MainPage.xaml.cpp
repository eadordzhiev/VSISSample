#include "pch.h"
#include "MainPage.xaml.h"
#include <synchapi.h>

using namespace VSISSample;

using namespace concurrency;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::Graphics::Display;

_Use_decl_annotations_ VOID WINAPI Sleep(DWORD dwMilliseconds)
{
    static HANDLE singletonEvent = nullptr;

    HANDLE sleepEvent = singletonEvent;

    // Demand create the event.
    if (!sleepEvent)
    {
        sleepEvent = CreateEventEx(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);

        if (!sleepEvent)
            return;

        HANDLE previousEvent = InterlockedCompareExchangePointerRelease(&singletonEvent, sleepEvent, nullptr);

        if (previousEvent)
        {
            // Back out if multiple threads try to demand create at the same time.
            CloseHandle(sleepEvent);
            sleepEvent = previousEvent;
        }
    }

    // Emulate sleep by waiting with timeout on an event that is never signalled.
    WaitForSingleObjectEx(sleepEvent, dwMilliseconds, false);
}

class VSISCallback : public RuntimeClass < RuntimeClassFlags<ClassicCom>, IVirtualSurfaceUpdatesCallbackNative >
{
public:
    HRESULT RuntimeClassInitialize(_In_ WeakReference parameter)
    {
        reference = parameter;
        return S_OK;
    }

    IFACEMETHODIMP UpdatesNeeded()
    {
        // Приводим к MainPage^
        MainPage^ mainPage = reference.Resolve<MainPage>();

        // Если mainPage еще не удален
        if (mainPage != nullptr)
        {
            mainPage->UpdatesNeeded();
        }

        return S_OK;
    }

private:
    WeakReference reference;
};

MainPage::MainPage()
{
    InitializeComponent();

    // Получаем текущий DPI
    dpi = DisplayInformation::GetForCurrentView()->LogicalDpi;

    CreateDeviceIndependentResources();
    CreateDeviceResources();

    CreateVSIS();
}

void MainPage::CreateVSIS()
{
    create_task([]()
    {
        //return StorageFile::GetFileFromApplicationUriAsync(ref new Uri("ms-appx:///map.png"));
        return StorageFile::GetFileFromApplicationUriAsync(ref new Uri("ms-appx:///captain.jpg"));
    }).then([](StorageFile^ file)
    {
        return file->OpenReadAsync();
    }).then([](IRandomAccessStreamWithContentType^ stream)
    {
        return BitmapDecoder::CreateAsync(stream);
    }).then([](BitmapDecoder^ bitmapDecoder)
    {
        return bitmapDecoder->GetFrameAsync(0);
    }).then([this](BitmapFrame^ decodedFrame)
    {
        bitmapFrame = decodedFrame;

        // Создаем VirtualSurfaceImageSource
        // Прозрачность нам не нужна, поэтому isOpaque = false
        vsis = ref new VirtualSurfaceImageSource(bitmapFrame->PixelWidth, bitmapFrame->PixelHeight, false);

        // Приводим VirtualSurfaceImageSource к IVirtualSurfaceImageSourceNative
        DX::ThrowIfFailed(
            reinterpret_cast<IInspectable*>(vsis)->QueryInterface(IID_PPV_ARGS(&vsisNative))
            );

        // Устанавливаем DXGI устройство
        DX::ThrowIfFailed(
            vsisNative->SetDevice(dxgiDevice.Get())
            );

        // Создаем экземпляр VSISCallBack
        WeakReference parameter(this);
        ComPtr<VSISCallback> callback;
        DX::ThrowIfFailed(
            MakeAndInitialize<VSISCallback>(&callback, parameter)
            );

        // Регистрируем callback
        DX::ThrowIfFailed(
            vsisNative->RegisterForUpdatesNeeded(callback.Get())
            );

        // Устанавливаем размер изображения и Source
        double pixelsPerLP = 100 / dpi;
        image->Width = bitmapFrame->PixelWidth * pixelsPerLP;
        image->Height = bitmapFrame->PixelHeight * pixelsPerLP;
        image->Source = vsis;
    });
}

void MainPage::UpdatesNeeded()
{
    // Получаем количество перерисовываемых регионов
    DWORD rectCount;
    DX::ThrowIfFailed(
        vsisNative->GetUpdateRectCount(&rectCount)
        );

    // Получаем сами регионы
    std::unique_ptr<RECT[]> updateRects(new RECT[rectCount]);
    DX::ThrowIfFailed(
        vsisNative->GetUpdateRects(updateRects.get(), rectCount)
        );

    // Рисуем их
    for (ULONG i = 0; i < rectCount; ++i)
    {
        RenderRegion(updateRects[i]);
    }
}

void MainPage::RenderRegion(const RECT& updateRect)
{
    // Surface, куда мы будем рисовать
    ComPtr<IDXGISurface> dxgiSurface;
    // Смещение Surface'а
    POINT surfaceOffset = { 0 };

    HRESULT hr = vsisNative->BeginDraw(updateRect, &dxgiSurface, &surfaceOffset);

    if (SUCCEEDED(hr))
    {
        // Ширина и высота отрисовываемого региона в пикселях
        UINT regionWidth = updateRect.right - updateRect.left;
        UINT regionHeight = updateRect.bottom - updateRect.top;
        // Количество байт в строке
        UINT rowSize = regionWidth * 4;

        // BitmapBounds позволяет нам указать нужный для отрисовки регион
        BitmapBounds bounds;
        bounds.X = updateRect.left;
        bounds.Y = updateRect.top;
        bounds.Width = regionWidth;
        bounds.Height = regionHeight;

        // Обрезаем картинку с помощью BitmapTransform
        BitmapTransform^ bitmapTransform = ref new BitmapTransform();
        bitmapTransform->Bounds = bounds;

        // В Windows Runtime API декодирование изображений происходит асинхронно.
        // Для того, чтобы сделать все синхронно
        auto pixelDataTask = create_task([=]
        {
            return bitmapFrame->GetPixelDataAsync(
                BitmapPixelFormat::Bgra8,
                BitmapAlphaMode::Ignore,
                bitmapTransform,
                ExifOrientationMode::IgnoreExifOrientation,
                ColorManagementMode::DoNotColorManage);
        });

        // Приходится исполнять такие трюки...
        while (!pixelDataTask.is_done()) 
        {
            Sleep(1);
        }

        PixelDataProvider^ pixelDataProvider = pixelDataTask.get();
        auto pixelData = pixelDataProvider->DetachPixelData();

        // Создаем Bitmap с нашим куском картинки
        ComPtr<ID2D1Bitmap> bitmap;
        DX::ThrowIfFailed(
            d2dDeviceContext->CreateBitmap(
            D2D1::SizeU(regionWidth, regionHeight),
            pixelData->Data,
            rowSize,
            D2D1::BitmapProperties(
            D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_IGNORE
            )
            ),
            &bitmap)
            );

        // Обнуляем счетчик ссылок
        pixelData = nullptr;
        pixelDataProvider = nullptr;

        // Превращаем наш Surface в Bitmap, на который и будем рисовать
        ComPtr<ID2D1Bitmap1> targetBitmap;
        DX::ThrowIfFailed(
            d2dDeviceContext->CreateBitmapFromDxgiSurface(
            dxgiSurface.Get(),
            nullptr,
            &targetBitmap
            )
            );
        d2dDeviceContext->SetTarget(targetBitmap.Get());

        // Делаем перенос на surfaceOffset
        auto transform = D2D1::Matrix3x2F::Translation(
            static_cast<float>(surfaceOffset.x),
            static_cast<float>(surfaceOffset.y)
            );
        d2dDeviceContext->SetTransform(transform);

        // Рисуем Bitmap
        d2dDeviceContext->BeginDraw();
        d2dDeviceContext->DrawBitmap(bitmap.Get());
        DX::ThrowIfFailed(
            d2dDeviceContext->EndDraw()
            );

        // Подчищаем за собой
        d2dDeviceContext->SetTarget(nullptr);

        // Заканчиваем рисовать
        DX::ThrowIfFailed(
            vsisNative->EndDraw()
            );
    }
    else if ((hr == DXGI_ERROR_DEVICE_REMOVED) || (hr == DXGI_ERROR_DEVICE_RESET))
    {
        // Обрбатываем сброс устройства
        HandleDeviceLost();
        // Пытаемся опять отрисовать updateRect
        vsisNative->Invalidate(updateRect);
    }
    else
    {
        // Неизвестная ошибка
        DX::ThrowIfFailed(hr);
    }
}

// Create device independent resources
void MainPage::CreateDeviceIndependentResources()
{
    D2D1_FACTORY_OPTIONS options;
    ZeroMemory(&options, sizeof(D2D1_FACTORY_OPTIONS));

#if defined(_DEBUG)
    // If the project is in a debug build, enable Direct2D debugging via Direct2D SDK layer.
    // Enabling SDK debug layer can help catch coding mistakes such as invalid calls and
    // resource leaking that needs to be fixed during the development cycle.
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

    DX::ThrowIfFailed(
        D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1),
        &options,
        &d2dFactory
        )
        );
}

// These are the resources that depend on hardware.
void MainPage::CreateDeviceResources()
{
    // This flag adds support for surfaces with a different color channel ordering than the API default.
    // It is recommended usage, and is required for compatibility with Direct2D.
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    // This array defines the set of DirectX hardware feature levels this app will support.
    // Note the ordering should be preserved.
    D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1
    };

    // Create the D3D11 API device object, and get a corresponding context.
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    D3D_FEATURE_LEVEL featureLevel;
    DX::ThrowIfFailed(
        D3D11CreateDevice(
        nullptr,                    // specify null to use the default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        0,                          // leave as 0 unless software device
        creationFlags,              // optionally set debug and Direct2D compatibility flags
        featureLevels,              // list of feature levels this app can support
        ARRAYSIZE(featureLevels),   // number of entries in above list
        D3D11_SDK_VERSION,          // always set this to D3D11_SDK_VERSION for Modern style apps
        &d3dDevice,                 // returns the Direct3D device created
        &featureLevel,              // returns feature level of device created
        &d3dContext                 // returns the device immediate context
        )
        );

    // Obtain the underlying DXGI device of the Direct3D11.1 device.
    DX::ThrowIfFailed(
        d3dDevice.As(&dxgiDevice)
        );

    // Obtain the Direct2D device for 2-D rendering.
    DX::ThrowIfFailed(
        d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice)
        );

    // And get its corresponding device context object.
    DX::ThrowIfFailed(
        d2dDevice->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        &d2dDeviceContext
        )
        );

    // Since this device context will be used to draw content onto XAML surface image source,
    // it needs to operate as pixels. Setting pixel unit mode is a way to tell Direct2D to treat
    // the incoming coordinates and vectors, typically as DIPs, as in pixels.
    d2dDeviceContext->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

    // Despite treating incoming values as pixels, it is still very important to tell Direct2D
    // the logical DPI the application operates on. Direct2D uses the DPI value as a hint to
    // optimize internal rendering policy such as to determine when is appropriate to enable
    // symmetric text rendering modes. Not specifying the appropriate DPI in this case will hurt
    // application performance.
    d2dDeviceContext->SetDpi(dpi, dpi);

    // When an application performs animation or image composition of graphics content, it is important
    // to use Direct2D grayscale text rendering mode rather than ClearType. The ClearType technique
    // operates on the color channels and not the alpha channel, and therefore unable to correctly perform
    // image composition or sub-pixel animation of text. ClearType is still a method of choice when it
    // comes to direct rendering of text to the destination surface with no subsequent composition required.
    d2dDeviceContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
}

void MainPage::HandleDeviceLost()
{
    CreateDeviceIndependentResources();
}