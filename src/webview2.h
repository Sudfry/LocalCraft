/* ==========================================================================
 * webview2.h —— WebView2 COM 接口的**手写声明**
 *
 * 为什么自己写
 * ------------
 * MinGW-w64 不自带 webview2.h，而本项目坚持"零第三方依赖、不联网拉包"。
 * 好在 C 语言调用 COM 需要的只是**vtable 的内存布局**，把接口按顺序抄一遍就行：
 * 每个接口就是"一个指向函数指针数组的指针"。
 *
 * 抄写要点（错一个就崩，且不报错）
 * --------------------------------
 *   · IUnknown 的三个方法必须排在最前面
 *   · 方法顺序必须与官方头文件完全一致 —— 不能因为"用不到"就把中间某个删掉，
 *     那会让后面所有方法的索引整体前移，调用时跳到错误的函数上
 *   · x64 只有一个调用约定，__stdcall 是空操作，所以不用纠结修饰符
 *   · 只声明到"用到的最后一个方法"为止；再往后的可以省略
 *
 * 这份声明只覆盖本程序实际要用的部分。
 * ========================================================================== */
#ifndef LC_WEBVIEW2_H
#define LC_WEBVIEW2_H

#include <windows.h>
#include <objidl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 基础类型 ---- */

typedef struct ICoreWebView2Environment ICoreWebView2Environment;
typedef struct ICoreWebView2Controller  ICoreWebView2Controller;
typedef struct ICoreWebView2            ICoreWebView2;
typedef struct ICoreWebView2Settings    ICoreWebView2Settings;

typedef struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;
typedef struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
typedef struct ICoreWebView2WebResourceRequestedEventHandler
        ICoreWebView2WebResourceRequestedEventHandler;
typedef struct ICoreWebView2NewWindowRequestedEventHandler
        ICoreWebView2NewWindowRequestedEventHandler;

typedef struct ICoreWebView2WebResourceRequest           ICoreWebView2WebResourceRequest;
typedef struct ICoreWebView2WebResourceResponse          ICoreWebView2WebResourceResponse;
typedef struct ICoreWebView2WebResourceRequestedEventArgs
        ICoreWebView2WebResourceRequestedEventArgs;
typedef struct ICoreWebView2NewWindowRequestedEventArgs
        ICoreWebView2NewWindowRequestedEventArgs;

#ifndef EVENTREGISTRATIONTOKEN_DEFINED
#define EVENTREGISTRATIONTOKEN_DEFINED
typedef struct EventRegistrationToken { LONGLONG value; } EventRegistrationToken;
#endif

typedef struct COREWEBVIEW2_COLOR {
    BYTE A, R, G, B;
} COREWEBVIEW2_COLOR;

/* 资源类型：我们拦截全部，只处理自己的虚拟域 */
typedef enum COREWEBVIEW2_WEB_RESOURCE_CONTEXT {
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL = 0,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT = 1,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_STYLESHEET = 2,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE = 3,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_MEDIA = 4,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FONT = 5,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT = 6,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_XML_HTTP_REQUEST = 7,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FETCH = 8,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_TEXT_TRACK = 9,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_EVENT_SOURCE = 10,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_WEBSOCKET = 11,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_MANIFEST = 12,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SIGNED_EXCHANGE = 13,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_PING = 14,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_CSP_VIOLATION_REPORT = 15,
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_OTHER = 16
} COREWEBVIEW2_WEB_RESOURCE_CONTEXT;

typedef enum COREWEBVIEW2_MOVE_FOCUS_REASON {
    COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC = 0,
    COREWEBVIEW2_MOVE_FOCUS_REASON_NEXT = 1,
    COREWEBVIEW2_MOVE_FOCUS_REASON_PREVIOUS = 2
} COREWEBVIEW2_MOVE_FOCUS_REASON;

/* ============================================ 回调接口 =================== */

/* 虚表顺序：QueryInterface / AddRef / Release / Invoke */
/* 回调接口的虚表：前面三个是 IUnknown，最后一个是 Invoke。
   Invoke 的参数个数各接口不同，所以用可变参数。 */
