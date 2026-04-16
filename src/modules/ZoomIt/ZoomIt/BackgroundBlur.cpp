//==============================================================================
//
// BackgroundBlur.cpp
//
// ONNX Runtime-based person segmentation and background blur for the
// webcam overlay.  Uses a lightweight ONNX segmentation model (e.g.
// MediaPipe SelfieSegmentation or SINet) to produce a per-pixel person
// mask, then blurs the background using an iterated box blur (which
// approximates a Gaussian blur).
//
// Copyright (C) Mark Russinovich
// Sysinternals - www.sysinternals.com
//
//==============================================================================
#include "pch.h"
#include "BackgroundBlur.h"
#include <algorithm>
#include <cstring>
#include <wincodec.h>
#include <wil/com.h>

// Defined in Zoomit.cpp; compiles to nothing in Release builds.
void OutputDebug(const TCHAR* format, ...);

//----------------------------------------------------------------------------
// BackgroundBlur::BackgroundBlur
//----------------------------------------------------------------------------
BackgroundBlur::BackgroundBlur()
{
    m_ortApi = OrtGetApiBase()->GetApi( ORT_API_VERSION );
}

//----------------------------------------------------------------------------
// BackgroundBlur::~BackgroundBlur
//----------------------------------------------------------------------------
BackgroundBlur::~BackgroundBlur()
{
    if( m_session )       m_ortApi->ReleaseSession( m_session );
    if( m_sessionOptions ) m_ortApi->ReleaseSessionOptions( m_sessionOptions );
    if( m_memoryInfo )    m_ortApi->ReleaseMemoryInfo( m_memoryInfo );
    if( m_env )           m_ortApi->ReleaseEnv( m_env );
}

//----------------------------------------------------------------------------
// BackgroundBlur::Initialize
//
// Loads the ONNX segmentation model and inspects its input/output
// tensor shapes to auto-configure preprocessing.
//----------------------------------------------------------------------------
bool BackgroundBlur::Initialize( const wchar_t* modelPath )
{
    if( !m_ortApi )
    {
        OutputDebug( L"[BackgroundBlur] ONNX Runtime API not available\n" );
        return false;
    }

    OrtStatus* status = nullptr;

    // Create environment.
    status = m_ortApi->CreateEnv( ORT_LOGGING_LEVEL_WARNING, "BackgroundBlur", &m_env );
    if( status )
    {
        OutputDebug( L"[BackgroundBlur] CreateEnv failed: %S\n", m_ortApi->GetErrorMessage( status ) );
        m_ortApi->ReleaseStatus( status );
        return false;
    }

    // Create session options (CPU execution, single thread for low latency).
    status = m_ortApi->CreateSessionOptions( &m_sessionOptions );
    if( status )
    {
        OutputDebug( L"[BackgroundBlur] CreateSessionOptions failed\n" );
        m_ortApi->ReleaseStatus( status );
        return false;
    }
    m_ortApi->SetIntraOpNumThreads( m_sessionOptions, 2 );
    m_ortApi->SetSessionGraphOptimizationLevel( m_sessionOptions, ORT_ENABLE_ALL );

    // Load the model.
    status = m_ortApi->CreateSession( m_env, modelPath, m_sessionOptions, &m_session );
    if( status )
    {
        OutputDebug( L"[BackgroundBlur] CreateSession failed: %S\n", m_ortApi->GetErrorMessage( status ) );
        m_ortApi->ReleaseStatus( status );
        return false;
    }

    // Create memory info for CPU tensors.
    status = m_ortApi->CreateCpuMemoryInfo( OrtArenaAllocator, OrtMemTypeDefault, &m_memoryInfo );
    if( status )
    {
        OutputDebug( L"[BackgroundBlur] CreateCpuMemoryInfo failed\n" );
        m_ortApi->ReleaseStatus( status );
        return false;
    }

    // Get input tensor shape.
    {
        OrtAllocator* allocator = nullptr;
        m_ortApi->GetAllocatorWithDefaultOptions( &allocator );

        // Input name.
        char* inputNameRaw = nullptr;
        status = m_ortApi->SessionGetInputName( m_session, 0, allocator, &inputNameRaw );
        if( status )
        {
            OutputDebug( L"[BackgroundBlur] SessionGetInputName failed\n" );
            m_ortApi->ReleaseStatus( status );
            return false;
        }
        m_inputName = inputNameRaw;
        allocator->Free( allocator, inputNameRaw );

        // Output name.
        char* outputNameRaw = nullptr;
        status = m_ortApi->SessionGetOutputName( m_session, 0, allocator, &outputNameRaw );
        if( status )
        {
            OutputDebug( L"[BackgroundBlur] SessionGetOutputName failed\n" );
            m_ortApi->ReleaseStatus( status );
            return false;
        }
        m_outputName = outputNameRaw;
        allocator->Free( allocator, outputNameRaw );

        // Input shape.
        OrtTypeInfo* typeInfo = nullptr;
        m_ortApi->SessionGetInputTypeInfo( m_session, 0, &typeInfo );
        const OrtTensorTypeAndShapeInfo* tensorInfo = nullptr;
        m_ortApi->CastTypeInfoToTensorInfo( typeInfo, &tensorInfo );

        size_t dimCount = 0;
        m_ortApi->GetDimensionsCount( tensorInfo, &dimCount );
        std::vector<int64_t> dims( dimCount );
        m_ortApi->GetDimensions( tensorInfo, dims.data(), dimCount );

        // Detect layout: [1, C, H, W] (NCHW) vs [1, H, W, C] (NHWC).
        if( dimCount == 4 )
        {
            if( dims[1] == 3 || dims[1] == 1 )
            {
                // NCHW layout.
                m_inputIsNchw = true;
                m_modelInputChannels = dims[1];
                m_modelInputHeight = dims[2] > 0 ? dims[2] : 256;
                m_modelInputWidth = dims[3] > 0 ? dims[3] : 256;
            }
            else
            {
                // NHWC layout.
                m_inputIsNchw = false;
                m_modelInputHeight = dims[1] > 0 ? dims[1] : 256;
                m_modelInputWidth = dims[2] > 0 ? dims[2] : 256;
                m_modelInputChannels = dims[3];
            }
        }

        m_ortApi->ReleaseTypeInfo( typeInfo );
    }

    OutputDebug( L"[BackgroundBlur] Model loaded: input=%S %lldx%lld (ch=%lld, %s)\n",
                 m_inputName.c_str(), m_modelInputWidth, m_modelInputHeight,
                 m_modelInputChannels, m_inputIsNchw ? L"NCHW" : L"NHWC" );

    // Pre-allocate buffers.
    m_inputTensor.resize( static_cast<size_t>( m_modelInputChannels * m_modelInputHeight * m_modelInputWidth ) );

    return true;
}

