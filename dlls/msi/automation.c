/*
 * Implementation of OLE Automation for Microsoft Installer (msi.dll)
 *
 * Copyright 2007 Misha Koshelev
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

#define COBJMACROS

#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winuser.h"
#include "winreg.h"
#include "msidefs.h"
#include "msipriv.h"
#include "activscp.h"
#include "oleauto.h"
#include "shlwapi.h"
#include "wine/debug.h"
#include "wine/unicode.h"

#include "msiserver.h"
#include "msiserver_dispids.h"

WINE_DEFAULT_DEBUG_CHANNEL(msi);

#define REG_INDEX_CLASSES_ROOT 0
#define REG_INDEX_DYN_DATA 6

/*
 * AutomationObject - "base" class for all automation objects. For each interface, we implement Invoke function
 *                    called from AutomationObject::Invoke, and pass this function to create_automation_object.
 */

typedef struct AutomationObject AutomationObject;

struct AutomationObject {
    /*
     * VTables - We provide IDispatch, IProvideClassInfo, IProvideClassInfo2, IProvideMultipleClassInfo
     */
    const IDispatchVtbl *lpVtbl;
    const IProvideMultipleClassInfoVtbl *lpvtblIProvideMultipleClassInfo;

    /* Object reference count */
    LONG ref;

    /* Clsid for this class and it's appropriate ITypeInfo object */
    LPCLSID clsid;
    ITypeInfo *iTypeInfo;

    /* The MSI handle of the current object */
    MSIHANDLE msiHandle;

    /* A function that is called from AutomationObject::Invoke, specific to this type of object. */
    HRESULT (*funcInvoke)(
        AutomationObject* This,
        DISPID dispIdMember,
        REFIID riid,
        LCID lcid,
        WORD wFlags,
        DISPPARAMS* pDispParams,
        VARIANT* pVarResult,
        EXCEPINFO* pExcepInfo,
        UINT* puArgErr);

    /* A function that is called from AutomationObject::Release when the object is being freed to free any private
     * data structures (or NULL) */
    void (*funcFree)(AutomationObject* This);
};

/*
 * ListEnumerator - IEnumVARIANT implementation for MSI automation lists.
 */

typedef struct {
    IEnumVARIANT IEnumVARIANT_iface;
    LONG ref;

    /* Current position and pointer to AutomationObject that stores actual data */
    ULONG ulPos;
    AutomationObject *pObj;
} ListEnumerator;

/*
 * Structures for additional data required by specific automation objects
 */

typedef struct {
    ULONG ulCount;
    VARIANT *pVars;
} ListData;

typedef struct {
    /* The parent Installer object */
    IDispatch *pInstaller;
} SessionData;

/* VTables */
static const struct IDispatchVtbl AutomationObject_Vtbl;
static const struct IProvideMultipleClassInfoVtbl AutomationObject_IProvideMultipleClassInfo_Vtbl;
static const struct IEnumVARIANTVtbl ListEnumerator_Vtbl;

/* Load type info so we don't have to process GetIDsOfNames */
HRESULT load_type_info(IDispatch *iface, ITypeInfo **pptinfo, REFIID clsid, LCID lcid)
{
    HRESULT hr;
    LPTYPELIB pLib = NULL;
    LPTYPEINFO pInfo = NULL;
    static const WCHAR szMsiServer[] = {'m','s','i','s','e','r','v','e','r','.','t','l','b'};

    TRACE("(%p)->(%s,%d)\n", iface, debugstr_guid(clsid), lcid);

    /* Load registered type library */
    hr = LoadRegTypeLib(&LIBID_WindowsInstaller, 1, 0, lcid, &pLib);
    if (FAILED(hr)) {
        hr = LoadTypeLib(szMsiServer, &pLib);
        if (FAILED(hr)) {
            ERR("Could not load msiserver.tlb\n");
            return hr;
        }
    }

    /* Get type information for object */
    hr = ITypeLib_GetTypeInfoOfGuid(pLib, clsid, &pInfo);
    ITypeLib_Release(pLib);
    if (FAILED(hr)) {
        ERR("Could not load ITypeInfo for %s\n", debugstr_guid(clsid));
        return hr;
    }
    *pptinfo = pInfo;
    return S_OK;
}

/* Create the automation object, placing the result in the pointer ppObj. The automation object is created
 * with the appropriate clsid and invocation function. */
static HRESULT create_automation_object(MSIHANDLE msiHandle, IUnknown *pUnkOuter, void **ppObj, REFIID clsid,
        HRESULT (*funcInvoke)(AutomationObject*,DISPID,REFIID,LCID,WORD,DISPPARAMS*,VARIANT*,EXCEPINFO*,UINT*),
        void (*funcFree)(AutomationObject*), SIZE_T sizetPrivateData)
{
    AutomationObject *object;
    HRESULT hr;

    TRACE("(%d,%p,%p,%s,%p,%p,%ld)\n", msiHandle, pUnkOuter, ppObj, debugstr_guid(clsid), funcInvoke, funcFree, sizetPrivateData);

    if( pUnkOuter )
        return CLASS_E_NOAGGREGATION;

    object = msi_alloc_zero( sizeof(AutomationObject) + sizetPrivateData );

    /* Set all the VTable references */
    object->lpVtbl = &AutomationObject_Vtbl;
    object->lpvtblIProvideMultipleClassInfo = &AutomationObject_IProvideMultipleClassInfo_Vtbl;
    object->ref = 1;

    /* Store data that was passed */
    object->msiHandle = msiHandle;
    object->clsid = (LPCLSID)clsid;
    object->funcInvoke = funcInvoke;
    object->funcFree = funcFree;

    /* Load our TypeInfo so we don't have to process GetIDsOfNames */
    object->iTypeInfo = NULL;
    hr = load_type_info((IDispatch *)object, &object->iTypeInfo, clsid, 0x0);
    if (FAILED(hr)) {
        msi_free( object );
        return hr;
    }

    *ppObj = object;

    return S_OK;
}

/* Create a list enumerator, placing the result in the pointer ppObj.  */
static HRESULT create_list_enumerator(IUnknown *pUnkOuter, LPVOID *ppObj, AutomationObject *pObj, ULONG ulPos)
{
    ListEnumerator *object;

    TRACE("(%p,%p,%p,%uld)\n", pUnkOuter, ppObj, pObj, ulPos);

    if( pUnkOuter )
        return CLASS_E_NOAGGREGATION;

    object = msi_alloc_zero( sizeof(ListEnumerator) );

    /* Set all the VTable references */
    object->IEnumVARIANT_iface.lpVtbl = &ListEnumerator_Vtbl;
    object->ref = 1;

    /* Store data that was passed */
    object->ulPos = ulPos;
    object->pObj = pObj;
    if (pObj) IDispatch_AddRef((IDispatch *)pObj);

    *ppObj = object;
    return S_OK;
}

/* Macros to get pointer to AutomationObject from the other VTables. */
static inline AutomationObject *obj_from_IProvideMultipleClassInfo( IProvideMultipleClassInfo *iface )
{
    return (AutomationObject *)((char*)iface - FIELD_OFFSET(AutomationObject, lpvtblIProvideMultipleClassInfo));
}

/* Macro to get pointer to private object data */
static inline void *private_data( AutomationObject *This )
{
    return This + 1;
}

/*
 * AutomationObject methods
 */

/*** IUnknown methods ***/
static HRESULT WINAPI AutomationObject_QueryInterface(IDispatch* iface, REFIID riid, void** ppvObject)
{
    AutomationObject *This = (AutomationObject *)iface;

    TRACE("(%p/%p)->(%s,%p)\n", iface, This, debugstr_guid(riid), ppvObject);

    if (ppvObject == NULL)
      return E_INVALIDARG;

    *ppvObject = 0;

    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, This->clsid))
        *ppvObject = This;
    else if (IsEqualGUID(riid, &IID_IProvideClassInfo) ||
             IsEqualGUID(riid, &IID_IProvideClassInfo2) ||
             IsEqualGUID(riid, &IID_IProvideMultipleClassInfo))
        *ppvObject = &This->lpvtblIProvideMultipleClassInfo;
    else
    {
        TRACE("() : asking for unsupported interface %s\n",debugstr_guid(riid));
        return E_NOINTERFACE;
    }

    /*
     * Query Interface always increases the reference count by one when it is
     * successful
     */
    IClassFactory_AddRef(iface);

    return S_OK;
}

static ULONG WINAPI AutomationObject_AddRef(IDispatch* iface)
{
    AutomationObject *This = (AutomationObject *)iface;

    TRACE("(%p/%p)\n", iface, This);

    return InterlockedIncrement(&This->ref);
}

static ULONG WINAPI AutomationObject_Release(IDispatch* iface)
{
    AutomationObject *This = (AutomationObject *)iface;
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p/%p)\n", iface, This);

    if (!ref)
    {
        if (This->funcFree) This->funcFree(This);
        ITypeInfo_Release(This->iTypeInfo);
        MsiCloseHandle(This->msiHandle);
        msi_free(This);
    }

    return ref;
}

/*** IDispatch methods ***/
static HRESULT WINAPI AutomationObject_GetTypeInfoCount(
        IDispatch* iface,
        UINT* pctinfo)
{
    AutomationObject *This = (AutomationObject *)iface;

    TRACE("(%p/%p)->(%p)\n", iface, This, pctinfo);
    *pctinfo = 1;
    return S_OK;
}

static HRESULT WINAPI AutomationObject_GetTypeInfo(
        IDispatch* iface,
        UINT iTInfo,
        LCID lcid,
        ITypeInfo** ppTInfo)
{
    AutomationObject *This = (AutomationObject *)iface;
    TRACE("(%p/%p)->(%d,%d,%p)\n", iface, This, iTInfo, lcid, ppTInfo);

    ITypeInfo_AddRef(This->iTypeInfo);
    *ppTInfo = This->iTypeInfo;
    return S_OK;
}