#define LC_DECL_HANDLER_VTBL(name, iface, ...)                                 \
    typedef struct name##Vtbl {                                                \
        HRESULT (STDMETHODCALLTYPE *QueryInterface)(iface *, REFIID, void **); \
        ULONG   (STDMETHODCALLTYPE *AddRef)(iface *);                          \
        ULONG   (STDMETHODCALLTYPE *Release)(iface *);                         \
        HRESULT (STDMETHODCALLTYPE *Invoke)(iface *, __VA_ARGS__);             \
    } name##Vtbl

LC_DECL_HANDLER_VTBL(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                     ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                     HRESULT, ICoreWebView2Environment *);

LC_DECL_HANDLER_VTBL(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                     ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                     HRESULT, ICoreWebView2Controller *);

LC_DECL_HANDLER_VTBL(ICoreWebView2WebResourceRequestedEventHandler,
                     ICoreWebView2WebResourceRequestedEventHandler,
                     ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *);

LC_DECL_HANDLER_VTBL(ICoreWebView2NewWindowRequestedEventHandler,
                     ICoreWebView2NewWindowRequestedEventHandler,
                     ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *);

struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl *lpVtbl;
};
struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl *lpVtbl;
};
struct ICoreWebView2WebResourceRequestedEventHandler {
    ICoreWebView2WebResourceRequestedEventHandlerVtbl *lpVtbl;
};
struct ICoreWebView2NewWindowRequestedEventHandler {
    ICoreWebView2NewWindowRequestedEventHandlerVtbl *lpVtbl;
};

/* ============================================ 环境 ======================= */

typedef struct ICoreWebView2EnvironmentVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2Environment *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2Environment *);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2Environment *);

    HRESULT (STDMETHODCALLTYPE *CreateCoreWebView2Controller)(
        ICoreWebView2Environment *, HWND parentWindow,
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *handler);

    HRESULT (STDMETHODCALLTYPE *CreateWebResourceResponse)(
        ICoreWebView2Environment *, IStream *content, int statusCode,
        LPCWSTR reasonPhrase, LPCWSTR headers,
        ICoreWebView2WebResourceResponse **response);

    HRESULT (STDMETHODCALLTYPE *get_BrowserVersionString)(
        ICoreWebView2Environment *, LPWSTR *versionInfo);

    HRESULT (STDMETHODCALLTYPE *add_NewBrowserVersionAvailable)(
        ICoreWebView2Environment *,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *,  /* 占位：实际是别的事件接口 */
        EventRegistrationToken *token);

    HRESULT (STDMETHODCALLTYPE *remove_NewBrowserVersionAvailable)(
        ICoreWebView2Environment *, EventRegistrationToken token);
} ICoreWebView2EnvironmentVtbl;

struct ICoreWebView2Environment { ICoreWebView2EnvironmentVtbl *lpVtbl; };

/* ============================================ 控制器 ===================== */