//----------------------------------------------------------------------------
// BackgroundBlur::RunSegmentation
//
// Resizes the BGRA frame to the model's expected input size, converts
// to float RGB, runs inference, and produces a float mask in m_mask
// where 1.0 = person, 0.0 = background.
//----------------------------------------------------------------------------
bool BackgroundBlur::RunSegmentation( const uint8_t* bgraPixels, uint32_t width, uint32_t height )
{
    const int64_t mW = m_modelInputWidth;
    const int64_t mH = m_modelInputHeight;
    const int64_t mC = m_modelInputChannels;

    // Resize BGRA → model-sized float RGB using nearest-neighbor.
    // This is intentionally simple; the model is tolerant of basic scaling.
    for( int64_t y = 0; y < mH; y++ )
    {
        uint32_t srcY = static_cast<uint32_t>( y * height / mH );
        for( int64_t x = 0; x < mW; x++ )
        {
            uint32_t srcX = static_cast<uint32_t>( x * width / mW );
            const uint8_t* px = bgraPixels + ( static_cast<size_t>( srcY ) * width + srcX ) * 4;
            float b = px[0] / 255.0f;
            float g = px[1] / 255.0f;
            float r = px[2] / 255.0f;

            if( m_inputIsNchw )
            {
                // [1, C, H, W]
                m_inputTensor[0 * mH * mW + y * mW + x] = r;
                if( mC > 1 ) m_inputTensor[1 * mH * mW + y * mW + x] = g;
                if( mC > 2 ) m_inputTensor[2 * mH * mW + y * mW + x] = b;
            }
            else
            {
                // [1, H, W, C]
                size_t idx = static_cast<size_t>( y * mW + x ) * mC;
                m_inputTensor[idx + 0] = r;
                if( mC > 1 ) m_inputTensor[idx + 1] = g;
                if( mC > 2 ) m_inputTensor[idx + 2] = b;
            }
        }
    }

    // Create input tensor.
    int64_t inputShape[] = { 1, m_inputIsNchw ? mC : mH, m_inputIsNchw ? mH : mW, m_inputIsNchw ? mW : mC };
    OrtValue* inputTensor = nullptr;
    OrtStatus* status = m_ortApi->CreateTensorWithDataAsOrtValue(
        m_memoryInfo,
        m_inputTensor.data(),
        m_inputTensor.size() * sizeof( float ),
        inputShape, 4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        &inputTensor );
    if( status )
    {
        OutputDebug( L"[BackgroundBlur] CreateTensor failed: %S\n", m_ortApi->GetErrorMessage( status ) );
        m_ortApi->ReleaseStatus( status );
        return false;
    }

    // Run inference.
    const char* inputNames[] = { m_inputName.c_str() };
    const char* outputNames[] = { m_outputName.c_str() };
    OrtValue* outputTensor = nullptr;

    status = m_ortApi->Run( m_session, nullptr, inputNames, (const OrtValue* const*)&inputTensor, 1,
                            outputNames, 1, &outputTensor );
    m_ortApi->ReleaseValue( inputTensor );

    if( status )
    {
        OutputDebug( L"[BackgroundBlur] Run failed: %S\n", m_ortApi->GetErrorMessage( status ) );
        m_ortApi->ReleaseStatus( status );
        return false;
    }

    // Extract output mask.  The output is typically [1, 1, H, W] or [1, H, W, 1]
    // with values in [0, 1] where higher = person.
    float* outputData = nullptr;
    m_ortApi->GetTensorMutableData( outputTensor, reinterpret_cast<void**>( &outputData ) );

    OrtTensorTypeAndShapeInfo* outputInfo = nullptr;
    m_ortApi->GetTensorTypeAndShape( outputTensor, &outputInfo );
    size_t outputElementCount = 0;
    m_ortApi->GetTensorShapeElementCount( outputInfo, &outputElementCount );

    // Get output dimensions to determine if we need to handle multi-class output.
    size_t outDimCount = 0;
    m_ortApi->GetDimensionsCount( outputInfo, &outDimCount );
    std::vector<int64_t> outDims( outDimCount );
    m_ortApi->GetDimensions( outputInfo, outDims.data(), outDimCount );
    m_ortApi->ReleaseTensorTypeAndShapeInfo( outputInfo );

    // Determine output mask dimensions.
    int64_t outH = mH, outW = mW;
    int64_t numClasses = 1;
    if( outDimCount == 4 )
    {
        // Could be [1,1,H,W], [1,H,W,1], or [1,2,H,W] (2-class).
        if( outDims[1] <= 2 && outDims[2] > 2 )
        {
            // [1, classes, H, W]
            numClasses = outDims[1];
            outH = outDims[2];
            outW = outDims[3];
        }
        else
        {
            // [1, H, W, classes]
            outH = outDims[1];
            outW = outDims[2];
            numClasses = outDims[3];
        }
    }
    else if( outDimCount == 3 )
    {
        // [1, H, W]
        outH = outDims[1];
        outW = outDims[2];
    }

    // Resize output mask to frame dimensions.
    m_mask.resize( static_cast<size_t>( width ) * height );
    for( uint32_t y = 0; y < height; y++ )
    {
        int64_t srcY = static_cast<int64_t>( y ) * outH / height;
        for( uint32_t x = 0; x < width; x++ )
        {
            int64_t srcX = static_cast<int64_t>( x ) * outW / width;
            float personScore;

            if( numClasses == 1 )
            {
                // Single-channel output: value is person confidence.
                personScore = outputData[srcY * outW + srcX];
            }
            else
            {
                // Two-class output: [background, person].
                // Person is class 1.
                float bg = outputData[0 * outH * outW + srcY * outW + srcX];
                float fg = outputData[1 * outH * outW + srcY * outW + srcX];
                // Softmax.
                float maxVal = (std::max)( bg, fg );
                float expBg = expf( bg - maxVal );
                float expFg = expf( fg - maxVal );
                personScore = expFg / ( expBg + expFg );
            }

            // Clamp to [0, 1].
            personScore = (std::max)( 0.0f, (std::min)( 1.0f, personScore ) );
            m_mask[static_cast<size_t>( y ) * width + x] = personScore;
        }
    }

    m_ortApi->ReleaseValue( outputTensor );
    return true;
}

