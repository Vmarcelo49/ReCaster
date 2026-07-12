#include "D3DHook.h"

#include <IRefPtr.h>
#include <CDllFile.h>
#include <CHookJump.h>

#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <comdef.h>

#include <cassert>
#include <sstream>

using namespace std;

template<typename T>
inline string toHexString ( const T& val )
{
    stringstream ss;
    ss << hex << val;
    return ss.str();
}

// DX interface entry offsets.
#define INTF_QueryInterface 0
#define INTF_AddRef         1
#define INTF_Release        2
#define INTF_DX9_Reset      16
#define INTF_DX9_Present    17
#define INTF_DX9_EndScene   42

typedef IDirect3D9 * ( __stdcall *DIRECT3DCREATE9 ) ( UINT );
typedef ULONG   ( __stdcall *PFN_DX9_ADDREF ) ( IDirect3DDevice9 *pDevice );
typedef ULONG   ( __stdcall *PFN_DX9_RELEASE ) ( IDirect3DDevice9 *pDevice );
typedef HRESULT ( __stdcall *PFN_DX9_RESET ) ( IDirect3DDevice9 *pDevice, LPVOID );
typedef HRESULT ( __stdcall *PFN_DX9_PRESENT ) ( IDirect3DDevice9 *pDevice, const RECT *, const RECT *, HWND, LPVOID );
typedef HRESULT ( __stdcall *PFN_DX9_ENDSCENE ) ( IDirect3DDevice9 *pDevice );

CDllFile g_DX9;
IDirect3DDevice9 *g_pDevice = 0;
ULONG g_iRefCount = 1;
ULONG g_iRefCountMe = 0;

// Vtable swap pointers. We store the ADDRESS OF each vtable entry (not the
// function pointer value) so we can write our hook function pointer into it.
// This works on both native Windows AND Wine — unlike code-overwrite (JMP
// patching), vtable swapping doesn't depend on the function's code being at
// a specific address or VirtualProtect succeeding on code pages.
static UINT_PTR *g_pVTable_Present  = 0;
static UINT_PTR *g_pVTable_Reset    = 0;
static UINT_PTR *g_pVTable_EndScene = 0;

// AddRef/Release vtable entry pointers — swapped lazily on first Present
// call (DX9_HooksInit) to track device lifetime for clean unhooking.
UINT_PTR *m_Hook_AddRef = 0;
UINT_PTR *m_Hook_Release = 0;

PFN_DX9_ADDREF  s_D3D9_AddRef = 0;
PFN_DX9_RELEASE s_D3D9_Release = 0;
PFN_DX9_RESET   s_D3D9_Reset = 0;
PFN_DX9_PRESENT s_D3D9_Present = 0;
PFN_DX9_ENDSCENE s_D3D9_EndScene = 0;

EXTERN_C ULONG __declspec ( dllexport ) __stdcall DX9_AddRef ( IDirect3DDevice9 *pDevice )
{
    g_iRefCount = s_D3D9_AddRef ( pDevice );
    return g_iRefCount;
}

EXTERN_C ULONG __declspec ( dllexport ) __stdcall DX9_Release ( IDirect3DDevice9 *pDevice )
{
    // "fall-through" case: just forward to the real Release while the device
    // is still alive (refcount > our own + 1).
    if ( ( g_iRefCount > g_iRefCountMe + 1 ) && s_D3D9_Release )
    {
        g_iRefCount = s_D3D9_Release ( pDevice );
        return g_iRefCount;
    }

    // Device is going away — unhook everything before the real Release.
    g_pDevice = pDevice;

    // unhook device methods
    UnhookDirectX();

    // reset the pointers
    m_Hook_AddRef = 0;
    m_Hook_Release = 0;

    // call the real Release()
    g_iRefCount = s_D3D9_Release ( pDevice );
    return g_iRefCount;
}