typedef struct ICoreWebView2ControllerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2Controller *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2Controller *);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2Controller *);

    HRESULT (STDMETHODCALLTYPE *get_IsVisible)(ICoreWebView2Controller *, BOOL *isVisible);
    HRESULT (STDMETHODCALLTYPE *put_IsVisible)(ICoreWebView2Controller *, BOOL isVisible);
    HRESULT (STDMETHODCALLTYPE *get_Bounds)(ICoreWebView2Controller *, RECT *bounds);
    HRESULT (STDMETHODCALLTYPE *put_Bounds)(ICoreWebView2Controller *, RECT bounds);
    HRESULT (STDMETHODCALLTYPE *get_ZoomFactor)(ICoreWebView2Controller *, double *zoomFactor);
    HRESULT (STDMETHODCALLTYPE *put_ZoomFactor)(ICoreWebView2Controller *, double zoomFactor);
    HRESULT (STDMETHODCALLTYPE *add_ZoomFactorChanged)(
        ICoreWebView2Controller *, void *handler, EventRegistrationToken *token);
    HRESULT (STDMETHODCALLTYPE *remove_ZoomFactorChanged)(
        ICoreWebView2Controller *, EventRegistrationToken token);
    HRESULT (STDMETHODCALLTYPE *SetBoundsAndZoomFactor)(ICoreWebView2Controller *, RECT bounds, double zoomFactor);
    HRESULT (STDMETHODCALLTYPE *MoveFocus)(
        ICoreWebView2Controller *, COREWEBVIEW2_MOVE_FOCUS_REASON reason);
    HRESULT (STDMETHODCALLTYPE *add_MoveFocusRequested)(
        ICoreWebView2Controller *, void *handler, EventRegistrationToken *token);
    HRESULT (STDMETHODCALLTYPE *remove_MoveFocusRequested)(
        ICoreWebView2Controller *, EventRegistrationToken token);
    HRESULT (STDMETHODCALLTYPE *add_GotFocus)(
        ICoreWebView2Controller *, void *handler, EventRegistrationToken *token);
    HRESULT (STDMETHODCALLTYPE *remove_GotFocus)(
        ICoreWebView2Controller *, EventRegistrationToken token);
    HRESULT (STDMETHODCALLTYPE *add_LostFocus)(
        ICoreWebView2Controller *, void *handler, EventRegistrationToken *token);
    HRESULT (STDMETHODCALLTYPE *remove_LostFocus)(
        ICoreWebView2Controller *, EventRegistrationToken token);
    HRESULT (STDMETHODCALLTYPE *add_AcceleratorKeyPressed)(
        ICoreWebView2Controller *, void *handler, EventRegistrationToken *token);
    HRESULT (STDMETHODCALLTYPE *remove_AcceleratorKeyPressed)(
        ICoreWebView2Controller *, EventRegistrationToken token);
    HRESULT (STDMETHODCALLTYPE *get_ParentWindow)(ICoreWebView2Controller *, HWND *parentWindow);
    HRESULT (STDMETHODCALLTYPE *put_ParentWindow)(ICoreWebView2Controller *, HWND parentWindow);
    HRESULT (STDMETHODCALLTYPE *NotifyParentWindowPositionChanged)(ICoreWebView2Controller *);
    HRESULT (STDMETHODCALLTYPE *Close)(ICoreWebView2Controller *);
    /*
     * get_CoreWebView2 在最后 —— 这点很容易记错。
     * 我第一版把它放在 Close 之前，结果调用落到了 NotifyParentWindowPositionChanged 上：
     * 那个方法返回 S_OK 但什么都不写，于是拿到的指针是 NULL，
     * 界面就是一片空白，而且**不报任何错**。
     */
    HRESULT (STDMETHODCALLTYPE *get_CoreWebView2)(
        ICoreWebView2Controller *, ICoreWebView2 **coreWebView2);
} ICoreWebView2ControllerVtbl;

struct ICoreWebView2Controller { ICoreWebView2ControllerVtbl *lpVtbl; };

/* ============================================ 设置 ======================= */

typedef struct ICoreWebView2SettingsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2Settings *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2Settings *);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2Settings *);

    HRESULT (STDMETHODCALLTYPE *get_IsScriptEnabled)(ICoreWebView2Settings *, BOOL *);
    HRESULT (STDMETHODCALLTYPE *put_IsScriptEnabled)(ICoreWebView2Settings *, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_IsWebMessageEnabled)(ICoreWebView2Settings *, BOOL *);
    HRESULT (STDMETHODCALLTYPE *put_IsWebMessageEnabled)(ICoreWebView2Settings *, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_AreDefaultScriptDialogsEnabled)(ICoreWebView2Settings *, BOOL *);
    HRESULT (STDMETHODCALLTYPE *put_AreDefaultScriptDialogsEnabled)(ICoreWebView2Settings *, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_IsStatusBarEnabled)(ICoreWebView2Settings *, BOOL *);
    HRESULT (STDMETHODCALLTYPE *put_IsStatusBarEnabled)(ICoreWebView2Settings *, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_AreDevToolsEnabled)(ICoreWebView2Settings *, BOOL *);
    HRESULT (STDMETHODCALLTYPE *put_AreDevToolsEnabled)(ICoreWebView2Settings *, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_AreDefaultContextMenusEnabled)(ICoreWebView2Settings *, BOOL *);
    HRESULT (STDMETHODCALLTYPE *put_AreDefaultContextMenusEnabled)(ICoreWebView2Settings *, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_AreHostObjectsAllowed)(ICoreWebView2Settings *, BOOL *);
    HRESULT (STDMETHODCALLTYPE *put_AreHostObjectsAllowed)(ICoreWebView2Settings *, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_IsZoomControlEnabled)(ICoreWebView2Settings *, BOOL *);
    HRESULT (STDMETHODCALLTYPE *put_IsZoomControlEnabled)(ICoreWebView2Settings *, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_IsBuiltInErrorPageEnabled)(ICoreWebView2Settings *, BOOL *);
    HRESULT (STDMETHODCALLTYPE *put_IsBuiltInErrorPageEnabled)(ICoreWebView2Settings *, BOOL);
} ICoreWebView2SettingsVtbl;