//----------------------------------------------------------------------------
// HorizontalBoxBlur / VerticalBoxBlur
//
// Separable box blur passes used to build an approximate Gaussian.
//----------------------------------------------------------------------------
static void HorizontalBoxBlur(
    const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height, int radius )
{
    const int diameter = radius * 2 + 1;
    for( uint32_t y = 0; y < height; y++ )
    {
        int rSum = 0, gSum = 0, bSum = 0;
        const uint8_t* row = src + static_cast<size_t>( y ) * width * 4;

        // Initialize window with clamped left edge.
        for( int i = -radius; i <= radius; i++ )
        {
            int ix = (std::max)( 0, (std::min)( static_cast<int>( width ) - 1, i ) );
            const uint8_t* px = row + ix * 4;
            bSum += px[0];
            gSum += px[1];
            rSum += px[2];
        }

        uint8_t* dstRow = dst + static_cast<size_t>( y ) * width * 4;
        for( uint32_t x = 0; x < width; x++ )
        {
            dstRow[x * 4 + 0] = static_cast<uint8_t>( bSum / diameter );
            dstRow[x * 4 + 1] = static_cast<uint8_t>( gSum / diameter );
            dstRow[x * 4 + 2] = static_cast<uint8_t>( rSum / diameter );
            dstRow[x * 4 + 3] = 0xFF;

            // Slide window: add right, remove left.
            int removeX = (std::max)( 0, static_cast<int>( x ) - radius );
            int addX = (std::min)( static_cast<int>( width ) - 1, static_cast<int>( x ) + radius + 1 );
            const uint8_t* remPx = row + removeX * 4;
            const uint8_t* addPx = row + addX * 4;
            bSum += addPx[0] - remPx[0];
            gSum += addPx[1] - remPx[1];
            rSum += addPx[2] - remPx[2];
        }
    }
}