void DX9_HooksInit ( IDirect3DDevice9 *pDevice )
{
    UINT_PTR *pVTable = ( UINT_PTR * ) ( * ( ( UINT_PTR * ) pDevice ) );
    assert ( pVTable );
    m_Hook_AddRef = pVTable + 1;
    m_Hook_Release = pVTable + 2;

    // Save original function pointers
    s_D3D9_AddRef = ( PFN_DX9_ADDREF ) ( *m_Hook_AddRef );
    s_D3D9_Release = ( PFN_DX9_RELEASE ) ( *m_Hook_Release );

    // Swap AddRef/Release vtable entries. On Wine, the vtable is in a
    // read-only page — VirtualProtect is required to make it writable.
    // Without this, writing to the vtable causes a page fault (crash).
    DWORD oldProtect = 0;

    if ( VirtualProtect ( m_Hook_AddRef, sizeof(UINT_PTR), PAGE_READWRITE, &oldProtect ) )
    {
        *m_Hook_AddRef = ( UINT_PTR ) DX9_AddRef;
        VirtualProtect ( m_Hook_AddRef, sizeof(UINT_PTR), oldProtect, &oldProtect );
    }

    if ( VirtualProtect ( m_Hook_Release, sizeof(UINT_PTR), PAGE_READWRITE, &oldProtect ) )
    {
        *m_Hook_Release = ( UINT_PTR ) DX9_Release;
        VirtualProtect ( m_Hook_Release, sizeof(UINT_PTR), oldProtect, &oldProtect );
    }
}

void DX9_HooksVerify ( IDirect3DDevice9 *pDevice )
{
    // It looks like at certain points, vtable entries get restored to its original values.
    // If that happens, we need to re-assign them to our functions again.
    // NOTE: we don't want blindly re-assign, because there can be other programs
    // hooking on the same methods. Therefore, we only re-assign if we see that
    // original addresses are restored by the system.

    UINT_PTR *pVTable = ( UINT_PTR * ) ( * ( ( UINT_PTR * ) pDevice ) );
    assert ( pVTable );
    DWORD oldProtect = 0;
    if ( pVTable[INTF_AddRef] == ( UINT_PTR ) s_D3D9_AddRef )
    {
        if ( VirtualProtect ( &pVTable[INTF_AddRef], sizeof(UINT_PTR), PAGE_READWRITE, &oldProtect ) )
        {
            pVTable[INTF_AddRef] = ( UINT_PTR ) DX9_AddRef;
            VirtualProtect ( &pVTable[INTF_AddRef], sizeof(UINT_PTR), oldProtect, &oldProtect );
        }
    }
    if ( pVTable[INTF_Release] == ( UINT_PTR ) s_D3D9_Release )
    {
        if ( VirtualProtect ( &pVTable[INTF_Release], sizeof(UINT_PTR), PAGE_READWRITE, &oldProtect ) )
        {
            pVTable[INTF_Release] = ( UINT_PTR ) DX9_Release;
            VirtualProtect ( &pVTable[INTF_Release], sizeof(UINT_PTR), oldProtect, &oldProtect );
        }
    }
}

EXTERN_C HRESULT __declspec ( dllexport ) __stdcall DX9_EndScene ( IDirect3DDevice9 *pDevice )
{
    g_pDevice = pDevice;

    // Initialize AddRef/Release vtable hooks on first EndScene call (for
    // device lifetime tracking / cleanup). This is independent of the
    // Present/Reset/EndScene vtable swap done in HookDirectX().
    if ( m_Hook_AddRef == 0 || m_Hook_Release == 0 )
    {
        DX9_HooksInit ( pDevice );
    }

    EndScene ( pDevice );

    // Call the real EndScene via the saved original pointer. With vtable
    // swap, s_D3D9_EndScene points to the original function — no need to
    // restore/re-apply JMP patches.
    HRESULT hRes = s_D3D9_EndScene ( pDevice );

    DX9_HooksVerify ( pDevice );

    return hRes;
}

EXTERN_C HRESULT __declspec ( dllexport ) __stdcall DX9_Reset ( IDirect3DDevice9 *pDevice, LPVOID params )
{
    g_pDevice = pDevice;

    InvalidateDeviceObjects();

    // Call the real Reset via the saved original pointer.
    HRESULT hRes = s_D3D9_Reset ( pDevice, params );

    DX9_HooksVerify ( pDevice );

    return hRes;
}