struct ICoreWebView2Settings { ICoreWebView2SettingsVtbl *lpVtbl; };

/* ============================================ 请求 / 响应 ================ */

typedef struct ICoreWebView2WebResourceRequestVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2WebResourceRequest *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2WebResourceRequest *);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2WebResourceRequest *);

    HRESULT (STDMETHODCALLTYPE *get_Uri)(ICoreWebView2WebResourceRequest *, LPWSTR *uri);
    HRESULT (STDMETHODCALLTYPE *put_Uri)(ICoreWebView2WebResourceRequest *, LPCWSTR uri);
    HRESULT (STDMETHODCALLTYPE *get_Method)(ICoreWebView2WebResourceRequest *, LPWSTR *method);
    HRESULT (STDMETHODCALLTYPE *put_Method)(ICoreWebView2WebResourceRequest *, LPCWSTR method);
    HRESULT (STDMETHODCALLTYPE *get_Content)(ICoreWebView2WebResourceRequest *, IStream **content);
    HRESULT (STDMETHODCALLTYPE *put_Content)(ICoreWebView2WebResourceRequest *, IStream *content);
    HRESULT (STDMETHODCALLTYPE *get_Headers)(ICoreWebView2WebResourceRequest *, void **headers);
} ICoreWebView2WebResourceRequestVtbl;

struct ICoreWebView2WebResourceRequest { ICoreWebView2WebResourceRequestVtbl *lpVtbl; };

typedef struct ICoreWebView2WebResourceResponseVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2WebResourceResponse *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2WebResourceResponse *);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2WebResourceResponse *);

    HRESULT (STDMETHODCALLTYPE *get_Content)(ICoreWebView2WebResourceResponse *, IStream **content);
    HRESULT (STDMETHODCALLTYPE *put_Content)(ICoreWebView2WebResourceResponse *, IStream *content);
    HRESULT (STDMETHODCALLTYPE *get_Headers)(ICoreWebView2WebResourceResponse *, void **headers);
    HRESULT (STDMETHODCALLTYPE *get_StatusCode)(ICoreWebView2WebResourceResponse *, int *statusCode);
    HRESULT (STDMETHODCALLTYPE *put_StatusCode)(ICoreWebView2WebResourceResponse *, int statusCode);
    HRESULT (STDMETHODCALLTYPE *get_ReasonPhrase)(ICoreWebView2WebResourceResponse *, LPWSTR *reasonPhrase);
    HRESULT (STDMETHODCALLTYPE *put_ReasonPhrase)(ICoreWebView2WebResourceResponse *, LPCWSTR reasonPhrase);
} ICoreWebView2WebResourceResponseVtbl;

struct ICoreWebView2WebResourceResponse { ICoreWebView2WebResourceResponseVtbl *lpVtbl; };

typedef struct ICoreWebView2WebResourceRequestedEventArgsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2WebResourceRequestedEventArgs *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2WebResourceRequestedEventArgs *);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2WebResourceRequestedEventArgs *);

    HRESULT (STDMETHODCALLTYPE *get_Request)(
        ICoreWebView2WebResourceRequestedEventArgs *, ICoreWebView2WebResourceRequest **request);
    HRESULT (STDMETHODCALLTYPE *get_Response)(
        ICoreWebView2WebResourceRequestedEventArgs *, ICoreWebView2WebResourceResponse **response);
    HRESULT (STDMETHODCALLTYPE *put_Response)(
        ICoreWebView2WebResourceRequestedEventArgs *, ICoreWebView2WebResourceResponse *response);
    HRESULT (STDMETHODCALLTYPE *GetDeferral)(
        ICoreWebView2WebResourceRequestedEventArgs *, void **deferral);
} ICoreWebView2WebResourceRequestedEventArgsVtbl;

