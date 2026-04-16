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
    std::vector<uint8_t> temp( frameBytes );

    // 3 passes of box blur → approximate Gaussian.
    // Pass 1: original → blurred.
    HorizontalBoxBlur( bgraPixels, m_blurredFrame.data(), width, height, blurRadius );
    VerticalBoxBlur( m_blurredFrame.data(), temp.data(), width, height, blurRadius );
    // Pass 2.
    HorizontalBoxBlur( temp.data(), m_blurredFrame.data(), width, height, blurRadius );
    VerticalBoxBlur( m_blurredFrame.data(), temp.data(), width, height, blurRadius );
    // Pass 3.
    HorizontalBoxBlur( temp.data(), m_blurredFrame.data(), width, height, blurRadius );
    VerticalBoxBlur( m_blurredFrame.data(), temp.data(), width, height, blurRadius );

    // Blend: output = mask * original + (1 - mask) * blurred.
    // mask=1.0 means person (keep original), mask=0.0 means background (use blurred).
    for( uint32_t y = 0; y < height; y++ )
    {
        for( uint32_t x = 0; x < width; x++ )
        {
            size_t idx = ( static_cast<size_t>( y ) * width + x );
            float alpha = m_mask[idx]; // 1.0 = person, 0.0 = background

            size_t px = idx * 4;
            bgraPixels[px + 0] = static_cast<uint8_t>( bgraPixels[px + 0] * alpha + temp[px + 0] * ( 1.0f - alpha ) );
            bgraPixels[px + 1] = static_cast<uint8_t>( bgraPixels[px + 1] * alpha + temp[px + 1] * ( 1.0f - alpha ) );
            bgraPixels[px + 2] = static_cast<uint8_t>( bgraPixels[px + 2] * alpha + temp[px + 2] * ( 1.0f - alpha ) );
            // Alpha channel unchanged.
        }
    }
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

    if( !RunSegmentation( bgraPixels, width, height ) )
        return false;

    ApplyBlurWithMask( bgraPixels, width, height, blurRadius );
    return true;
}
