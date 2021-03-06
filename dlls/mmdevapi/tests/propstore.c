/*
 * Copyright 2010 Maarten Lankhorst for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define NONAMELESSUNION
#include "wine/test.h"

#define COBJMACROS

#ifdef STANDALONE
#include "initguid.h"
#endif

#include "unknwn.h"
#include "uuids.h"
#include "mmdeviceapi.h"
#include "devpkey.h"

static void test_propertystore(IPropertyStore *store)
{
    HRESULT hr;
    PROPVARIANT pv;
    char temp[128];
    temp[sizeof(temp)-1] = 0;

    pv.vt = VT_EMPTY;
    hr = IPropertyStore_GetValue(store, &PKEY_AudioEndpoint_GUID, &pv);
    ok(hr == S_OK, "Failed with %08x\n", hr);
    ok(pv.vt == VT_LPWSTR, "Value should be %i, is %i\n", VT_LPWSTR, pv.vt);
    if (hr == S_OK && pv.vt == VT_LPWSTR)
    {
        WideCharToMultiByte(CP_ACP, 0, pv.u.pwszVal, -1, temp, sizeof(temp)-1, NULL, NULL);
        trace("guid: %s\n", temp);
        CoTaskMemFree(pv.u.pwszVal);
    }

    pv.vt = VT_EMPTY;
    hr = IPropertyStore_GetValue(store, (const PROPERTYKEY*)&DEVPKEY_DeviceInterface_FriendlyName, &pv);
    ok(hr == S_OK, "Failed with %08x\n", hr);
    ok(pv.vt == VT_EMPTY, "Key should not be found\n");

    pv.vt = VT_EMPTY;
    hr = IPropertyStore_GetValue(store, (const PROPERTYKEY*)&DEVPKEY_DeviceInterface_Enabled, &pv);
    ok(hr == S_OK, "Failed with %08x\n", hr);
    ok(pv.vt == VT_EMPTY, "Key should not be found\n");

    pv.vt = VT_EMPTY;
    hr = IPropertyStore_GetValue(store, (const PROPERTYKEY*)&DEVPKEY_DeviceInterface_ClassGuid, &pv);
    ok(hr == S_OK, "Failed with %08x\n", hr);
    ok(pv.vt == VT_EMPTY, "Key should not be found\n");
}

START_TEST(propstore)
{
    HRESULT hr;
    IMMDeviceEnumerator *mme = NULL;
    IMMDevice *dev = NULL;
    IPropertyStore *store;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator, (void**)&mme);
    if (FAILED(hr))
    {
        skip("mmdevapi not available: 0x%08x\n", hr);
        goto cleanup;
    }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(mme, eRender, eMultimedia, &dev);
    ok(hr == S_OK || hr == E_NOTFOUND, "GetDefaultAudioEndpoint failed: 0x%08x\n", hr);
    if (hr != S_OK)
    {
        if (hr == E_NOTFOUND)
            skip("No sound card available\n");
        else
            skip("GetDefaultAudioEndpoint returns 0x%08x\n", hr);
        goto cleanup;
    }
    store = NULL;
    hr = IMMDevice_OpenPropertyStore(dev, 3, &store);
    ok(hr == E_INVALIDARG, "Wrong hr returned: %08x\n", hr);
    if (hr != S_OK)
        /* It seems on windows returning with E_INVALIDARG doesn't
         * set store to NULL, so just don't set store to non-null
         * before calling this function
         */
        ok(!store, "Store set to non-NULL on failure: %p/%08x\n", store, hr);
    else if (store)
        IPropertyStore_Release(store);
    hr = IMMDevice_OpenPropertyStore(dev, STGM_READ, NULL);
    ok(hr == E_POINTER, "Wrong hr returned: %08x\n", hr);

    store = NULL;
    hr = IMMDevice_OpenPropertyStore(dev, STGM_READ, &store);
    ok(hr == S_OK, "Opening valid store returned %08x\n", hr);
    if (store)
    {
        test_propertystore(store);
        IPropertyStore_Release(store);
    }
    IMMDevice_Release(dev);
cleanup:
    if (mme)
        IUnknown_Release(mme);
    CoUninitialize();
}
