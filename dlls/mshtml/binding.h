/*
 * Copyright 2011 Jacek Caban for CodeWeavers
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

typedef struct nsWineURI nsWineURI;

/* Keep sync with request_method_strings in nsio.c */
typedef enum {
    METHOD_GET,
    METHOD_PUT,
    METHOD_POST
} REQUEST_METHOD;

typedef struct {
    nsIHttpChannel         nsIHttpChannel_iface;
    nsIUploadChannel       nsIUploadChannel_iface;
    nsIHttpChannelInternal nsIHttpChannelInternal_iface;

    LONG ref;

    nsWineURI *uri;
    nsIInputStream *post_data_stream;
    BOOL post_data_contains_headers;
    nsILoadGroup *load_group;
    nsIInterfaceRequestor *notif_callback;
    nsISupports *owner;
    nsLoadFlags load_flags;
    nsIURI *original_uri;
    nsIURI *referrer;
    char *content_type;
    char *charset;
    PRUint32 response_status;
    REQUEST_METHOD request_method;
    struct list response_headers;
    struct list request_headers;
    UINT url_scheme;
} nsChannel;

typedef struct BSCallbackVtbl BSCallbackVtbl;

struct BSCallback {
    IBindStatusCallback IBindStatusCallback_iface;
    IServiceProvider    IServiceProvider_iface;
    IHttpNegotiate2     IHttpNegotiate2_iface;
    IInternetBindInfo   IInternetBindInfo_iface;

    const BSCallbackVtbl          *vtbl;

    LONG ref;

    LPWSTR headers;
    HGLOBAL post_data;
    ULONG post_data_len;
    ULONG readed;
    DWORD bindf;
    BOOL bindinfo_ready;

    IMoniker *mon;
    IBinding *binding;

    HTMLDocumentNode *doc;

    struct list entry;
};

typedef struct nsProtocolStream nsProtocolStream;

struct nsChannelBSC {
    BSCallback bsc;

    HTMLWindow *window;

    nsChannel *nschannel;
    nsIStreamListener *nslistener;
    nsISupports *nscontext;
    BOOL is_js;

    nsProtocolStream *nsstream;
};

typedef struct {
    struct list entry;
    WCHAR *header;
    WCHAR *data;
} http_header_t;

HRESULT set_http_header(struct list*,const WCHAR*,int,const WCHAR*,int) DECLSPEC_HIDDEN;
HRESULT create_redirect_nschannel(const WCHAR*,nsChannel*,nsChannel**) DECLSPEC_HIDDEN;

nsresult on_start_uri_open(NSContainer*,nsIURI*,PRBool*) DECLSPEC_HIDDEN;
HRESULT hlink_frame_navigate(HTMLDocument*,LPCWSTR,nsChannel*,DWORD,BOOL*) DECLSPEC_HIDDEN;
HRESULT create_doc_uri(HTMLWindow*,WCHAR*,nsWineURI**) DECLSPEC_HIDDEN;
HRESULT load_nsuri(HTMLWindow*,nsWineURI*,nsChannelBSC*,DWORD) DECLSPEC_HIDDEN;
HRESULT set_moniker(HTMLDocument*,IMoniker*,IBindCtx*,nsChannelBSC*,BOOL) DECLSPEC_HIDDEN;
void prepare_for_binding(HTMLDocument*,IMoniker*,IBindCtx*,BOOL) DECLSPEC_HIDDEN;

HRESULT create_channelbsc(IMoniker*,WCHAR*,BYTE*,DWORD,nsChannelBSC**) DECLSPEC_HIDDEN;
HRESULT channelbsc_load_stream(nsChannelBSC*,IStream*) DECLSPEC_HIDDEN;
void channelbsc_set_channel(nsChannelBSC*,nsChannel*,nsIStreamListener*,nsISupports*) DECLSPEC_HIDDEN;
