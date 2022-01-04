#include <iostream>
#include <fstream>
#include <Windows.h>
#include <wrl/client.h>
#include <d3d11.h>

#include "exception.h"
#include "AppEncUtilsD3D11.h"

using namespace std;
using namespace Microsoft::WRL;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "DXGI.lib")
//#pragma comment(lib, "nvenc_lib/x64/nvencodeapi.lib")

// yuv file http://trace.eas.asu.edu/yuv/

bool readOneFrame(ifstream& fin, char* frame, int size)
{
    streamsize nread = fin.read(frame, size).gcount();
    if (nread != size) {
        cout << "should read: " << size << ", but read: " << nread << endl;
        return false;
    }
    return true;
}

int main()
{
    int yuvWidth = 352;
    int yuvHeight = 288;
    string inFilePath = "yuv/akiyo_cif_352x288.yuv";
    string outFilePath = "out.h264";

    ifstream fin(inFilePath, std::ios::in | std::ios::binary);
    if (fin.fail()) {
        cout << "open file: " << inFilePath << " failed" << endl;
        return 0;
    }

    ofstream fout(outFilePath, std::ios::out | std::ios::binary);
    if (fout.fail()) {
        cout << "open file: " << outFilePath << " failed" << endl;
        return 0;
    }

    try {
        NV_ENCODE_API_FUNCTION_LIST m_nvenc;

        // 版本校验
        uint32_t version = 0;
        uint32_t currentVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
        NVENC_API_CALL(NvEncodeAPIGetMaxSupportedVersion(&version));
        if (currentVersion > version) {
            NVENC_THROW_ERROR("Current Driver Version does not support this NvEncodeAPI version, please upgrade driver",
                NV_ENC_ERR_INVALID_VERSION);
        }

        // 1. 加载NVENCODE API
        m_nvenc = { NV_ENCODE_API_FUNCTION_LIST_VER };
        NVENC_API_CALL(NvEncodeAPICreateInstance(&m_nvenc));
        if (!m_nvenc.nvEncOpenEncodeSession) {
            NVENC_THROW_ERROR("EncodeAPI not found", NV_ENC_ERR_NO_ENCODE_DEVICE);
        }

        // 创建D3D设备
        ComPtr<ID3D11Device> pDevice;
        ComPtr<ID3D11DeviceContext> pContext;
        ComPtr<IDXGIFactory1> pFactory;
        ComPtr<IDXGIAdapter> pAdapter;

        ck(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)pFactory.GetAddressOf()));
        ck(pFactory->EnumAdapters(0, pAdapter.GetAddressOf()));

        ck(D3D11CreateDevice(pAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
            NULL, 0, D3D11_SDK_VERSION, pDevice.GetAddressOf(), NULL, pContext.GetAddressOf()));

        DXGI_ADAPTER_DESC adapterDesc;
        pAdapter->GetDesc(&adapterDesc);
        char szDesc[80];
        size_t retv;
        //wcstombs(szDesc, adapterDesc.Description, sizeof(szDesc));
        wcstombs_s(&retv, szDesc, sizeof(szDesc), adapterDesc.Description, sizeof(szDesc));
        std::cout << "GPU in use: " << szDesc << std::endl;

        // 创建纹理
        ComPtr<ID3D11Texture2D> pTexSysMem;
        D3D11_TEXTURE2D_DESC desc;
        ZeroMemory(&desc, sizeof(D3D11_TEXTURE2D_DESC));
        desc.Width = yuvWidth;
        desc.Height = yuvHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ck(pDevice->CreateTexture2D(&desc, NULL, pTexSysMem.GetAddressOf()));

        // 2. 打开编码会话
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS encodeSessionExParams = { 0 };
        encodeSessionExParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        encodeSessionExParams.device = (void*)pDevice.Get();
        encodeSessionExParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        encodeSessionExParams.apiVersion = NVENCAPI_VERSION;
        void* hEncoder = NULL;
        NVENC_API_CALL(m_nvenc.nvEncOpenEncodeSessionEx(&encodeSessionExParams, &hEncoder));

        cout << "open encoder session success" << endl;

        // 获取编码器支持的编码GUID
        uint32_t encodeGUIDCount;
        NVENC_API_CALL(m_nvenc.nvEncGetEncodeGUIDCount(hEncoder, &encodeGUIDCount));
        if (encodeGUIDCount < 1) {
            return 0;
        }
        GUID* guids = new GUID[encodeGUIDCount];
        uint32_t tmpCount;
        NVENC_API_CALL(m_nvenc.nvEncGetEncodeGUIDs(hEncoder, guids, encodeGUIDCount, &tmpCount));

        // 获取preset guid
        uint32_t encodePresetGUIDCount;
        NVENC_API_CALL(m_nvenc.nvEncGetEncodePresetCount(hEncoder, NV_ENC_CODEC_H264_GUID, &encodePresetGUIDCount));
        if (encodePresetGUIDCount < 1) {
            return 0;
        }
        GUID* presetGuids = new GUID[encodePresetGUIDCount];
        NVENC_API_CALL(m_nvenc.nvEncGetEncodePresetGUIDs(hEncoder, NV_ENC_CODEC_H264_GUID, presetGuids, encodePresetGUIDCount, &tmpCount));

        // 3. 初始化编码器
        NV_ENC_PRESET_CONFIG presetConfig = { NV_ENC_PRESET_CONFIG_VER, { NV_ENC_CONFIG_VER } };
        m_nvenc.nvEncGetEncodePresetConfig(hEncoder, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P3_GUID, &presetConfig);
        NV_ENC_CONFIG config = { NV_ENC_CONFIG_VER };
        memcpy(&config, &presetConfig.presetCfg, sizeof(config));
        config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        config.rcParams.averageBitRate = 25000;
        config.gopLength = 10;

        NV_ENC_INITIALIZE_PARAMS encoder_init_params = { NV_ENC_INITIALIZE_PARAMS_VER };
        encoder_init_params.encodeConfig = &config;        
        encoder_init_params.encodeGUID = NV_ENC_CODEC_H264_GUID;
        encoder_init_params.presetGUID = NV_ENC_PRESET_P3_GUID;
        encoder_init_params.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        encoder_init_params.encodeWidth = yuvWidth;
        encoder_init_params.encodeHeight = yuvHeight;
        encoder_init_params.darWidth = yuvWidth;
        encoder_init_params.darHeight = yuvHeight;
        encoder_init_params.frameRateNum = 30;  // 帧率：30
        encoder_init_params.frameRateDen = 1;
        encoder_init_params.enablePTD = 1;
        encoder_init_params.maxEncodeWidth = yuvWidth;
        encoder_init_params.maxEncodeHeight = yuvHeight;
        encoder_init_params.enableEncodeAsync = 0;  // 同步模式
        bool exConfig = false;
        if (exConfig) {
            NV_ENC_PRESET_CONFIG presetConfig = { NV_ENC_PRESET_CONFIG_VER, { NV_ENC_CONFIG_VER } };
            m_nvenc.nvEncGetEncodePresetConfigEx(hEncoder,
                NV_ENC_CODEC_H264_GUID,
                NV_ENC_PRESET_P3_GUID,
                NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
                &presetConfig);
            memcpy(encoder_init_params.encodeConfig, &presetConfig.presetCfg, sizeof(NV_ENC_CONFIG));
        }
        encoder_init_params.encodeConfig->encodeCodecConfig.h264Config.chromaFormatIDC = 1; // for yuv420 input
        NVENC_API_CALL(m_nvenc.nvEncInitializeEncoder(hEncoder, &encoder_init_params));

        // 创建输入纹理资源
        ID3D11Texture2D* pInputTextures = NULL;
        ZeroMemory(&desc, sizeof(D3D11_TEXTURE2D_DESC));
        desc.Width = yuvWidth;
        desc.Height = yuvHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        desc.CPUAccessFlags = 0;
        if (pDevice->CreateTexture2D(&desc, NULL, &pInputTextures) != S_OK) {
            NVENC_THROW_ERROR("Failed to create d3d11textures", NV_ENC_ERR_OUT_OF_MEMORY);
        }

        // 注册输入资源
        NV_ENC_REGISTER_RESOURCE registerResource = { NV_ENC_REGISTER_RESOURCE_VER };
        registerResource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        registerResource.resourceToRegister = pInputTextures;
        registerResource.width = yuvWidth;
        registerResource.height = yuvHeight;
        registerResource.pitch = 0;
        registerResource.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        registerResource.bufferUsage = NV_ENC_INPUT_IMAGE;
        registerResource.pInputFencePoint = NULL;
        registerResource.pOutputFencePoint = NULL;
        NVENC_API_CALL(m_nvenc.nvEncRegisterResource(hEncoder, &registerResource));

        // 4. 映射注册的输入资源
        NV_ENC_MAP_INPUT_RESOURCE mapResource = { NV_ENC_MAP_INPUT_RESOURCE_VER };
        mapResource.registeredResource = registerResource.registeredResource;
        NVENC_API_CALL(m_nvenc.nvEncMapInputResource(hEncoder, &mapResource));

        // 5. 创建输出比特流缓冲
        NV_ENC_CREATE_BITSTREAM_BUFFER BitstreamBuffer = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
        NVENC_API_CALL(m_nvenc.nvEncCreateBitstreamBuffer(hEncoder, &BitstreamBuffer));

        std::unique_ptr<RGBToNV12ConverterD3D11> pConverter;
        bool bForceNv12 = true;
        if (bForceNv12) {
            pConverter.reset(new RGBToNV12ConverterD3D11(pDevice.Get(), pContext.Get(), yuvWidth, yuvHeight));
        }

        int size = 0;
        if (bForceNv12) {
            size = yuvWidth* yuvHeight * 3 / 2;
        }
            
        char* frame = new char[size];
        int frameCount = 0;
        while (true) {
            // 读入一帧数据
            cout << "start to read " << frameCount << endl;
            if (!readOneFrame(fin, frame, size)) {                    
                break;
            }
            
            // 输入数据map到已注册的纹理
            D3D11_MAPPED_SUBRESOURCE map;
            ck(pContext->Map(pTexSysMem.Get(), D3D11CalcSubresource(0, 0, 1), D3D11_MAP_WRITE, 0, &map));
            if (bForceNv12) {
                memcpy((uint8_t*)map.pData, frame, size);
            } else {
                for (int y = 0; y < yuvHeight; y++) {
                    memcpy((uint8_t*)map.pData + y * map.RowPitch, frame + y * yuvWidth * 4, yuvWidth * 4);
                }
            }            
            pContext->Unmap(pTexSysMem.Get(), D3D11CalcSubresource(0, 0, 1));
            
            if (bForceNv12) {
                ID3D11Texture2D* pNV12Textyure = reinterpret_cast<ID3D11Texture2D*>(pInputTextures);
                pConverter->ConvertRGBToNV12(pTexSysMem.Get(), pNV12Textyure);
            } else {
                pContext->CopyResource(pInputTextures, pTexSysMem.Get());
            }

            // 编码一帧
            NV_ENC_PIC_PARAMS pic_params = { 0 };
            pic_params.version = NV_ENC_PIC_PARAMS_VER;
            pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
            pic_params.inputBuffer = mapResource.mappedResource;
            pic_params.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
            pic_params.inputWidth = yuvWidth;
            pic_params.inputHeight = yuvHeight;
            pic_params.outputBitstream = BitstreamBuffer.bitstreamBuffer;
            pic_params.inputTimeStamp = 0;
            NVENC_API_CALL(m_nvenc.nvEncEncodePicture(hEncoder, &pic_params));

            // 获取输出
            NV_ENC_LOCK_BITSTREAM lockBitstreamData = { NV_ENC_LOCK_BITSTREAM_VER };
            lockBitstreamData.outputBitstream = BitstreamBuffer.bitstreamBuffer;
            lockBitstreamData.doNotWait = 0;
            NVENC_API_CALL(m_nvenc.nvEncLockBitstream(hEncoder, &lockBitstreamData));

            unsigned char* out_data = NULL;
            int datasize = lockBitstreamData.bitstreamSizeInBytes;
            out_data = (unsigned char*)malloc(datasize);
            memcpy(out_data, lockBitstreamData.bitstreamBufferPtr, datasize);

            NVENC_API_CALL(m_nvenc.nvEncUnlockBitstream(hEncoder, lockBitstreamData.outputBitstream));

            frameCount++;

            fout.write(reinterpret_cast<char*>(out_data), datasize);
            free(out_data);
        }

        cout << "frameCount: " << frameCount << endl;
    }
    catch (const std::exception& ex) {
        std::cout << ex.what();
        exit(1);
    }

    return 0;
}