static HRESULT WINAPI AutomationObject_GetIDsOfNames(
        IDispatch* iface,
        REFIID riid,
        LPOLESTR* rgszNames,
        UINT cNames,
        LCID lcid,
        DISPID* rgDispId)
{
    AutomationObject *This = (AutomationObject *)iface;
    HRESULT hr;
    TRACE("(%p/%p)->(%p,%p,%d,%d,%p)\n", iface, This, riid, rgszNames, cNames, lcid, rgDispId);

    if (!IsEqualGUID(riid, &IID_NULL)) return E_INVALIDARG;
    hr = ITypeInfo_GetIDsOfNames(This->iTypeInfo, rgszNames, cNames, rgDispId);
    if (hr == DISP_E_UNKNOWNNAME)
    {
        UINT idx;
        for (idx=0; idx<cNames; idx++)
        {
            if (rgDispId[idx] == DISPID_UNKNOWN)
                FIXME("Unknown member %s, clsid %s\n", debugstr_w(rgszNames[idx]), debugstr_guid(This->clsid));
        }
    }
    return hr;
}

/* Maximum number of allowed function parameters+1 */
#define MAX_FUNC_PARAMS 20

/* Some error checking is done here to simplify individual object function invocation */
static HRESULT WINAPI AutomationObject_Invoke(
        IDispatch* iface,
        DISPID dispIdMember,
        REFIID riid,
        LCID lcid,
        WORD wFlags,
        DISPPARAMS* pDispParams,
        VARIANT* pVarResult,
        EXCEPINFO* pExcepInfo,
        UINT* puArgErr)
{
    AutomationObject *This = (AutomationObject *)iface;
    HRESULT hr;
    unsigned int uArgErr;
    VARIANT varResultDummy;
    BSTR bstrName = NULL;

    TRACE("(%p/%p)->(%d,%p,%d,%d,%p,%p,%p,%p)\n", iface, This, dispIdMember, riid, lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    if (!IsEqualIID(riid, &IID_NULL))
    {
        ERR("riid was %s instead of IID_NULL\n", debugstr_guid(riid));
        return DISP_E_UNKNOWNNAME;
    }

    if (wFlags & DISPATCH_PROPERTYGET && !pVarResult)
    {
        ERR("NULL pVarResult not allowed when DISPATCH_PROPERTYGET specified\n");
        return DISP_E_PARAMNOTOPTIONAL;
    }

    /* This simplifies our individual object invocation functions */
    if (puArgErr == NULL) puArgErr = &uArgErr;
    if (pVarResult == NULL) pVarResult = &varResultDummy;

    /* Assume return type is void unless determined otherwise */
    VariantInit(pVarResult);

    /* If we are tracing, we want to see the name of the member we are invoking */
    if (TRACE_ON(msi))
    {
        ITypeInfo_GetDocumentation(This->iTypeInfo, dispIdMember, &bstrName, NULL, NULL, NULL);
        TRACE("Method %d, %s\n", dispIdMember, debugstr_w(bstrName));
    }

    hr = This->funcInvoke(This,dispIdMember,riid,lcid,wFlags,pDispParams,pVarResult,pExcepInfo,puArgErr);

    if (hr == DISP_E_MEMBERNOTFOUND) {
        if (bstrName == NULL) ITypeInfo_GetDocumentation(This->iTypeInfo, dispIdMember, &bstrName, NULL, NULL, NULL);
        FIXME("Method %d, %s wflags %d not implemented, clsid %s\n", dispIdMember, debugstr_w(bstrName), wFlags, debugstr_guid(This->clsid));
    }
    else if (pExcepInfo &&
             (hr == DISP_E_PARAMNOTFOUND ||
              hr == DISP_E_EXCEPTION)) {
        static const WCHAR szComma[] = { ',',0 };
        static const WCHAR szExceptionSource[] = {'M','s','i',' ','A','P','I',' ','E','r','r','o','r',0};
        WCHAR szExceptionDescription[MAX_PATH];
        BSTR bstrParamNames[MAX_FUNC_PARAMS];
        unsigned namesNo, i;
        BOOL bFirst = TRUE;

        if (FAILED(ITypeInfo_GetNames(This->iTypeInfo, dispIdMember, bstrParamNames,
                                      MAX_FUNC_PARAMS, &namesNo)))
        {
            TRACE("Failed to retrieve names for dispIdMember %d\n", dispIdMember);
        }
        else
        {
            memset(szExceptionDescription, 0, sizeof(szExceptionDescription));
            for (i=0; i<namesNo; i++)
            {
                if (bFirst) bFirst = FALSE;
                else {
                    lstrcpyW(&szExceptionDescription[lstrlenW(szExceptionDescription)], szComma);
                }
                lstrcpyW(&szExceptionDescription[lstrlenW(szExceptionDescription)], bstrParamNames[i]);
                SysFreeString(bstrParamNames[i]);
            }

            memset(pExcepInfo, 0, sizeof(EXCEPINFO));
            pExcepInfo->wCode = 1000;
            pExcepInfo->bstrSource = SysAllocString(szExceptionSource);
            pExcepInfo->bstrDescription = SysAllocString(szExceptionDescription);
            hr = DISP_E_EXCEPTION;
        }
    }

    /* Make sure we free the return variant if it is our dummy variant */
    if (pVarResult == &varResultDummy) VariantClear(pVarResult);

    /* Free function name if we retrieved it */
    SysFreeString(bstrName);

    TRACE("Returning 0x%08x, %s\n", hr, SUCCEEDED(hr) ? "ok" : "not ok");

    return hr;
}

static const struct IDispatchVtbl AutomationObject_Vtbl =
{
    AutomationObject_QueryInterface,
    AutomationObject_AddRef,
    AutomationObject_Release,
    AutomationObject_GetTypeInfoCount,
    AutomationObject_GetTypeInfo,
    AutomationObject_GetIDsOfNames,
    AutomationObject_Invoke
};

/*
 * IProvideMultipleClassInfo methods
 */

static HRESULT WINAPI AutomationObject_IProvideMultipleClassInfo_QueryInterface(
  IProvideMultipleClassInfo* iface,
  REFIID     riid,
  VOID**     ppvoid)
{
    AutomationObject *This = obj_from_IProvideMultipleClassInfo(iface);
    return AutomationObject_QueryInterface((IDispatch *)This, riid, ppvoid);
}

static ULONG WINAPI AutomationObject_IProvideMultipleClassInfo_AddRef(IProvideMultipleClassInfo* iface)
{
    AutomationObject *This = obj_from_IProvideMultipleClassInfo(iface);
    return AutomationObject_AddRef((IDispatch *)This);
}

static ULONG WINAPI AutomationObject_IProvideMultipleClassInfo_Release(IProvideMultipleClassInfo* iface)
{
    AutomationObject *This = obj_from_IProvideMultipleClassInfo(iface);
    return AutomationObject_Release((IDispatch *)This);
}

static HRESULT WINAPI AutomationObject_IProvideMultipleClassInfo_GetClassInfo(IProvideMultipleClassInfo* iface, ITypeInfo** ppTI)
{
    AutomationObject *This = obj_from_IProvideMultipleClassInfo(iface);
    TRACE("(%p/%p)->(%p)\n", iface, This, ppTI);
    return load_type_info((IDispatch *)This, ppTI, This->clsid, 0);
}

static HRESULT WINAPI AutomationObject_IProvideMultipleClassInfo_GetGUID(IProvideMultipleClassInfo* iface, DWORD dwGuidKind, GUID* pGUID)
{
    AutomationObject *This = obj_from_IProvideMultipleClassInfo(iface);
    TRACE("(%p/%p)->(%d,%s)\n", iface, This, dwGuidKind, debugstr_guid(pGUID));

    if (dwGuidKind != GUIDKIND_DEFAULT_SOURCE_DISP_IID)
        return E_INVALIDARG;
    else {
        *pGUID = *This->clsid;
        return S_OK;
    }
}

static HRESULT WINAPI AutomationObject_GetMultiTypeInfoCount(IProvideMultipleClassInfo* iface, ULONG* pcti)
{
    AutomationObject *This = obj_from_IProvideMultipleClassInfo(iface);

    TRACE("(%p/%p)->(%p)\n", iface, This, pcti);
    *pcti = 1;
    return S_OK;
}

static HRESULT WINAPI AutomationObject_GetInfoOfIndex(IProvideMultipleClassInfo* iface,
        ULONG iti,
        DWORD dwFlags,
        ITypeInfo** pptiCoClass,
        DWORD* pdwTIFlags,
        ULONG* pcdispidReserved,
        IID* piidPrimary,
        IID* piidSource)
{
    AutomationObject *This = obj_from_IProvideMultipleClassInfo(iface);

    TRACE("(%p/%p)->(%d,%d,%p,%p,%p,%p,%p)\n", iface, This, iti, dwFlags, pptiCoClass, pdwTIFlags, pcdispidReserved, piidPrimary, piidSource);

    if (iti != 0)
        return E_INVALIDARG;

    if (dwFlags & MULTICLASSINFO_GETTYPEINFO)
        load_type_info((IDispatch *)This, pptiCoClass, This->clsid, 0);

    if (dwFlags & MULTICLASSINFO_GETNUMRESERVEDDISPIDS)
    {
        *pdwTIFlags = 0;
        *pcdispidReserved = 0;
    }

    if (dwFlags & MULTICLASSINFO_GETIIDPRIMARY){
        *piidPrimary = *This->clsid;
    }

    if (dwFlags & MULTICLASSINFO_GETIIDSOURCE){
        *piidSource = *This->clsid;
    }

    return S_OK;
}

static const IProvideMultipleClassInfoVtbl AutomationObject_IProvideMultipleClassInfo_Vtbl =
{
    AutomationObject_IProvideMultipleClassInfo_QueryInterface,
    AutomationObject_IProvideMultipleClassInfo_AddRef,
    AutomationObject_IProvideMultipleClassInfo_Release,
    AutomationObject_IProvideMultipleClassInfo_GetClassInfo,
    AutomationObject_IProvideMultipleClassInfo_GetGUID,
    AutomationObject_GetMultiTypeInfoCount,
    AutomationObject_GetInfoOfIndex
};

/*
 * ListEnumerator methods
 */

static inline ListEnumerator *impl_from_IEnumVARIANT(IEnumVARIANT* iface)
{
    return CONTAINING_RECORD(iface, ListEnumerator, IEnumVARIANT_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI ListEnumerator_QueryInterface(IEnumVARIANT* iface, REFIID riid,
        void** ppvObject)
{
    ListEnumerator *This = impl_from_IEnumVARIANT(iface);

    TRACE("(%p/%p)->(%s,%p)\n", iface, This, debugstr_guid(riid), ppvObject);

    if (ppvObject == NULL)
      return E_INVALIDARG;

    *ppvObject = 0;

    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IEnumVARIANT))
        *ppvObject = This;
    else
    {
        TRACE("() : asking for unsupported interface %s\n",debugstr_guid(riid));
        return E_NOINTERFACE;
    }

    IClassFactory_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI ListEnumerator_AddRef(IEnumVARIANT* iface)
{
    ListEnumerator *This = impl_from_IEnumVARIANT(iface);

    TRACE("(%p/%p)\n", iface, This);

    return InterlockedIncrement(&This->ref);
}

static ULONG WINAPI ListEnumerator_Release(IEnumVARIANT* iface)
{
    ListEnumerator *This = impl_from_IEnumVARIANT(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p/%p)\n", iface, This);

    if (!ref)
    {
        if (This->pObj) IDispatch_Release((IDispatch *)This->pObj);
        msi_free(This);
    }

    return ref;
}

/* IEnumVARIANT methods */

static HRESULT WINAPI ListEnumerator_Next(IEnumVARIANT* iface, ULONG celt, VARIANT* rgVar,
        ULONG* pCeltFetched)
{
    ListEnumerator *This = impl_from_IEnumVARIANT(iface);
    ListData *data = private_data(This->pObj);
    ULONG idx, local;

    TRACE("(%p,%uld,%p,%p)\n", iface, celt, rgVar, pCeltFetched);

    if (pCeltFetched != NULL)
        *pCeltFetched = 0;

    if (rgVar == NULL)
        return S_FALSE;

    for (local = 0; local < celt; local++)
        VariantInit(&rgVar[local]);

    for (idx = This->ulPos, local = 0; idx < data->ulCount && local < celt; idx++, local++)
        VariantCopy(&rgVar[local], &data->pVars[idx]);

    if (pCeltFetched != NULL)
        *pCeltFetched = local;
    This->ulPos = idx;

    return (local < celt) ? S_FALSE : S_OK;
}

static HRESULT WINAPI ListEnumerator_Skip(IEnumVARIANT* iface, ULONG celt)
{
    ListEnumerator *This = impl_from_IEnumVARIANT(iface);
    ListData *data = private_data(This->pObj);

    TRACE("(%p,%uld)\n", iface, celt);

    This->ulPos += celt;
    if (This->ulPos >= data->ulCount)
    {
        This->ulPos = data->ulCount;
        return S_FALSE;
    }
    return S_OK;
}

static HRESULT WINAPI ListEnumerator_Reset(IEnumVARIANT* iface)
{
    ListEnumerator *This = impl_from_IEnumVARIANT(iface);

    TRACE("(%p)\n", iface);

    This->ulPos = 0;
    return S_OK;
}

static HRESULT WINAPI ListEnumerator_Clone(IEnumVARIANT* iface, IEnumVARIANT **ppEnum)
{
    ListEnumerator *This = impl_from_IEnumVARIANT(iface);
    HRESULT hr;

    TRACE("(%p,%p)\n", iface, ppEnum);

    if (ppEnum == NULL)
        return S_FALSE;

    *ppEnum = NULL;
    hr = create_list_enumerator(NULL, (LPVOID *)ppEnum, This->pObj, 0);
    if (FAILED(hr))
    {
        if (*ppEnum)
            IUnknown_Release(*ppEnum);
        return hr;
    }

    return S_OK;
}

static const struct IEnumVARIANTVtbl ListEnumerator_Vtbl =
{
    ListEnumerator_QueryInterface,
    ListEnumerator_AddRef,
    ListEnumerator_Release,
    ListEnumerator_Next,
    ListEnumerator_Skip,
    ListEnumerator_Reset,
    ListEnumerator_Clone
};

/*
 * Individual Object Invocation Functions
 */

/* Helper function that copies a passed parameter instead of using VariantChangeType like the actual DispGetParam.
   This function is only for VARIANT type parameters that have several types that cannot be properly discriminated
   using DispGetParam/VariantChangeType. */
static HRESULT DispGetParam_CopyOnly(
        DISPPARAMS *pdispparams, /* [in] Parameter list */
        UINT        *position,    /* [in] Position of parameter to copy in pdispparams; on return will contain calculated position */
        VARIANT    *pvarResult)  /* [out] Destination for resulting variant */
{
    /* position is counted backwards */
    UINT pos;

    TRACE("position=%d, cArgs=%d, cNamedArgs=%d\n",
          *position, pdispparams->cArgs, pdispparams->cNamedArgs);
    if (*position < pdispparams->cArgs) {
      /* positional arg? */
      pos = pdispparams->cArgs - *position - 1;
    } else {
      /* FIXME: is this how to handle named args? */
      for (pos=0; pos<pdispparams->cNamedArgs; pos++)
        if (pdispparams->rgdispidNamedArgs[pos] == *position) break;

      if (pos==pdispparams->cNamedArgs)
        return DISP_E_PARAMNOTFOUND;
    }
    *position = pos;
    return VariantCopyInd(pvarResult,
                        &pdispparams->rgvarg[pos]);
}

static HRESULT SummaryInfoImpl_Invoke(
        AutomationObject* This,
        DISPID dispIdMember,
        REFIID riid,
        LCID lcid,
        WORD wFlags,
        DISPPARAMS* pDispParams,
        VARIANT* pVarResult,
        EXCEPINFO* pExcepInfo,
        UINT* puArgErr)
{
    UINT ret;
    VARIANTARG varg0, varg1;
    FILETIME ft, ftlocal;
    SYSTEMTIME st;
    HRESULT hr;

    VariantInit(&varg0);
    VariantInit(&varg1);

    switch (dispIdMember)
    {
        case DISPID_SUMMARYINFO_PROPERTY:
            if (wFlags & DISPATCH_PROPERTYGET)
            {
                UINT type;
                INT value;
                DWORD size = 0;
                DATE date;
                LPWSTR str;

                static WCHAR szEmpty[] = {0};

                hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                ret = MsiSummaryInfoGetPropertyW(This->msiHandle, V_I4(&varg0), &type, &value,
                                                 &ft, szEmpty, &size);
                if (ret != ERROR_SUCCESS &&
                    ret != ERROR_MORE_DATA)
                {
                    ERR("MsiSummaryInfoGetProperty returned %d\n", ret);
                    return DISP_E_EXCEPTION;
                }

                switch (type)
                {
                    case VT_EMPTY:
                        break;

                    case VT_I2:
                    case VT_I4:
                        V_VT(pVarResult) = VT_I4;
                        V_I4(pVarResult) = value;
                        break;

                    case VT_LPSTR:
                        if (!(str = msi_alloc(++size * sizeof(WCHAR))))
                            ERR("Out of memory\n");
                        else if ((ret = MsiSummaryInfoGetPropertyW(This->msiHandle, V_I4(&varg0), &type, NULL,
                                                                   NULL, str, &size)) != ERROR_SUCCESS)
                            ERR("MsiSummaryInfoGetProperty returned %d\n", ret);
                        else
                        {
                            V_VT(pVarResult) = VT_BSTR;
                            V_BSTR(pVarResult) = SysAllocString(str);
                        }
                        msi_free(str);
                        break;

                    case VT_FILETIME:
                        FileTimeToLocalFileTime(&ft, &ftlocal);
                        FileTimeToSystemTime(&ftlocal, &st);
                        SystemTimeToVariantTime(&st, &date);

                        V_VT(pVarResult) = VT_DATE;
                        V_DATE(pVarResult) = date;
                        break;

                    default:
                        ERR("Unhandled variant type %d\n", type);
                }
            }
            else if (wFlags & DISPATCH_PROPERTYPUT)
            {
                UINT posValue = DISPID_PROPERTYPUT;

                hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                hr = DispGetParam_CopyOnly(pDispParams, &posValue, &varg1);
                if (FAILED(hr))
                {
                    *puArgErr = posValue;
                    return hr;
                }

                switch (V_VT(&varg1))
                {
                    case VT_I2:
                    case VT_I4:
                        ret = MsiSummaryInfoSetPropertyW(This->msiHandle, V_I4(&varg0), V_VT(&varg1), V_I4(&varg1), NULL, NULL);
                        break;

                    case VT_DATE:
                        VariantTimeToSystemTime(V_DATE(&varg1), &st);
                        SystemTimeToFileTime(&st, &ftlocal);
                        LocalFileTimeToFileTime(&ftlocal, &ft);
                        ret = MsiSummaryInfoSetPropertyW(This->msiHandle, V_I4(&varg0), VT_FILETIME, 0, &ft, NULL);
                        break;

                    case VT_BSTR:
                        ret = MsiSummaryInfoSetPropertyW(This->msiHandle, V_I4(&varg0), VT_LPSTR, 0, NULL, V_BSTR(&varg1));
                        break;

                    default:
                        FIXME("Unhandled variant type %d\n", V_VT(&varg1));
                        VariantClear(&varg1);
                        return DISP_E_EXCEPTION;
                }

                if (ret != ERROR_SUCCESS)
                {
                    ERR("MsiSummaryInfoSetPropertyW returned %d\n", ret);
                    return DISP_E_EXCEPTION;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_SUMMARYINFO_PROPERTYCOUNT:
            if (wFlags & DISPATCH_PROPERTYGET) {
                UINT count;
                if ((ret = MsiSummaryInfoGetPropertyCount(This->msiHandle, &count)) != ERROR_SUCCESS)
                    ERR("MsiSummaryInfoGetPropertyCount returned %d\n", ret);
                else
                {
                    V_VT(pVarResult) = VT_I4;
                    V_I4(pVarResult) = count;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        default:
            return DISP_E_MEMBERNOTFOUND;
    }

    VariantClear(&varg1);
    VariantClear(&varg0);

    return S_OK;
}

static HRESULT RecordImpl_Invoke(
        AutomationObject* This,
        DISPID dispIdMember,
        REFIID riid,
        LCID lcid,
        WORD wFlags,
        DISPPARAMS* pDispParams,
        VARIANT* pVarResult,
        EXCEPINFO* pExcepInfo,
        UINT* puArgErr)
{
    WCHAR *szString;
    DWORD dwLen;
    UINT ret;
    VARIANTARG varg0, varg1;
    HRESULT hr;

    VariantInit(&varg0);
    VariantInit(&varg1);

    switch (dispIdMember)
    {
        case DISPID_RECORD_FIELDCOUNT:
            if (wFlags & DISPATCH_PROPERTYGET) {
                V_VT(pVarResult) = VT_I4;
                V_I4(pVarResult) = MsiRecordGetFieldCount(This->msiHandle);
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_RECORD_STRINGDATA:
            if (wFlags & DISPATCH_PROPERTYGET) {
                hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                V_VT(pVarResult) = VT_BSTR;
                V_BSTR(pVarResult) = NULL;
                if ((ret = MsiRecordGetStringW(This->msiHandle, V_I4(&varg0), NULL, &dwLen)) == ERROR_SUCCESS)
                {
                    if (!(szString = msi_alloc((++dwLen)*sizeof(WCHAR))))
                        ERR("Out of memory\n");
                    else if ((ret = MsiRecordGetStringW(This->msiHandle, V_I4(&varg0), szString, &dwLen)) == ERROR_SUCCESS)
                        V_BSTR(pVarResult) = SysAllocString(szString);
                    msi_free(szString);
                }
                if (ret != ERROR_SUCCESS)
                    ERR("MsiRecordGetString returned %d\n", ret);
            } else if (wFlags & DISPATCH_PROPERTYPUT) {
                hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                hr = DispGetParam(pDispParams, DISPID_PROPERTYPUT, VT_BSTR, &varg1, puArgErr);
                if (FAILED(hr)) return hr;
                if ((ret = MsiRecordSetStringW(This->msiHandle, V_I4(&varg0), V_BSTR(&varg1))) != ERROR_SUCCESS)
                {
                    VariantClear(&varg1);
                    ERR("MsiRecordSetString returned %d\n", ret);
                    return DISP_E_EXCEPTION;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_RECORD_INTEGERDATA:
            if (wFlags & DISPATCH_PROPERTYGET) {
                hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                V_VT(pVarResult) = VT_I4;
                V_I4(pVarResult) = MsiRecordGetInteger(This->msiHandle, V_I4(&varg0));
            } else if (wFlags & DISPATCH_PROPERTYPUT) {
                hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                hr = DispGetParam(pDispParams, DISPID_PROPERTYPUT, VT_I4, &varg1, puArgErr);
                if (FAILED(hr)) return hr;
                if ((ret = MsiRecordSetInteger(This->msiHandle, V_I4(&varg0), V_I4(&varg1))) != ERROR_SUCCESS)
                {
                    ERR("MsiRecordSetInteger returned %d\n", ret);
                    return DISP_E_EXCEPTION;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

         default:
            return DISP_E_MEMBERNOTFOUND;
    }

    VariantClear(&varg1);
    VariantClear(&varg0);

    return S_OK;
}

static HRESULT ListImpl_Invoke(
        AutomationObject* This,
        DISPID dispIdMember,
        REFIID riid,
        LCID lcid,
        WORD wFlags,
        DISPPARAMS* pDispParams,
        VARIANT* pVarResult,
        EXCEPINFO* pExcepInfo,
        UINT* puArgErr)
{
    ListData *data = private_data(This);
    HRESULT hr;
    VARIANTARG varg0;
    IUnknown *pUnk = NULL;

    VariantInit(&varg0);

    switch (dispIdMember)
    {
         case DISPID_LIST__NEWENUM:
             if (wFlags & DISPATCH_METHOD) {
                 V_VT(pVarResult) = VT_UNKNOWN;
                 if (SUCCEEDED(hr = create_list_enumerator(NULL, (LPVOID *)&pUnk, This, 0)))
                     V_UNKNOWN(pVarResult) = pUnk;
                 else
                     ERR("Failed to create IEnumVARIANT object, hresult 0x%08x\n", hr);
             }
             else return DISP_E_MEMBERNOTFOUND;
             break;

         case DISPID_LIST_ITEM:
             if (wFlags & DISPATCH_PROPERTYGET) {
                hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                if (V_I4(&varg0) < 0 || V_I4(&varg0) >= data->ulCount)
                    return DISP_E_BADINDEX;
                VariantCopy(pVarResult, &data->pVars[V_I4(&varg0)]);
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

         case DISPID_LIST_COUNT:
            if (wFlags & DISPATCH_PROPERTYGET) {
                V_VT(pVarResult) = VT_I4;
                V_I4(pVarResult) = data->ulCount;
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

         default:
            return DISP_E_MEMBERNOTFOUND;
    }

    VariantClear(&varg0);

    return S_OK;
}

static void ListImpl_Free(AutomationObject *This)
{
    ListData *data = private_data(This);
    ULONG idx;

    for (idx=0; idx<data->ulCount; idx++)
        VariantClear(&data->pVars[idx]);
    msi_free(data->pVars);
}

static HRESULT ViewImpl_Invoke(
        AutomationObject* This,
        DISPID dispIdMember,
        REFIID riid,
        LCID lcid,
        WORD wFlags,
        DISPPARAMS* pDispParams,
        VARIANT* pVarResult,
        EXCEPINFO* pExcepInfo,
        UINT* puArgErr)
{
    MSIHANDLE msiHandle;
    IDispatch *pDispatch = NULL;
    UINT ret;
    VARIANTARG varg0, varg1;
    HRESULT hr;

    VariantInit(&varg0);
    VariantInit(&varg1);

    switch (dispIdMember)
    {
        case DISPID_VIEW_EXECUTE:
            if (wFlags & DISPATCH_METHOD)
            {
                hr = DispGetParam(pDispParams, 0, VT_DISPATCH, &varg0, puArgErr);
                if (SUCCEEDED(hr) && V_DISPATCH(&varg0) != NULL)
                    MsiViewExecute(This->msiHandle, ((AutomationObject *)V_DISPATCH(&varg0))->msiHandle);
                else
                    MsiViewExecute(This->msiHandle, 0);
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_VIEW_FETCH:
            if (wFlags & DISPATCH_METHOD)
            {
                V_VT(pVarResult) = VT_DISPATCH;
                if ((ret = MsiViewFetch(This->msiHandle, &msiHandle)) == ERROR_SUCCESS)
                {
                    if (SUCCEEDED(hr = create_automation_object(msiHandle, NULL, (LPVOID*)&pDispatch, &DIID_Record, RecordImpl_Invoke, NULL, 0)))
                        V_DISPATCH(pVarResult) = pDispatch;
                    else
                        ERR("Failed to create Record object, hresult 0x%08x\n", hr);
                }
                else if (ret == ERROR_NO_MORE_ITEMS)
                    V_DISPATCH(pVarResult) = NULL;
                else
                {
                    ERR("MsiViewFetch returned %d\n", ret);
                    return DISP_E_EXCEPTION;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_VIEW_MODIFY:
            if (wFlags & DISPATCH_METHOD)
            {
                hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                hr = DispGetParam(pDispParams, 1, VT_DISPATCH, &varg1, puArgErr);
                if (FAILED(hr)) return hr;
                if (!V_DISPATCH(&varg1)) return DISP_E_EXCEPTION;
                if ((ret = MsiViewModify(This->msiHandle, V_I4(&varg0), ((AutomationObject *)V_DISPATCH(&varg1))->msiHandle)) != ERROR_SUCCESS)
                {
                    VariantClear(&varg1);
                    ERR("MsiViewModify returned %d\n", ret);
                    return DISP_E_EXCEPTION;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_VIEW_CLOSE:
            if (wFlags & DISPATCH_METHOD)
            {
                MsiViewClose(This->msiHandle);
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

         default:
            return DISP_E_MEMBERNOTFOUND;
    }

    VariantClear(&varg1);
    VariantClear(&varg0);

    return S_OK;
}

static HRESULT DatabaseImpl_LastErrorRecord(WORD wFlags,
                                            DISPPARAMS* pDispParams,
                                            VARIANT* pVarResult,
                                            EXCEPINFO* pExcepInfo,
                                            UINT* puArgErr)
{
    if (!(wFlags & DISPATCH_METHOD))
        return DISP_E_MEMBERNOTFOUND;

    FIXME("\n");

    VariantInit(pVarResult);
    return S_OK;
}

static HRESULT DatabaseImpl_Invoke(
        AutomationObject* This,
        DISPID dispIdMember,
        REFIID riid,
        LCID lcid,
        WORD wFlags,
        DISPPARAMS* pDispParams,
        VARIANT* pVarResult,
        EXCEPINFO* pExcepInfo,
        UINT* puArgErr)
{
    MSIHANDLE msiHandle;
    IDispatch *pDispatch = NULL;
    UINT ret;
    VARIANTARG varg0, varg1;
    HRESULT hr;

    VariantInit(&varg0);
    VariantInit(&varg1);

    switch (dispIdMember)
    {
        case DISPID_DATABASE_SUMMARYINFORMATION:
            if (wFlags & DISPATCH_PROPERTYGET)
            {
                hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
                if (FAILED(hr))
                    V_I4(&varg0) = 0;

                V_VT(pVarResult) = VT_DISPATCH;
                if ((ret = MsiGetSummaryInformationW(This->msiHandle, NULL, V_I4(&varg0), &msiHandle)) == ERROR_SUCCESS)
                {
                    hr = create_automation_object(msiHandle, NULL, (LPVOID *)&pDispatch, &DIID_SummaryInfo, SummaryInfoImpl_Invoke, NULL, 0);
                    if (SUCCEEDED(hr))
                        V_DISPATCH(pVarResult) = pDispatch;
                    else
                        ERR("Failed to create SummaryInfo object: 0x%08x\n", hr);
                }
                else
                {
                    ERR("MsiGetSummaryInformation returned %d\n", ret);
                    return DISP_E_EXCEPTION;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_DATABASE_OPENVIEW:
            if (wFlags & DISPATCH_METHOD)
            {
                hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                V_VT(pVarResult) = VT_DISPATCH;
                if ((ret = MsiDatabaseOpenViewW(This->msiHandle, V_BSTR(&varg0), &msiHandle)) == ERROR_SUCCESS)
                {
                    if (SUCCEEDED(hr = create_automation_object(msiHandle, NULL, (LPVOID*)&pDispatch, &DIID_View, ViewImpl_Invoke, NULL, 0)))
                        V_DISPATCH(pVarResult) = pDispatch;
                    else
                        ERR("Failed to create View object, hresult 0x%08x\n", hr);
                }
                else
                {
                    VariantClear(&varg0);
                    ERR("MsiDatabaseOpenView returned %d\n", ret);
                    return DISP_E_EXCEPTION;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_INSTALLER_LASTERRORRECORD:
            return DatabaseImpl_LastErrorRecord(wFlags, pDispParams,
                                                pVarResult, pExcepInfo,
                                                puArgErr);

         default:
            return DISP_E_MEMBERNOTFOUND;
    }

    VariantClear(&varg1);
    VariantClear(&varg0);

    return S_OK;
}

static HRESULT SessionImpl_Invoke(
        AutomationObject* This,
        DISPID dispIdMember,
        REFIID riid,
        LCID lcid,
        WORD wFlags,
        DISPPARAMS* pDispParams,
        VARIANT* pVarResult,
        EXCEPINFO* pExcepInfo,
        UINT* puArgErr)
{
    SessionData *data = private_data(This);
    WCHAR *szString;
    DWORD dwLen;
    IDispatch *pDispatch = NULL;
    MSIHANDLE msiHandle;
    LANGID langId;
    UINT ret;
    INSTALLSTATE iInstalled, iAction;
    VARIANTARG varg0, varg1;
    HRESULT hr;

    VariantInit(&varg0);
    VariantInit(&varg1);

    switch (dispIdMember)
    {
        case DISPID_SESSION_INSTALLER:
            if (wFlags & DISPATCH_PROPERTYGET) {
                V_VT(pVarResult) = VT_DISPATCH;
                IDispatch_AddRef(data->pInstaller);
                V_DISPATCH(pVarResult) = data->pInstaller;
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_SESSION_PROPERTY:
            if (wFlags & DISPATCH_PROPERTYGET) {
                hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                V_VT(pVarResult) = VT_BSTR;
                V_BSTR(pVarResult) = NULL;
                if ((ret = MsiGetPropertyW(This->msiHandle, V_BSTR(&varg0), NULL, &dwLen)) == ERROR_SUCCESS)
                {
                    if (!(szString = msi_alloc((++dwLen)*sizeof(WCHAR))))
                        ERR("Out of memory\n");
                    else if ((ret = MsiGetPropertyW(This->msiHandle, V_BSTR(&varg0), szString, &dwLen)) == ERROR_SUCCESS)
                        V_BSTR(pVarResult) = SysAllocString(szString);
                    msi_free(szString);
                }
                if (ret != ERROR_SUCCESS)
                    ERR("MsiGetProperty returned %d\n", ret);
            } else if (wFlags & DISPATCH_PROPERTYPUT) {
                hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                hr = DispGetParam(pDispParams, DISPID_PROPERTYPUT, VT_BSTR, &varg1, puArgErr);
                if (FAILED(hr)) {
                    VariantClear(&varg0);
                    return hr;
                }
                if ((ret = MsiSetPropertyW(This->msiHandle, V_BSTR(&varg0), V_BSTR(&varg1))) != ERROR_SUCCESS)
                {
                    VariantClear(&varg0);
                    VariantClear(&varg1);
                    ERR("MsiSetProperty returned %d\n", ret);
                    return DISP_E_EXCEPTION;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_SESSION_LANGUAGE:
            if (wFlags & DISPATCH_PROPERTYGET) {
                langId = MsiGetLanguage(This->msiHandle);
                V_VT(pVarResult) = VT_I4;
                V_I4(pVarResult) = langId;
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_SESSION_MODE:
            if (wFlags & DISPATCH_PROPERTYGET) {
                hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                V_VT(pVarResult) = VT_BOOL;
                V_BOOL(pVarResult) = MsiGetMode(This->msiHandle, V_I4(&varg0));
            } else if (wFlags & DISPATCH_PROPERTYPUT) {
                hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                hr = DispGetParam(pDispParams, DISPID_PROPERTYPUT, VT_BOOL, &varg1, puArgErr);
                if (FAILED(hr)) return hr;
                if ((ret = MsiSetMode(This->msiHandle, V_I4(&varg0), V_BOOL(&varg1))) != ERROR_SUCCESS)
                {
                    ERR("MsiSetMode returned %d\n", ret);
                    return DISP_E_EXCEPTION;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_SESSION_DATABASE:
            if (wFlags & DISPATCH_PROPERTYGET) {
                V_VT(pVarResult) = VT_DISPATCH;
                if ((msiHandle = MsiGetActiveDatabase(This->msiHandle)))
                {
                    if (SUCCEEDED(hr = create_automation_object(msiHandle, NULL, (LPVOID*)&pDispatch, &DIID_Database, DatabaseImpl_Invoke, NULL, 0)))
                        V_DISPATCH(pVarResult) = pDispatch;
                    else
                        ERR("Failed to create Database object, hresult 0x%08x\n", hr);
                }
                else
                {
                    ERR("MsiGetActiveDatabase failed\n");
                    return DISP_E_EXCEPTION;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_SESSION_DOACTION:
            if (wFlags & DISPATCH_METHOD) {
                hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                ret = MsiDoActionW(This->msiHandle, V_BSTR(&varg0));
                V_VT(pVarResult) = VT_I4;
                switch (ret)
                {
                    case ERROR_FUNCTION_NOT_CALLED:
                        V_I4(pVarResult) = msiDoActionStatusNoAction;
                        break;
                    case ERROR_SUCCESS:
                        V_I4(pVarResult) = msiDoActionStatusSuccess;
                        break;
                    case ERROR_INSTALL_USEREXIT:
                        V_I4(pVarResult) = msiDoActionStatusUserExit;
                        break;
                    case ERROR_INSTALL_FAILURE:
                        V_I4(pVarResult) = msiDoActionStatusFailure;
                        break;
                    case ERROR_INSTALL_SUSPEND:
                        V_I4(pVarResult) = msiDoActionStatusSuspend;
                        break;
                    case ERROR_MORE_DATA:
                        V_I4(pVarResult) = msiDoActionStatusFinished;
                        break;
                    case ERROR_INVALID_HANDLE_STATE:
                        V_I4(pVarResult) = msiDoActionStatusWrongState;
                        break;
                    case ERROR_INVALID_DATA:
                        V_I4(pVarResult) = msiDoActionStatusBadActionData;
                        break;
                    default:
                        VariantClear(&varg0);
                        FIXME("MsiDoAction returned unhandled value %d\n", ret);
                        return DISP_E_EXCEPTION;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_SESSION_EVALUATECONDITION:
            if (wFlags & DISPATCH_METHOD) {
                hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                V_VT(pVarResult) = VT_I4;
                V_I4(pVarResult) = MsiEvaluateConditionW(This->msiHandle, V_BSTR(&varg0));
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_SESSION_MESSAGE:
            if(!(wFlags & DISPATCH_METHOD))
                return DISP_E_MEMBERNOTFOUND;

            hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
            if (FAILED(hr)) return hr;
            hr = DispGetParam(pDispParams, 1, VT_DISPATCH, &varg1, puArgErr);
            if (FAILED(hr)) return hr;

            V_VT(pVarResult) = VT_I4;
            V_I4(pVarResult) =
                MsiProcessMessage(This->msiHandle, V_I4(&varg0), ((AutomationObject *)V_DISPATCH(&varg1))->msiHandle);
            break;

        case DISPID_SESSION_SETINSTALLLEVEL:
            if (wFlags & DISPATCH_METHOD) {
                hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                if ((ret = MsiSetInstallLevel(This->msiHandle, V_I4(&varg0))) != ERROR_SUCCESS)
                {
                    ERR("MsiSetInstallLevel returned %d\n", ret);
                    return DISP_E_EXCEPTION;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_SESSION_FEATURECURRENTSTATE:
            if (wFlags & DISPATCH_PROPERTYGET) {
                hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                V_VT(pVarResult) = VT_I4;
                if ((ret = MsiGetFeatureStateW(This->msiHandle, V_BSTR(&varg0), &iInstalled, &iAction)) == ERROR_SUCCESS)
                    V_I4(pVarResult) = iInstalled;
                else
                {
                    ERR("MsiGetFeatureState returned %d\n", ret);
                    V_I4(pVarResult) = msiInstallStateUnknown;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

        case DISPID_SESSION_FEATUREREQUESTSTATE:
            if (wFlags & DISPATCH_PROPERTYGET) {
                hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                V_VT(pVarResult) = VT_I4;
                if ((ret = MsiGetFeatureStateW(This->msiHandle, V_BSTR(&varg0), &iInstalled, &iAction)) == ERROR_SUCCESS)
                    V_I4(pVarResult) = iAction;
                else
                {
                    ERR("MsiGetFeatureState returned %d\n", ret);
                    V_I4(pVarResult) = msiInstallStateUnknown;
                }
            } else if (wFlags & DISPATCH_PROPERTYPUT) {
                hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
                if (FAILED(hr)) return hr;
                hr = DispGetParam(pDispParams, DISPID_PROPERTYPUT, VT_I4, &varg1, puArgErr);
                if (FAILED(hr)) {
                    VariantClear(&varg0);
                    return hr;
                }
                if ((ret = MsiSetFeatureStateW(This->msiHandle, V_BSTR(&varg0), V_I4(&varg1))) != ERROR_SUCCESS)
                {
                    VariantClear(&varg0);
                    ERR("MsiSetFeatureState returned %d\n", ret);
                    return DISP_E_EXCEPTION;
                }
            }
            else return DISP_E_MEMBERNOTFOUND;
            break;

         default:
            return DISP_E_MEMBERNOTFOUND;
    }

    VariantClear(&varg1);
    VariantClear(&varg0);

    return S_OK;
}

/* Fill the variant pointed to by pVarResult with the value & size returned by RegQueryValueEx as dictated by the
 * registry value type. Used by Installer::RegistryValue. */
static void variant_from_registry_value(VARIANT *pVarResult, DWORD dwType, LPBYTE lpData, DWORD dwSize)
{
    static const WCHAR szREG_BINARY[] = { '(','R','E','G','_','B','I','N','A','R','Y',')',0 };
    static const WCHAR szREG_[] = { '(','R','E','G','_',']',0 };
    WCHAR *szString = (WCHAR *)lpData;
    LPWSTR szNewString = NULL;
    DWORD dwNewSize = 0;
    int idx;

    switch (dwType)
    {
        /* Registry strings may not be null terminated so we must use SysAllocStringByteLen/Len */
        case REG_MULTI_SZ: /* Multi SZ change internal null characters to newlines */
            idx = (dwSize/sizeof(WCHAR))-1;
            while (idx >= 0 && !szString[idx]) idx--;
            for (; idx >= 0; idx--)
                if (!szString[idx]) szString[idx] = '\n';
        case REG_SZ:
            V_VT(pVarResult) = VT_BSTR;
            V_BSTR(pVarResult) = SysAllocStringByteLen((LPCSTR)szString, dwSize);
            break;

        case REG_EXPAND_SZ:
            if (!(dwNewSize = ExpandEnvironmentStringsW(szString, szNewString, dwNewSize)))
                ERR("ExpandEnvironmentStrings returned error %d\n", GetLastError());
            else if (!(szNewString = msi_alloc(dwNewSize * sizeof(WCHAR))))
                ERR("Out of memory\n");
            else if (!(dwNewSize = ExpandEnvironmentStringsW(szString, szNewString, dwNewSize)))
                ERR("ExpandEnvironmentStrings returned error %d\n", GetLastError());
            else
            {
                V_VT(pVarResult) = VT_BSTR;
                V_BSTR(pVarResult) = SysAllocStringLen(szNewString, dwNewSize);
            }
            msi_free(szNewString);
            break;

        case REG_DWORD:
            V_VT(pVarResult) = VT_I4;
            V_I4(pVarResult) = *((DWORD *)lpData);
            break;

        case REG_QWORD:
            V_VT(pVarResult) = VT_BSTR;
            V_BSTR(pVarResult) = SysAllocString(szREG_);   /* Weird string, don't know why native returns it */
            break;

        case REG_BINARY:
            V_VT(pVarResult) = VT_BSTR;
            V_BSTR(pVarResult) = SysAllocString(szREG_BINARY);
            break;

        case REG_NONE:
            V_VT(pVarResult) = VT_EMPTY;
            break;

        default:
            FIXME("Unhandled registry value type %d\n", dwType);
    }
}

static HRESULT InstallerImpl_CreateRecord(WORD wFlags,
                                          DISPPARAMS* pDispParams,
                                          VARIANT* pVarResult,
                                          EXCEPINFO* pExcepInfo,
                                          UINT* puArgErr)
{
    HRESULT hr;
    VARIANTARG varg0;
    MSIHANDLE hrec;
    IDispatch* dispatch;

    if (!(wFlags & DISPATCH_METHOD))
        return DISP_E_MEMBERNOTFOUND;

    VariantInit(&varg0);
    hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
    if (FAILED(hr))
        return hr;

    V_VT(pVarResult) = VT_DISPATCH;

    hrec = MsiCreateRecord(V_I4(&varg0));
    if (!hrec)
        return DISP_E_EXCEPTION;

    hr = create_automation_object(hrec, NULL, (LPVOID*)&dispatch,
                                  &DIID_Record, RecordImpl_Invoke, NULL, 0);
    if (SUCCEEDED(hr))
        V_DISPATCH(pVarResult) = dispatch;

    return hr;
}

static HRESULT InstallerImpl_OpenPackage(AutomationObject* This,
                                         WORD wFlags,
                                         DISPPARAMS* pDispParams,
                                         VARIANT* pVarResult,
                                         EXCEPINFO* pExcepInfo,
                                         UINT* puArgErr)
{
    UINT ret;
    HRESULT hr;
    MSIHANDLE hpkg;
    IDispatch* dispatch;
    VARIANTARG varg0, varg1;

    if (!(wFlags & DISPATCH_METHOD))
        return DISP_E_MEMBERNOTFOUND;

    if (pDispParams->cArgs == 0)
        return DISP_E_TYPEMISMATCH;

    if (V_VT(&pDispParams->rgvarg[pDispParams->cArgs - 1]) != VT_BSTR)
        return DISP_E_TYPEMISMATCH;

    VariantInit(&varg0);
    hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
    if (FAILED(hr))
        return hr;

    VariantInit(&varg1);
    if (pDispParams->cArgs == 2)
    {
        hr = DispGetParam(pDispParams, 1, VT_I4, &varg1, puArgErr);
        if (FAILED(hr))
            goto done;
    }
    else
    {
        V_VT(&varg1) = VT_I4;
        V_I4(&varg1) = 0;
    }

    V_VT(pVarResult) = VT_DISPATCH;

    ret = MsiOpenPackageExW(V_BSTR(&varg0), V_I4(&varg1), &hpkg);
    if (ret != ERROR_SUCCESS)
    {
        hr = DISP_E_EXCEPTION;
        goto done;
    }

    hr = create_session(hpkg, (IDispatch *)This, &dispatch);
    if (SUCCEEDED(hr))
        V_DISPATCH(pVarResult) = dispatch;

done:
    VariantClear(&varg0);
    VariantClear(&varg1);
    return hr;
}

static HRESULT InstallerImpl_OpenProduct(WORD wFlags,
                                         DISPPARAMS* pDispParams,
                                         VARIANT* pVarResult,
                                         EXCEPINFO* pExcepInfo,
                                         UINT* puArgErr)
{
    HRESULT hr;
    VARIANTARG varg0;

    if (!(wFlags & DISPATCH_METHOD))
        return DISP_E_MEMBERNOTFOUND;

    VariantInit(&varg0);
    hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
    if (FAILED(hr))
        return hr;

    FIXME("%s\n", debugstr_w(V_BSTR(&varg0)));

    VariantInit(pVarResult);

    VariantClear(&varg0);
    return S_OK;
}

static HRESULT InstallerImpl_OpenDatabase(WORD wFlags,
                                          DISPPARAMS* pDispParams,
                                          VARIANT* pVarResult,
                                          EXCEPINFO* pExcepInfo,
                                          UINT* puArgErr)
{
    UINT ret;
    HRESULT hr;
    MSIHANDLE hdb;
    IDispatch* dispatch;
    VARIANTARG varg0, varg1;

    if (!(wFlags & DISPATCH_METHOD))
        return DISP_E_MEMBERNOTFOUND;

    VariantInit(&varg0);
    hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
    if (FAILED(hr))
        return hr;

    VariantInit(&varg1);
    hr = DispGetParam(pDispParams, 1, VT_BSTR, &varg1, puArgErr);
    if (FAILED(hr))
        goto done;

    V_VT(pVarResult) = VT_DISPATCH;

    ret = MsiOpenDatabaseW(V_BSTR(&varg0), V_BSTR(&varg1), &hdb);
    if (ret != ERROR_SUCCESS)
    {
        hr = DISP_E_EXCEPTION;
        goto done;
    }

    hr = create_automation_object(hdb, NULL, (LPVOID *)&dispatch,
                                  &DIID_Database, DatabaseImpl_Invoke, NULL, 0);
    if (SUCCEEDED(hr))
        V_DISPATCH(pVarResult) = dispatch;

done:
    VariantClear(&varg0);
    VariantClear(&varg1);
    return hr;
}

static HRESULT InstallerImpl_SummaryInformation(WORD wFlags,
                                                DISPPARAMS* pDispParams,
                                                VARIANT* pVarResult,
                                                EXCEPINFO* pExcepInfo,
                                                UINT* puArgErr)
{
    if (!(wFlags & DISPATCH_METHOD))
        return DISP_E_MEMBERNOTFOUND;

    FIXME("\n");

    VariantInit(pVarResult);
    return S_OK;
}

static HRESULT InstallerImpl_UILevel(WORD wFlags,
                                     DISPPARAMS* pDispParams,
                                     VARIANT* pVarResult,
                                     EXCEPINFO* pExcepInfo,
                                     UINT* puArgErr)
{
    HRESULT hr;
    VARIANTARG varg0;
    INSTALLUILEVEL ui;

    if (!(wFlags & DISPATCH_PROPERTYPUT) && !(wFlags & DISPATCH_PROPERTYGET))
        return DISP_E_MEMBERNOTFOUND;

    if (wFlags & DISPATCH_PROPERTYPUT)
    {
        VariantInit(&varg0);
        hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
        if (FAILED(hr))
            return hr;

        ui = MsiSetInternalUI(V_I4(&varg0), NULL);
        if (ui == INSTALLUILEVEL_NOCHANGE)
            return DISP_E_EXCEPTION;
    }
    else if (wFlags & DISPATCH_PROPERTYGET)
    {
        ui = MsiSetInternalUI(INSTALLUILEVEL_NOCHANGE, NULL);
        if (ui == INSTALLUILEVEL_NOCHANGE)
            return DISP_E_EXCEPTION;

        V_VT(pVarResult) = VT_I4;
        V_I4(pVarResult) = ui;
    }

    return S_OK;
}

static HRESULT InstallerImpl_EnableLog(WORD wFlags,
                                       DISPPARAMS* pDispParams,
                                       VARIANT* pVarResult,
                                       EXCEPINFO* pExcepInfo,
                                       UINT* puArgErr)
{
    if (!(wFlags & DISPATCH_METHOD))
        return DISP_E_MEMBERNOTFOUND;

    FIXME("\n");

    VariantInit(pVarResult);
    return S_OK;
}

static HRESULT InstallerImpl_InstallProduct(WORD wFlags,
                                            DISPPARAMS* pDispParams,
                                            VARIANT* pVarResult,
                                            EXCEPINFO* pExcepInfo,
                                            UINT* puArgErr)
{
    UINT ret;
    HRESULT hr;
    VARIANTARG varg0, varg1;

    if (!(wFlags & DISPATCH_METHOD))
        return DISP_E_MEMBERNOTFOUND;

    VariantInit(&varg0);
    hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
    if (FAILED(hr))
        return hr;

    VariantInit(&varg1);
    hr = DispGetParam(pDispParams, 1, VT_BSTR, &varg1, puArgErr);
    if (FAILED(hr))
        goto done;

    ret = MsiInstallProductW(V_BSTR(&varg0), V_BSTR(&varg1));
    if (ret != ERROR_SUCCESS)
    {
        hr = DISP_E_EXCEPTION;
        goto done;
    }

done:
    VariantClear(&varg0);
    VariantClear(&varg1);
    return hr;
}

static HRESULT InstallerImpl_Version(WORD wFlags,
                                     VARIANT* pVarResult,
                                     EXCEPINFO* pExcepInfo,
                                     UINT* puArgErr)
{
    HRESULT hr;
    DLLVERSIONINFO verinfo;
    WCHAR version[MAX_PATH];

    static const WCHAR format[] = {
        '%','d','.','%','d','.','%','d','.','%','d',0};

    if (!(wFlags & DISPATCH_PROPERTYGET))
        return DISP_E_MEMBERNOTFOUND;

    verinfo.cbSize = sizeof(DLLVERSIONINFO);
    hr = DllGetVersion(&verinfo);
    if (FAILED(hr))
        return hr;

    sprintfW(version, format, verinfo.dwMajorVersion, verinfo.dwMinorVersion,
             verinfo.dwBuildNumber, verinfo.dwPlatformID);

    V_VT(pVarResult) = VT_BSTR;
    V_BSTR(pVarResult) = SysAllocString(version);
    return S_OK;
}

static HRESULT InstallerImpl_LastErrorRecord(WORD wFlags,
                                             DISPPARAMS* pDispParams,
                                             VARIANT* pVarResult,
                                             EXCEPINFO* pExcepInfo,
                                             UINT* puArgErr)
{
    if (!(wFlags & DISPATCH_METHOD))
        return DISP_E_MEMBERNOTFOUND;

    FIXME("\n");

    VariantInit(pVarResult);
    return S_OK;
}

static HRESULT InstallerImpl_RegistryValue(WORD wFlags,
                                           DISPPARAMS* pDispParams,
                                           VARIANT* pVarResult,
                                           EXCEPINFO* pExcepInfo,
                                           UINT* puArgErr)
{
    UINT ret;
    HKEY hkey = NULL;
    HRESULT hr;
    UINT posValue;
    DWORD type, size;
    LPWSTR szString = NULL;
    VARIANTARG varg0, varg1, varg2;

    if (!(wFlags & DISPATCH_METHOD))
        return DISP_E_MEMBERNOTFOUND;

    VariantInit(&varg0);
    hr = DispGetParam(pDispParams, 0, VT_I4, &varg0, puArgErr);
    if (FAILED(hr))
        return hr;

    VariantInit(&varg1);
    hr = DispGetParam(pDispParams, 1, VT_BSTR, &varg1, puArgErr);
    if (FAILED(hr))
        goto done;

    /* Save valuePos so we can save puArgErr if we are unable to do our type
     * conversions.
     */
    posValue = 2;
    VariantInit(&varg2);
    hr = DispGetParam_CopyOnly(pDispParams, &posValue, &varg2);
    if (FAILED(hr))
        goto done;

    if (V_I4(&varg0) >= REG_INDEX_CLASSES_ROOT &&
        V_I4(&varg0) <= REG_INDEX_DYN_DATA)
    {
        V_I4(&varg0) |= (UINT_PTR)HKEY_CLASSES_ROOT;
    }

    ret = RegOpenKeyW((HKEY)(UINT_PTR)V_I4(&varg0), V_BSTR(&varg1), &hkey);

    /* Only VT_EMPTY case can do anything if the key doesn't exist. */
    if (ret != ERROR_SUCCESS && V_VT(&varg2) != VT_EMPTY)
    {
        hr = DISP_E_BADINDEX;
        goto done;
    }

    /* Third parameter can be VT_EMPTY, VT_I4, or VT_BSTR */
    switch (V_VT(&varg2))
    {
        /* Return VT_BOOL clarifying whether registry key exists or not. */
        case VT_EMPTY:
            V_VT(pVarResult) = VT_BOOL;
            V_BOOL(pVarResult) = (ret == ERROR_SUCCESS);
            break;

        /* Return the value of specified key if it exists. */
        case VT_BSTR:
            ret = RegQueryValueExW(hkey, V_BSTR(&varg2),
                                   NULL, NULL, NULL, &size);
            if (ret != ERROR_SUCCESS)
            {
                hr = DISP_E_BADINDEX;
                goto done;
            }

            szString = msi_alloc(size);
            if (!szString)
            {
                hr = E_OUTOFMEMORY;
                goto done;
            }

            ret = RegQueryValueExW(hkey, V_BSTR(&varg2), NULL,
                                   &type, (LPBYTE)szString, &size);
            if (ret != ERROR_SUCCESS)
            {
                msi_free(szString);
                hr = DISP_E_BADINDEX;
                goto done;
            }

            variant_from_registry_value(pVarResult, type,
                                        (LPBYTE)szString, size);
            msi_free(szString);
            break;

        /* Try to make it into VT_I4, can use VariantChangeType for this. */
        default:
            hr = VariantChangeType(&varg2, &varg2, 0, VT_I4);
            if (FAILED(hr))
            {
                if (hr == DISP_E_TYPEMISMATCH)
                    *puArgErr = posValue;

                goto done;
            }

            /* Retrieve class name or maximum value name or subkey name size. */
            if (!V_I4(&varg2))
                ret = RegQueryInfoKeyW(hkey, NULL, &size, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL);
            else if (V_I4(&varg2) > 0)
                ret = RegQueryInfoKeyW(hkey, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, &size, NULL, NULL, NULL);
            else /* V_I4(&varg2) < 0 */
                ret = RegQueryInfoKeyW(hkey, NULL, NULL, NULL, NULL, &size,
                                       NULL, NULL, NULL, NULL, NULL, NULL);

            if (ret != ERROR_SUCCESS)
                goto done;

            szString = msi_alloc(++size * sizeof(WCHAR));
            if (!szString)
            {
                hr = E_OUTOFMEMORY;
                goto done;
            }

            if (!V_I4(&varg2))
                ret = RegQueryInfoKeyW(hkey, szString, &size,NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL);
            else if (V_I4(&varg2) > 0)
                ret = RegEnumValueW(hkey, V_I4(&varg2)-1, szString,
                                    &size, 0, 0, NULL, NULL);
            else /* V_I4(&varg2) < 0 */
                ret = RegEnumKeyW(hkey, -1 - V_I4(&varg2), szString, size);

            if (ret == ERROR_SUCCESS)
            {
                V_VT(pVarResult) = VT_BSTR;
                V_BSTR(pVarResult) = SysAllocString(szString);
            }

            msi_free(szString);
    }

done:
    VariantClear(&varg0);
    VariantClear(&varg1);
    VariantClear(&varg2);
    RegCloseKey(hkey);
    return hr;
}

static HRESULT InstallerImpl_Environment(WORD wFlags,
                                         DISPPARAMS* pDispParams,
                                         VARIANT* pVarResult,
                                         EXCEPINFO* pExcepInfo,
                                         UINT* puArgErr)
{
    if (!(wFlags & DISPATCH_METHOD))
        return DISP_E_MEMBERNOTFOUND;

    FIXME("\n");

    VariantInit(pVarResult);
    return S_OK;
}

static HRESULT InstallerImpl_FileAttributes(WORD wFlags,
                                            DISPPARAMS* pDispParams,
                                            VARIANT* pVarResult,
                                            EXCEPINFO* pExcepInfo,
                                            UINT* puArgErr)
{
    if (!(wFlags & DISPATCH_METHOD))
        return DISP_E_MEMBERNOTFOUND;

    FIXME("\n");

    VariantInit(pVarResult);
    return S_OK;
}

static HRESULT InstallerImpl_FileSize(WORD wFlags,
                                      DISPPARAMS* pDispParams,
                                      VARIANT* pVarResult,
                                      EXCEPINFO* pExcepInfo,
                                      UINT* puArgErr)
{
    if (!(wFlags & DISPATCH_METHOD))
        return DISP_E_MEMBERNOTFOUND;

    FIXME("\n");

    VariantInit(pVarResult);
    return S_OK;
}

static HRESULT InstallerImpl_FileVersion(WORD wFlags,
                                         DISPPARAMS* pDispParams,
                                         VARIANT* pVarResult,
                                         EXCEPINFO* pExcepInfo,
                                         UINT* puArgErr)
{
    if (!(wFlags & DISPATCH_METHOD))
        return DISP_E_MEMBERNOTFOUND;

    FIXME("\n");

    VariantInit(pVarResult);
    return S_OK;
}

static HRESULT InstallerImpl_ProductState(WORD wFlags,
                                          DISPPARAMS* pDispParams,
                                          VARIANT* pVarResult,
                                          EXCEPINFO* pExcepInfo,
                                          UINT* puArgErr)
{
    HRESULT hr;
    VARIANTARG varg0;

    if (!(wFlags & DISPATCH_PROPERTYGET))
        return DISP_E_MEMBERNOTFOUND;

    VariantInit(&varg0);
    hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
    if (FAILED(hr))
        return hr;

    V_VT(pVarResult) = VT_I4;
    V_I4(pVarResult) = MsiQueryProductStateW(V_BSTR(&varg0));

    VariantClear(&varg0);
    return S_OK;
}

static HRESULT InstallerImpl_ProductInfo(WORD wFlags,
                                         DISPPARAMS* pDispParams,
                                         VARIANT* pVarResult,
                                         EXCEPINFO* pExcepInfo,
                                         UINT* puArgErr)
{
    UINT ret;
    HRESULT hr;
    DWORD size;
    LPWSTR str = NULL;
    VARIANTARG varg0, varg1;

    if (!(wFlags & DISPATCH_PROPERTYGET))
        return DISP_E_MEMBERNOTFOUND;

    VariantInit(&varg0);
    hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
    if (FAILED(hr))
        return hr;

    VariantInit(&varg1);
    hr = DispGetParam(pDispParams, 1, VT_BSTR, &varg1, puArgErr);
    if (FAILED(hr))
        goto done;

    V_VT(pVarResult) = VT_BSTR;
    V_BSTR(pVarResult) = NULL;

    ret = MsiGetProductInfoW(V_BSTR(&varg0), V_BSTR(&varg1), NULL, &size);
    if (ret != ERROR_SUCCESS)
    {
        hr = DISP_E_EXCEPTION;
        goto done;
    }

    str = msi_alloc(++size * sizeof(WCHAR));
    if (!str)
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }

    ret = MsiGetProductInfoW(V_BSTR(&varg0), V_BSTR(&varg1), str, &size);
    if (ret != ERROR_SUCCESS)
    {
        hr = DISP_E_EXCEPTION;
        goto done;
    }

    V_BSTR(pVarResult) = SysAllocString(str);
    hr = S_OK;

done:
    msi_free(str);
    VariantClear(&varg0);
    VariantClear(&varg1);
    return hr;
}

static void cleanup_products(IDispatch* dispatch, ULONG count)
{
    UINT i;
    ListData* ldata = private_data((AutomationObject *)dispatch);

    for (i = 0; i < count - 1; i++)
        VariantClear(&ldata->pVars[i]);

    ldata->ulCount = 0;
    msi_free(ldata->pVars);

    IDispatch_Release(dispatch);
}

static HRESULT InstallerImpl_Products(WORD wFlags,
                                      DISPPARAMS* pDispParams,
                                      VARIANT* pVarResult,
                                      EXCEPINFO* pExcepInfo,
                                      UINT* puArgErr)
{
    UINT ret;
    HRESULT hr;
    ULONG idx = 0;
    ListData *ldata;
    IDispatch *dispatch;
    WCHAR product[GUID_SIZE];

    if (!(wFlags & DISPATCH_PROPERTYGET))
        return DISP_E_MEMBERNOTFOUND;

    /* Find number of products. */
    while ((ret = MsiEnumProductsW(idx, product)) == ERROR_SUCCESS)
        idx++;

    if (ret != ERROR_NO_MORE_ITEMS)
        return DISP_E_EXCEPTION;

    V_VT(pVarResult) = VT_DISPATCH;
    hr = create_automation_object(0, NULL, (LPVOID*)&dispatch,
                                  &DIID_StringList, ListImpl_Invoke,
                                  ListImpl_Free, sizeof(ListData));
    if (FAILED(hr))
        return hr;

    V_DISPATCH(pVarResult) = dispatch;

    /* Save product strings. */
    ldata = private_data((AutomationObject *)dispatch);
    ldata->ulCount = 0;
    ldata->pVars = msi_alloc_zero(sizeof(VARIANT) * idx);
    if (!ldata->pVars)
    {
        IDispatch_Release(dispatch);
        return E_OUTOFMEMORY;
    }

    ldata->ulCount = idx;
    for (idx = 0; idx < ldata->ulCount; idx++)
    {
        ret = MsiEnumProductsW(idx, product);
        if (ret != ERROR_SUCCESS)
        {
            cleanup_products(dispatch, idx - 1);
            return DISP_E_EXCEPTION;
        }

        VariantInit(&ldata->pVars[idx]);
        V_VT(&ldata->pVars[idx]) = VT_BSTR;
        V_BSTR(&ldata->pVars[idx]) = SysAllocString(product);
    }

    return S_OK;
}

static HRESULT InstallerImpl_RelatedProducts(WORD wFlags,
                                             DISPPARAMS* pDispParams,
                                             VARIANT* pVarResult,
                                             EXCEPINFO* pExcepInfo,
                                             UINT* puArgErr)
{
    UINT ret;
    ULONG idx;
    HRESULT hr;
    ListData *ldata;
    VARIANTARG varg0;
    IDispatch* dispatch;
    WCHAR product[GUID_SIZE];

    if (!(wFlags & DISPATCH_PROPERTYGET))
        return DISP_E_MEMBERNOTFOUND;

    VariantInit(&varg0);
    hr = DispGetParam(pDispParams, 0, VT_BSTR, &varg0, puArgErr);
    if (FAILED(hr))
        return hr;

    /* Find number of related products. */
    idx = 0;
    do
    {
        ret = MsiEnumRelatedProductsW(V_BSTR(&varg0), 0, idx, product);
        if (ret == ERROR_SUCCESS)
            idx++;
    } while (ret == ERROR_SUCCESS);

    if (ret != ERROR_NO_MORE_ITEMS)
    {
        hr = DISP_E_EXCEPTION;
        goto done;
    }

    V_VT(pVarResult) = VT_DISPATCH;

    hr = create_automation_object(0, NULL, (LPVOID*)&dispatch,
                                  &DIID_StringList, ListImpl_Invoke,
                                  ListImpl_Free, sizeof(ListData));
    if (FAILED(hr))
        goto done;

    V_DISPATCH(pVarResult) = dispatch;

    /* Save product strings. */
    ldata = private_data((AutomationObject *)dispatch);
    ldata->pVars = msi_alloc(sizeof(VARIANT) * idx);
    if (!ldata->pVars)
    {
        IDispatch_Release(dispatch);
        hr = E_OUTOFMEMORY;
        goto done;
    }

    ldata->ulCount = idx;
    for (idx = 0; idx < ldata->ulCount; idx++)
    {
        ret = MsiEnumRelatedProductsW(V_BSTR(&varg0), 0, idx, product);
        if (ret != ERROR_SUCCESS)
        {
            cleanup_products(dispatch, idx - 1);
            hr = DISP_E_EXCEPTION;
            goto done;
        }

        VariantInit(&ldata->pVars[idx]);
        V_VT(&ldata->pVars[idx]) = VT_BSTR;
        V_BSTR(&ldata->pVars[idx]) = SysAllocString(product);
    }

    hr = S_OK;

done:
    VariantClear(&varg0);
    return hr;
}

static HRESULT InstallerImpl_Invoke(
        AutomationObject* This,
        DISPID dispIdMember,
        REFIID riid,
        LCID lcid,
        WORD wFlags,
        DISPPARAMS* pDispParams,
        VARIANT* pVarResult,
        EXCEPINFO* pExcepInfo,
        UINT* puArgErr)
{
    switch (dispIdMember)
    {
        case DISPID_INSTALLER_CREATERECORD:
            return InstallerImpl_CreateRecord(wFlags, pDispParams,
                                              pVarResult, pExcepInfo, puArgErr);

        case DISPID_INSTALLER_OPENPACKAGE:
            return InstallerImpl_OpenPackage(This, wFlags, pDispParams,
                                             pVarResult, pExcepInfo, puArgErr);

        case DISPID_INSTALLER_OPENPRODUCT:
            return InstallerImpl_OpenProduct(wFlags, pDispParams,
                                             pVarResult, pExcepInfo, puArgErr);

        case DISPID_INSTALLER_OPENDATABASE:
            return InstallerImpl_OpenDatabase(wFlags, pDispParams,
                                              pVarResult, pExcepInfo, puArgErr);

        case DISPID_INSTALLER_SUMMARYINFORMATION:
            return InstallerImpl_SummaryInformation(wFlags, pDispParams,
                                                    pVarResult, pExcepInfo,
                                                    puArgErr);

        case DISPID_INSTALLER_UILEVEL:
            return InstallerImpl_UILevel(wFlags, pDispParams,
                                         pVarResult, pExcepInfo, puArgErr);

        case DISPID_INSTALLER_ENABLELOG:
            return InstallerImpl_EnableLog(wFlags, pDispParams,
                                           pVarResult, pExcepInfo, puArgErr);

        case DISPID_INSTALLER_INSTALLPRODUCT:
            return InstallerImpl_InstallProduct(wFlags, pDispParams,
                                                pVarResult, pExcepInfo,
                                                puArgErr);

        case DISPID_INSTALLER_VERSION:
            return InstallerImpl_Version(wFlags, pVarResult,
                                         pExcepInfo, puArgErr);

        case DISPID_INSTALLER_LASTERRORRECORD:
            return InstallerImpl_LastErrorRecord(wFlags, pDispParams,
                                                 pVarResult, pExcepInfo,
                                                 puArgErr);

        case DISPID_INSTALLER_REGISTRYVALUE:
            return InstallerImpl_RegistryValue(wFlags, pDispParams,
                                               pVarResult, pExcepInfo,
                                               puArgErr);

        case DISPID_INSTALLER_ENVIRONMENT:
            return InstallerImpl_Environment(wFlags, pDispParams,
                                             pVarResult, pExcepInfo, puArgErr);

        case DISPID_INSTALLER_FILEATTRIBUTES:
            return InstallerImpl_FileAttributes(wFlags, pDispParams,
                                                pVarResult, pExcepInfo,
                                                puArgErr);

        case DISPID_INSTALLER_FILESIZE:
            return InstallerImpl_FileSize(wFlags, pDispParams,
                                          pVarResult, pExcepInfo, puArgErr);

        case DISPID_INSTALLER_FILEVERSION:
            return InstallerImpl_FileVersion(wFlags, pDispParams,
                                             pVarResult, pExcepInfo, puArgErr);

        case DISPID_INSTALLER_PRODUCTSTATE:
            return InstallerImpl_ProductState(wFlags, pDispParams,
                                              pVarResult, pExcepInfo, puArgErr);

        case DISPID_INSTALLER_PRODUCTINFO:
            return InstallerImpl_ProductInfo(wFlags, pDispParams,
                                             pVarResult, pExcepInfo, puArgErr);

        case DISPID_INSTALLER_PRODUCTS:
            return InstallerImpl_Products(wFlags, pDispParams,
                                          pVarResult, pExcepInfo, puArgErr);

        case DISPID_INSTALLER_RELATEDPRODUCTS:
            return InstallerImpl_RelatedProducts(wFlags, pDispParams,
                                                 pVarResult, pExcepInfo,
                                                 puArgErr);

        default:
            return DISP_E_MEMBERNOTFOUND;
    }
}

/* Wrapper around create_automation_object to create an installer object. */
HRESULT create_msiserver(IUnknown *pOuter, LPVOID *ppObj)
{
    return create_automation_object(0, pOuter, ppObj, &DIID_Installer, InstallerImpl_Invoke, NULL, 0);
}

/* Wrapper around create_automation_object to create a session object. */
HRESULT create_session(MSIHANDLE msiHandle, IDispatch *pInstaller, IDispatch **pDispatch)
{
    HRESULT hr = create_automation_object(msiHandle, NULL, (LPVOID)pDispatch, &DIID_Session, SessionImpl_Invoke, NULL, sizeof(SessionData));
    if (SUCCEEDED(hr) && pDispatch && *pDispatch)
        ((SessionData *)private_data((AutomationObject *)*pDispatch))->pInstaller = pInstaller;
    return hr;
}