struct ICoreWebView2WebResourceRequestedEventArgs {
    ICoreWebView2WebResourceRequestedEventArgsVtbl *lpVtbl;
};

typedef struct ICoreWebView2NewWindowRequestedEventArgsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2NewWindowRequestedEventArgs *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2NewWindowRequestedEventArgs *);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2NewWindowRequestedEventArgs *);

    HRESULT (STDMETHODCALLTYPE *get_Uri)(ICoreWebView2NewWindowRequestedEventArgs *, LPWSTR *uri);
    HRESULT (STDMETHODCALLTYPE *put_NewWindow)(ICoreWebView2NewWindowRequestedEventArgs *, ICoreWebView2 *);
    HRESULT (STDMETHODCALLTYPE *get_NewWindow)(ICoreWebView2NewWindowRequestedEventArgs *, ICoreWebView2 **);
    HRESULT (STDMETHODCALLTYPE *put_Handled)(ICoreWebView2NewWindowRequestedEventArgs *, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_Handled)(ICoreWebView2NewWindowRequestedEventArgs *, BOOL *);
} ICoreWebView2NewWindowRequestedEventArgsVtbl;

struct ICoreWebView2NewWindowRequestedEventArgs {
    ICoreWebView2NewWindowRequestedEventArgsVtbl *lpVtbl;
};

/* ============================================ 核心 ======================= */

typedef struct ICoreWebView2Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2 *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2 *);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2 *);

    HRESULT (STDMETHODCALLTYPE *get_Settings)(ICoreWebView2 *, ICoreWebView2Settings **settings);
    HRESULT (STDMETHODCALLTYPE *get_Source)(ICoreWebView2 *, LPWSTR *uri);
    HRESULT (STDMETHODCALLTYPE *Navigate)(ICoreWebView2 *, LPCWSTR uri);
    HRESULT (STDMETHODCALLTYPE *NavigateToString)(ICoreWebView2 *, LPCWSTR htmlContent);
    HRESULT (STDMETHODCALLTYPE *add_NavigationStarting)(ICoreWebView2 *, void *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_NavigationStarting)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_ContentLoading)(ICoreWebView2 *, void *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_ContentLoading)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_SourceChanged)(ICoreWebView2 *, void *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_SourceChanged)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_HistoryChanged)(ICoreWebView2 *, void *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_HistoryChanged)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_NavigationCompleted)(ICoreWebView2 *, void *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_NavigationCompleted)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_FrameNavigationStarting)(ICoreWebView2 *, void *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_FrameNavigationStarting)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_FrameNavigationCompleted)(ICoreWebView2 *, void *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_FrameNavigationCompleted)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_ScriptDialogOpening)(ICoreWebView2 *, void *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_ScriptDialogOpening)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_PermissionRequested)(ICoreWebView2 *, void *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_PermissionRequested)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_ProcessFailed)(ICoreWebView2 *, void *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_ProcessFailed)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *AddScriptToExecuteOnDocumentCreated)(ICoreWebView2 *, LPCWSTR, void *);
    HRESULT (STDMETHODCALLTYPE *RemoveScriptToExecuteOnDocumentCreated)(ICoreWebView2 *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *ExecuteScript)(ICoreWebView2 *, LPCWSTR, void *);
    HRESULT (STDMETHODCALLTYPE *CapturePreview)(ICoreWebView2 *, int, IStream *, void *);
    HRESULT (STDMETHODCALLTYPE *Reload)(ICoreWebView2 *);
    HRESULT (STDMETHODCALLTYPE *PostWebMessageAsJson)(ICoreWebView2 *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *PostWebMessageAsString)(ICoreWebView2 *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *add_WebMessageReceived)(ICoreWebView2 *, void *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_WebMessageReceived)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *CallDevToolsProtocolMethod)(ICoreWebView2 *, LPCWSTR, LPCWSTR, void *);
    HRESULT (STDMETHODCALLTYPE *get_BrowserProcessId)(ICoreWebView2 *, UINT32 *value);
    HRESULT (STDMETHODCALLTYPE *get_CanGoBack)(ICoreWebView2 *, BOOL *);
    HRESULT (STDMETHODCALLTYPE *get_CanGoForward)(ICoreWebView2 *, BOOL *);
    HRESULT (STDMETHODCALLTYPE *GoBack)(ICoreWebView2 *);
    HRESULT (STDMETHODCALLTYPE *GoForward)(ICoreWebView2 *);
    HRESULT (STDMETHODCALLTYPE *GetDevToolsProtocolEventReceiver)(ICoreWebView2 *, LPCWSTR, void **);
    HRESULT (STDMETHODCALLTYPE *Stop)(ICoreWebView2 *);
    HRESULT (STDMETHODCALLTYPE *add_NewWindowRequested)(
        ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventHandler *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_NewWindowRequested)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_DocumentTitleChanged)(ICoreWebView2 *, void *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_DocumentTitleChanged)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *get_DocumentTitle)(ICoreWebView2 *, LPWSTR *title);
    HRESULT (STDMETHODCALLTYPE *AddHostObjectToScript)(ICoreWebView2 *, LPCWSTR, VARIANT *);
    HRESULT (STDMETHODCALLTYPE *RemoveHostObjectFromScript)(ICoreWebView2 *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *OpenDevToolsWindow)(ICoreWebView2 *);
    HRESULT (STDMETHODCALLTYPE *add_ContainsFullScreenElementChanged)(ICoreWebView2 *, void *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_ContainsFullScreenElementChanged)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *get_ContainsFullScreenElement)(ICoreWebView2 *, BOOL *containsFullScreenElement);
    HRESULT (STDMETHODCALLTYPE *add_WebResourceRequested)(
        ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventHandler *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_WebResourceRequested)(ICoreWebView2 *, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *AddWebResourceRequestedFilter)(
        ICoreWebView2 *, LPCWSTR uri, COREWEBVIEW2_WEB_RESOURCE_CONTEXT resourceContext);
    HRESULT (STDMETHODCALLTYPE *RemoveWebResourceRequestedFilter)(
        ICoreWebView2 *, LPCWSTR uri, COREWEBVIEW2_WEB_RESOURCE_CONTEXT resourceContext);
    HRESULT (STDMETHODCALLTYPE *add_WindowCloseRequested)(ICoreWebView2 *, void *, EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *remove_WindowCloseRequested)(ICoreWebView2 *, EventRegistrationToken);
} ICoreWebView2Vtbl;