EXTERN_C HRESULT __declspec ( dllexport ) __stdcall DX9_Present (
    IDirect3DDevice9 *pDevice, const RECT *src, const RECT *dest, HWND hwnd, LPVOID unused )
{
    g_pDevice = pDevice;

    // Initialize AddRef/Release vtable hooks on first Present call.
    if ( m_Hook_AddRef == 0 || m_Hook_Release == 0 )
    {
        DX9_HooksInit ( pDevice );
    }

    PresentFrameBegin ( pDevice );

    // Call the real Present via the saved original pointer.
    HRESULT hRes = s_D3D9_Present ( pDevice, src, dest, hwnd, unused );

    PresentFrameEnd ( pDevice );

    DX9_HooksVerify ( pDevice );

    return hRes;
}

string InitDirectX ( void *hwnd )
{
    // get the offset from the start of the DLL to the interface element we want.
    // step 1: Load d3d9.dll
    HRESULT hRes = g_DX9.LoadDll ( TEXT ( "d3d9" ) );
    if ( IS_ERROR ( hRes ) )
    {
        _com_error err ( hRes );
        return "Failed to load d3d9.dll: [" + toHexString ( hRes ) + "] " + err.ErrorMessage();
    }

    // step 2: Get IDirect3D9
    DIRECT3DCREATE9 pDirect3DCreate9 = ( DIRECT3DCREATE9 ) g_DX9.GetProcAddress ( "Direct3DCreate9" );
    if ( pDirect3DCreate9 == 0 )
    {
        return "Unable to find Direct3DCreate9";
    }

    IRefPtr<IDirect3D9> pD3D = pDirect3DCreate9 ( D3D_SDK_VERSION );
    if ( !pD3D.IsValidRefObj() )
    {
        return "Direct3DCreate9 failed";
    }

    // step 3: Get IDirect3DDevice9
    D3DDISPLAYMODE d3ddm;
    hRes = pD3D->GetAdapterDisplayMode ( D3DADAPTER_DEFAULT, &d3ddm );
    if ( FAILED ( hRes ) )
    {
        _com_error err ( hRes );
        return "GetAdapterDisplayMode failed: [" + toHexString ( hRes ) + "] " + err.ErrorMessage();
    }

    D3DPRESENT_PARAMETERS d3dpp;
    ZeroMemory ( &d3dpp, sizeof ( d3dpp ) );
    d3dpp.Windowed = true;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.BackBufferFormat = d3ddm.Format;

    IRefPtr<IDirect3DDevice9> pD3DDevice;
    hRes = pD3D->CreateDevice ( D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, ( HWND ) hwnd,
                                D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                &d3dpp, IREF_GETPPTR ( pD3DDevice, IDirect3DDevice9 ) );
    if ( FAILED ( hRes ) )
    {
        _com_error err ( hRes );
        return "CreateDevice failed: [" + toHexString ( hRes ) + "] " + err.ErrorMessage();
    }

    // step 4: store vtable entry addresses and save original function pointers.
    //
    // We store POINTERS TO vtable entries (not function pointers, not DLL-relative
    // offsets). This allows HookDirectX() to swap the entries via vtable swap,
    // which works on both native Windows AND Wine (code-overwrite JMP patching
    // does NOT work on Wine because Wine's D3D9 functions live in wined3d.dll
    // and may not be called through the hooked code address).
    //
    // The vtable is shared by ALL IDirect3DDevice9 instances (COM interface —
    // per-class, not per-object). So the temporary device's vtable IS the
    // game device's vtable. Swapping an entry here affects all devices.
    UINT_PTR *pVTable = ( UINT_PTR * ) ( * ( ( UINT_PTR * ) pD3DDevice.get_RefObj() ) );
    assert ( pVTable );
    g_pVTable_Present  = &pVTable[INTF_DX9_Present];
    g_pVTable_Reset    = &pVTable[INTF_DX9_Reset];
    g_pVTable_EndScene = &pVTable[INTF_DX9_EndScene];

    // Save original function pointers — called from our hook functions.
    s_D3D9_Present  = ( PFN_DX9_PRESENT )  *g_pVTable_Present;
    s_D3D9_Reset    = ( PFN_DX9_RESET )    *g_pVTable_Reset;
    s_D3D9_EndScene = ( PFN_DX9_ENDSCENE ) *g_pVTable_EndScene;

    return "";
}