static void VerticalBoxBlur(
    const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height, int radius )
{
    const int diameter = radius * 2 + 1;
    for( uint32_t x = 0; x < width; x++ )
    {
        int rSum = 0, gSum = 0, bSum = 0;

        // Initialize window with clamped top edge.
        for( int i = -radius; i <= radius; i++ )
        {
            int iy = (std::max)( 0, (std::min)( static_cast<int>( height ) - 1, i ) );
            const uint8_t* px = src + ( static_cast<size_t>( iy ) * width + x ) * 4;
            bSum += px[0];
            gSum += px[1];
            rSum += px[2];
        }

        for( uint32_t y = 0; y < height; y++ )
        {
            uint8_t* dstPx = dst + ( static_cast<size_t>( y ) * width + x ) * 4;
            dstPx[0] = static_cast<uint8_t>( bSum / diameter );
            dstPx[1] = static_cast<uint8_t>( gSum / diameter );
            dstPx[2] = static_cast<uint8_t>( rSum / diameter );
            dstPx[3] = 0xFF;

            int removeY = (std::max)( 0, static_cast<int>( y ) - radius );
            int addY = (std::min)( static_cast<int>( height ) - 1, static_cast<int>( y ) + radius + 1 );
            const uint8_t* remPx = src + ( static_cast<size_t>( removeY ) * width + x ) * 4;
            const uint8_t* addPx = src + ( static_cast<size_t>( addY ) * width + x ) * 4;
            bSum += addPx[0] - remPx[0];
            gSum += addPx[1] - remPx[1];
            rSum += addPx[2] - remPx[2];
        }
    }
}

//----------------------------------------------------------------------------
// BackgroundBlur::ApplyBlurWithMask
//
// Creates a blurred copy of the frame using 3 passes of box blur
// (approximates Gaussian), then blends original and blurred based
// on the segmentation mask.
//----------------------------------------------------------------------------
void BackgroundBlur::ApplyBlurWithMask( uint8_t* bgraPixels, uint32_t width, uint32_t height, int blurRadius )
{
    const size_t frameBytes = static_cast<size_t>( width ) * height * 4;
    m_blurredFrame.resize( frameBytes );
    m_tempFrame.resize( frameBytes );

    // 2 passes of box blur → approximate Gaussian (fast enough for small overlays).
    HorizontalBoxBlur( bgraPixels, m_blurredFrame.data(), width, height, blurRadius );
    VerticalBoxBlur( m_blurredFrame.data(), m_tempFrame.data(), width, height, blurRadius );
    HorizontalBoxBlur( m_tempFrame.data(), m_blurredFrame.data(), width, height, blurRadius );
    VerticalBoxBlur( m_blurredFrame.data(), m_tempFrame.data(), width, height, blurRadius );

    // Blend: output = mask * original + (1 - mask) * blurred.
    // Use integer math (0-255) instead of float for speed.
    for( uint32_t y = 0; y < height; y++ )
    {
        for( uint32_t x = 0; x < width; x++ )
        {
            size_t idx = ( static_cast<size_t>( y ) * width + x );
            // Convert float mask [0..1] to integer [0..255] for fast blending.
            uint32_t alpha = static_cast<uint32_t>( m_mask[idx] * 255.0f + 0.5f );
            if( alpha > 255 ) alpha = 255;
            uint32_t invAlpha = 255 - alpha;

            size_t px = idx * 4;
            bgraPixels[px + 0] = static_cast<uint8_t>( ( bgraPixels[px + 0] * alpha + m_tempFrame[px + 0] * invAlpha ) / 255 );
            bgraPixels[px + 1] = static_cast<uint8_t>( ( bgraPixels[px + 1] * alpha + m_tempFrame[px + 1] * invAlpha ) / 255 );
            bgraPixels[px + 2] = static_cast<uint8_t>( ( bgraPixels[px + 2] * alpha + m_tempFrame[px + 2] * invAlpha ) / 255 );
        }
    }
}

