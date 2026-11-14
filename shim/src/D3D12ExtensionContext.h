/*****************************************************************************\

Copyright (C) 2026 Intel Corporation

SPDX-License-Identifier: MIT

File Name:  D3D12ExtensionContext.h

Abstract:   D3D12 Extension Context

Notes:      Modified for xess-xmx-linux. In dxvk-igdext this context is a
            stub: it reports no supported version and never initializes.
            Here it is created for XeSS, XeSS-FG and XeLL, and keeps the
            application's device and the granted version for the
            _INTC_D3D12_* functions (dll/D3D12Api.cpp).

\*****************************************************************************/

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