string HookDirectX()
{
    // VTABLE SWAP hooking for IDirect3DDevice9::Present / Reset / EndScene.
    //
    // Instead of patching the function's code with a JMP (code-overwrite,
    // which doesn't work on Wine), we swap the function pointer in the
    // vtable array itself. When the game calls device->Present(), it reads
    // the vtable entry -> our DX9_Present -> which calls the saved original.
    //
    // The vtable is in a data section (typically .rdata / read-only). We
    // VirtualProtect it to PAGE_READWRITE, write our function pointers,
    // then restore the protection. This works on both native Windows and
    // Wine because Wine implements COM vtables the same way.

    if ( !g_pVTable_Present || !g_pVTable_Reset || !g_pVTable_EndScene )
        return "No vtable pointers (InitDirectX not called?)";

    if ( !s_D3D9_Present || !s_D3D9_Reset || !s_D3D9_EndScene )
        return "No original function pointers saved";

    DWORD oldProtect = 0;

    if ( !VirtualProtect ( g_pVTable_Present, sizeof(UINT_PTR), PAGE_READWRITE, &oldProtect ) )
        return "VirtualProtect failed for Present vtable entry";
    *g_pVTable_Present = ( UINT_PTR ) DX9_Present;
    VirtualProtect ( g_pVTable_Present, sizeof(UINT_PTR), oldProtect, &oldProtect );

    if ( !VirtualProtect ( g_pVTable_Reset, sizeof(UINT_PTR), PAGE_READWRITE, &oldProtect ) )
        return "VirtualProtect failed for Reset vtable entry";
    *g_pVTable_Reset = ( UINT_PTR ) DX9_Reset;
    VirtualProtect ( g_pVTable_Reset, sizeof(UINT_PTR), oldProtect, &oldProtect );

    if ( !VirtualProtect ( g_pVTable_EndScene, sizeof(UINT_PTR), PAGE_READWRITE, &oldProtect ) )
        return "VirtualProtect failed for EndScene vtable entry";
    *g_pVTable_EndScene = ( UINT_PTR ) DX9_EndScene;
    VirtualProtect ( g_pVTable_EndScene, sizeof(UINT_PTR), oldProtect, &oldProtect );

    return "";
}

void UnhookDirectX()
{
    // Restore original AddRef/Release vtable entries
    if ( m_Hook_AddRef != 0 && s_D3D9_AddRef != 0 )
    {
        *m_Hook_AddRef = ( UINT_PTR ) s_D3D9_AddRef;
    }
    if ( m_Hook_Release != 0 && s_D3D9_Release != 0 )
    {
        *m_Hook_Release = ( UINT_PTR ) s_D3D9_Release;
    }

    // Restore original Present/Reset/EndScene vtable entries
    DWORD oldProtect = 0;
    if ( g_pVTable_Present && s_D3D9_Present )
    {
        if ( VirtualProtect ( g_pVTable_Present, sizeof(UINT_PTR), PAGE_READWRITE, &oldProtect ) )
        {
            *g_pVTable_Present = ( UINT_PTR ) s_D3D9_Present;
            VirtualProtect ( g_pVTable_Present, sizeof(UINT_PTR), oldProtect, &oldProtect );
        }
    }
    if ( g_pVTable_Reset && s_D3D9_Reset )
    {
        if ( VirtualProtect ( g_pVTable_Reset, sizeof(UINT_PTR), PAGE_READWRITE, &oldProtect ) )
        {
            *g_pVTable_Reset = ( UINT_PTR ) s_D3D9_Reset;
            VirtualProtect ( g_pVTable_Reset, sizeof(UINT_PTR), oldProtect, &oldProtect );
        }
    }
    if ( g_pVTable_EndScene && s_D3D9_EndScene )
    {
        if ( VirtualProtect ( g_pVTable_EndScene, sizeof(UINT_PTR), PAGE_READWRITE, &oldProtect ) )
        {
            *g_pVTable_EndScene = ( UINT_PTR ) s_D3D9_EndScene;
            VirtualProtect ( g_pVTable_EndScene, sizeof(UINT_PTR), oldProtect, &oldProtect );
        }
    }

    InvalidateDeviceObjects();
}