struct ICoreWebView2 { ICoreWebView2Vtbl *lpVtbl; };

/* ============================================ 加载器 ===================== */

typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateCoreWebView2EnvironmentWithOptions)(
    LPCWSTR browserExecutableFolder, LPCWSTR userDataFolder,
    void *environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler);

typedef HRESULT (STDMETHODCALLTYPE *PFN_GetAvailableCoreWebView2BrowserVersionString)(
    LPCWSTR browserExecutableFolder, LPWSTR *versionInfo);

#ifdef __cplusplus
}
#endif


/* ============================================ 编译期自检 =================
 *
 * vtable 顺序写错是这类代码最阴险的 bug：编译通过、运行不报错、
 * 只是界面一片空白。下面这些断言把每个接口的方法个数钉死，
 * 以后谁改错了顺序（多一个、少一个），编译期就会直接失败。
 *
 * 数字来自官方 WebView2.h 里各 Vtbl 的方法个数 + 3 个 IUnknown 方法。
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ICoreWebView2Vtbl) == (3 + 58) * sizeof(void *),
               "ICoreWebView2 的方法个数对不上，vtable 顺序错了");
_Static_assert(sizeof(ICoreWebView2ControllerVtbl) == (3 + 23) * sizeof(void *),
               "ICoreWebView2Controller 的方法个数对不上（注意 get_CoreWebView2 在最后）");
_Static_assert(sizeof(ICoreWebView2EnvironmentVtbl) == (3 + 5) * sizeof(void *),
               "ICoreWebView2Environment 的方法个数对不上");
_Static_assert(sizeof(ICoreWebView2SettingsVtbl) == (3 + 18) * sizeof(void *),
               "ICoreWebView2Settings 的方法个数对不上");
#endif
#endif /* LC_WEBVIEW2_H */
