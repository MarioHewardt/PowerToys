//==============================================================================
//
// BackgroundBlur.h
//
// Performs person segmentation using ONNX Runtime and applies a Gaussian
// blur to the background of a BGRA webcam frame.  The segmentation model
// (SelfieSegmentation or similar) runs on CPU via ONNX Runtime's default
// execution provider, keeping the GPU free for the recording pipeline.
//
// Copyright (C) Mark Russinovich
// Sysinternals - www.sysinternals.com
//
//==============================================================================
#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <onnxruntime_c_api.h>

class BackgroundBlur
{
public:
    BackgroundBlur();
    ~BackgroundBlur();

    // Initialize the ONNX model.  modelPath must point to a valid .onnx
    // segmentation model file.  Returns true on success.
    bool Initialize( const wchar_t* modelPath );

    // Apply background blur to a BGRA pixel buffer in-place.
    // width/height are the frame dimensions.  blurRadius controls
    // the strength of the Gaussian blur (in pixels).
    // Returns true if segmentation + blur was applied successfully.
    bool Apply( uint8_t* bgraPixels, uint32_t width, uint32_t height, int blurRadius = 21 );

    // Returns true if the model has been loaded successfully.
    bool IsInitialized() const { return m_session != nullptr; }

private:
    // Run the segmentation model and produce a float mask [0..1] per pixel.
    bool RunSegmentation( const uint8_t* bgraPixels, uint32_t width, uint32_t height );

    // Apply box blur (iterated for Gaussian approximation) to bgraPixels
    // only where the mask indicates background.
    void ApplyBlurWithMask( uint8_t* bgraPixels, uint32_t width, uint32_t height, int blurRadius );

    const OrtApi*           m_ortApi = nullptr;
    OrtEnv*                 m_env = nullptr;
    OrtSessionOptions*      m_sessionOptions = nullptr;
    OrtSession*             m_session = nullptr;
    OrtMemoryInfo*          m_memoryInfo = nullptr;

    // Model metadata (detected from the loaded model).
    int64_t                 m_modelInputWidth = 256;
    int64_t                 m_modelInputHeight = 256;
    int64_t                 m_modelInputChannels = 3;
    std::string             m_inputName;
    std::string             m_outputName;

    // Reusable buffers to avoid per-frame allocations.
    std::vector<float>      m_inputTensor;      // RGB float [1,3,H,W] or [1,H,W,3]
    std::vector<float>      m_mask;             // Segmentation mask [width*height]
    std::vector<uint8_t>    m_blurredFrame;     // Temporary blurred copy
    std::vector<float>      m_resizedRgb;       // Resized frame for model input
    bool                    m_inputIsNchw = true; // true = [1,C,H,W], false = [1,H,W,C]
};