//----------------------------------------------------------------------------
// BackgroundBlur::SetBackgroundImage
//
// Loads an image file via WIC and stores it as a BGRA pixel buffer.
//----------------------------------------------------------------------------
bool BackgroundBlur::SetBackgroundImage( const wchar_t* imagePath )
{
    m_bgImage.clear();
    m_bgImageWidth = 0;
    m_bgImageHeight = 0;
    m_scaledBgImage.clear();
    m_scaledBgW = 0;
    m_scaledBgH = 0;

    if( !imagePath || !*imagePath )
        return false;

    auto factory = wil::CoCreateInstance<IWICImagingFactory>( CLSID_WICImagingFactory );
    if( !factory )
        return false;

    wil::com_ptr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(
        imagePath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder );
    if( FAILED( hr ) )
    {
        OutputDebug( L"[BackgroundBlur] Failed to decode image: %s (hr=0x%08X)\n", imagePath, hr );
        return false;
    }

    wil::com_ptr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame( 0, &frame );
    if( FAILED( hr ) )
        return false;

    // Convert to BGRA 32bpp.
    wil::com_ptr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter( &converter );
    if( FAILED( hr ) )
        return false;

    hr = converter->Initialize(
        frame.get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom );
    if( FAILED( hr ) )
        return false;

    UINT w = 0, h = 0;
    converter->GetSize( &w, &h );
    if( w == 0 || h == 0 )
        return false;

    m_bgImage.resize( static_cast<size_t>( w ) * h * 4 );
    hr = converter->CopyPixels( nullptr, w * 4, static_cast<UINT>( m_bgImage.size() ), m_bgImage.data() );
    if( FAILED( hr ) )
    {
        m_bgImage.clear();
        return false;
    }

    m_bgImageWidth = w;
    m_bgImageHeight = h;

    OutputDebug( L"[BackgroundBlur] Background image loaded: %ux%u from %s\n", w, h, imagePath );
    return true;
}

//----------------------------------------------------------------------------
// BackgroundBlur::EnsureScaledBgImage
//
// Scales the loaded background image to the specified dimensions using
// nearest-neighbor.  The result is cached and only recomputed when the
// target dimensions change.  The image is center-cropped to preserve
// aspect ratio (like "cover" scaling).
//----------------------------------------------------------------------------
void BackgroundBlur::EnsureScaledBgImage( uint32_t width, uint32_t height )
{
    if( m_scaledBgW == width && m_scaledBgH == height && !m_scaledBgImage.empty() )
        return;

    m_scaledBgImage.resize( static_cast<size_t>( width ) * height * 4 );
    m_scaledBgW = width;
    m_scaledBgH = height;

    // Compute center-crop of the source image to match the target aspect ratio.
    double targetAspect = static_cast<double>( width ) / height;
    double srcAspect = static_cast<double>( m_bgImageWidth ) / m_bgImageHeight;

    uint32_t cropW, cropH, cropX, cropY;
    if( srcAspect > targetAspect )
    {
        // Source is wider — crop horizontally.
        cropH = m_bgImageHeight;
        cropW = static_cast<uint32_t>( m_bgImageHeight * targetAspect + 0.5 );
        cropX = ( m_bgImageWidth - cropW ) / 2;
        cropY = 0;
    }
    else
    {
        // Source is taller — crop vertically.
        cropW = m_bgImageWidth;
        cropH = static_cast<uint32_t>( m_bgImageWidth / targetAspect + 0.5 );
        cropX = 0;
        cropY = ( m_bgImageHeight - cropH ) / 2;
    }

    for( uint32_t y = 0; y < height; y++ )
    {
        uint32_t srcY = cropY + y * cropH / height;
        for( uint32_t x = 0; x < width; x++ )
        {
            uint32_t srcX = cropX + x * cropW / width;
            size_t srcIdx = ( static_cast<size_t>( srcY ) * m_bgImageWidth + srcX ) * 4;
            size_t dstIdx = ( static_cast<size_t>( y ) * width + x ) * 4;
            m_scaledBgImage[dstIdx + 0] = m_bgImage[srcIdx + 0];
            m_scaledBgImage[dstIdx + 1] = m_bgImage[srcIdx + 1];
            m_scaledBgImage[dstIdx + 2] = m_bgImage[srcIdx + 2];
            m_scaledBgImage[dstIdx + 3] = 0xFF;
        }
    }
}

