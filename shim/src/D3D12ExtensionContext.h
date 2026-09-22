#pragma once

struct D3D12ExtensionContext final : ExtensionContextBase
{
    D3D12ExtensionContext()                                        = default;
    D3D12ExtensionContext(const D3D12ExtensionContext&)            = delete;
    D3D12ExtensionContext& operator=(const D3D12ExtensionContext&) = delete;

    Microsoft::WRL::ComPtr<ID3D12Device> m_pAppDevice;
    INTCExtensionVersion                 m_SupportedExtVersion = {};

    HRESULT GetSupportedVersions(const void* pDevice, INTCExtensionVersionHelper* driverExtensionVersion);
    HRESULT InitExtensions(const void* pDevice, void** ppfnExtensionFuncs, UINT32 extensionFuncsSize, INTCExtensionInfo1* pExtensionInfo, INTCExtensionAppInfo1* pExtensionAppInfo, bool internalExtensions = false);
};