//----------------------------------------------------------------------------
// BackgroundBlur::ApplyImageWithMask
//
// Replaces background pixels with the loaded background image using the
// segmentation mask.  Person pixels are preserved, background pixels come
// from the scaled image.
//----------------------------------------------------------------------------
void BackgroundBlur::ApplyImageWithMask( uint8_t* bgraPixels, uint32_t width, uint32_t height )
{
    EnsureScaledBgImage( width, height );

    const uint8_t* bgData = m_scaledBgImage.data();

    for( uint32_t y = 0; y < height; y++ )
    {
        for( uint32_t x = 0; x < width; x++ )
        {
            size_t idx = static_cast<size_t>( y ) * width + x;
            uint32_t alpha = static_cast<uint32_t>( m_mask[idx] * 255.0f + 0.5f );
            if( alpha > 255 ) alpha = 255;
            uint32_t invAlpha = 255 - alpha;

            size_t px = idx * 4;
            bgraPixels[px + 0] = static_cast<uint8_t>( ( bgraPixels[px + 0] * alpha + bgData[px + 0] * invAlpha ) / 255 );
            bgraPixels[px + 1] = static_cast<uint8_t>( ( bgraPixels[px + 1] * alpha + bgData[px + 1] * invAlpha ) / 255 );
            bgraPixels[px + 2] = static_cast<uint8_t>( ( bgraPixels[px + 2] * alpha + bgData[px + 2] * invAlpha ) / 255 );
        }
    }
}

//----------------------------------------------------------------------------
// BackgroundBlur::ApplyImageReplacement
//
// Main entry point for background image replacement mode.
//----------------------------------------------------------------------------
bool BackgroundBlur::ApplyImageReplacement( uint8_t* bgraPixels, uint32_t width, uint32_t height )
{
    if( !m_session || !bgraPixels || width == 0 || height == 0 )
        return false;

    if( m_bgImage.empty() )
        return false;

    bool needInference = !m_hasCachedMask
        || m_lastMaskWidth != width
        || m_lastMaskHeight != height
        || ( m_frameCounter % m_inferenceInterval ) == 0;

    if( needInference )
    {
        if( !RunSegmentation( bgraPixels, width, height ) )
            return false;
        m_lastMaskWidth = width;
        m_lastMaskHeight = height;
        m_hasCachedMask = true;
    }
    m_frameCounter++;

    ApplyImageWithMask( bgraPixels, width, height );
    return true;
}

//----------------------------------------------------------------------------
// BackgroundBlur::Apply
//
// Main entry point: runs segmentation and applies blur to the background.
//----------------------------------------------------------------------------
bool BackgroundBlur::Apply( uint8_t* bgraPixels, uint32_t width, uint32_t height, int blurRadius )
{
    if( !m_session || !bgraPixels || width == 0 || height == 0 )
        return false;

    // Only run the expensive ONNX inference every N frames.
    // Reuse the cached mask for intermediate frames — the person
    // doesn't move much between frames at 30fps.
    bool needInference = !m_hasCachedMask
        || m_lastMaskWidth != width
        || m_lastMaskHeight != height
        || ( m_frameCounter % m_inferenceInterval ) == 0;

    if( needInference )
    {
        if( !RunSegmentation( bgraPixels, width, height ) )
            return false;
        m_lastMaskWidth = width;
        m_lastMaskHeight = height;
        m_hasCachedMask = true;
    }
    m_frameCounter++;

    ApplyBlurWithMask( bgraPixels, width, height, blurRadius );
    return true;
}
