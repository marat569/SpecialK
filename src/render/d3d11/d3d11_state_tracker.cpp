/**
* This file is part of Special K.
*
* Special K is free software : you can redistribute it
* and/or modify it under the terms of the GNU General Public License
* as published by The Free Software Foundation, either version 3 of
* the License, or (at your option) any later version.
*
* Special K is distributed in the hope that it will be useful,
*
* But WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with Special K.
*
*   If not, see <http://www.gnu.org/licenses/>.
*
**/

#define __SK_SUBSYSTEM__ L"  D3D 11  "

#include <SpecialK/control_panel/d3d11.h>
#include <SpecialK/render/d3d11/d3d11_core.h>
#include <SpecialK/render/d3d11/d3d11_state_tracker.h>

bool SK_D3D11_EnableTracking      = false;
bool SK_D3D11_EnableMMIOTracking  = false;
volatile LONG
     SK_D3D11_DrawTrackingReqs    = 0L;
volatile LONG
     SK_D3D11_CBufferTrackingReqs = 0L;

bool
SK_D3D11_ShouldTrackSetShaderResources ( ID3D11DeviceContext* pDevCtx,
                                         SK_TLS**             ppTLS )
{
  UNREFERENCED_PARAMETER (pDevCtx); UNREFERENCED_PARAMETER (ppTLS);

  if (! SK::ControlPanel::D3D11::show_shader_mod_dlg)
    return false;

  SK_TLS *pTLS = nullptr;

  if (ppTLS != nullptr)
  {
    if (*ppTLS != nullptr)
    {
      pTLS = *ppTLS;
    }

    else
    {
        pTLS = SK_TLS_Bottom ();
      *ppTLS = pTLS;
    }
  }

  else
    pTLS = SK_TLS_Bottom ();

  if (pTLS->imgui.drawing)
    return false;

  return true;
}

bool
SK_D3D11_ShouldTrackMMIO ( ID3D11DeviceContext* pDevCtx,
                           SK_TLS**             ppTLS )
{
  UNREFERENCED_PARAMETER (pDevCtx); UNREFERENCED_PARAMETER (ppTLS);

  if (! SK::ControlPanel::D3D11::show_shader_mod_dlg)
    return false;

  return true;
}


HMODULE hModReShade = (HMODULE)-2;


bool
SK_D3D11_ShouldTrackRenderOp ( ID3D11DeviceContext* pDevCtx,
                               SK_TLS**             ppTLS )
{
  //if (! SK_D3D11_EnableTracking)
  //  return false;

  if ((! config.render.dxgi.deferred_isolation) && SK_D3D11_IsDevCtxDeferred (pDevCtx))
    return false;

  static const SK_RenderBackend& rb =
    SK_GetCurrentRenderBackend ();

  if ( rb.d3d11.immediate_ctx == nullptr ||
       rb.device              == nullptr ||
       rb.swapchain           == nullptr )
  {
    //SK_ReleaseAssert (! "ShouldTrackRenderOp: RenderBackend State");

    return false;
  }

  SK_TLS *pTLS = nullptr;

  if (ppTLS != nullptr)
  {
    if (*ppTLS != nullptr)
    {
      pTLS = *ppTLS;
    }

    else
    {
        pTLS = SK_TLS_Bottom ();
      *ppTLS = pTLS;
    }
  }

  if ( pTLS != nullptr &&
       pTLS->imgui.drawing )
  {
    //SK_ReleaseAssert (! "ShouldTrackRenderOp: ImGui Is Drawing");
  
    return false;
  }

  return true;
}


// Only accessed by the swapchain thread and only to clear any outstanding
//   references prior to a buffer resize
std::vector <IUnknown *> SK_D3D11_TempResources;

std::array <SK_D3D11_KnownTargets, SK_D3D11_MAX_DEV_CONTEXTS + 1> SK_D3D11_RenderTargets;

void
SK_D3D11_KnownThreads::clear_all (void)
{
  if (use_lock)
  {
    SK_AutoCriticalSection auto_cs (&cs);
    ids.clear ();
    return;
  }

  ids.clear ();
}

size_t
SK_D3D11_KnownThreads::count_all (void)
{
  if (use_lock)
  {
    SK_AutoCriticalSection auto_cs (&cs);
    return ids.size ();
  }

  return ids.size ();
}

void
SK_D3D11_KnownThreads::mark (void)
{
  //#ifndef _DEBUG
#if 1
  return;
#else
  if (! SK_D3D11_EnableTracking)
    return;

  if (use_lock)
  {
    SK_AutoCriticalSection auto_cs (&cs);
    ids.emplace    (GetCurrentThreadId ());
    active.emplace (GetCurrentThreadId ());
    return;
  }

  ids.emplace    (GetCurrentThreadId ());
  active.emplace (GetCurrentThreadId ());
#endif
}

#include <array>

memory_tracking_s mem_map_stats;
target_tracking_s tracked_rtv;

ID3D11Texture2D* SK_D3D11_TrackedTexture       = nullptr;
DWORD            tracked_tex_blink_duration    = 666UL;
DWORD            tracked_shader_blink_duration = 666UL;

struct SK_DisjointTimerQueryD3D11 d3d11_shader_tracking_s::disjoint_query;



void
d3d11_shader_tracking_s::activate ( ID3D11DeviceContext        *pDevContext,
                                   ID3D11ClassInstance *const *ppClassInstances,
                                   UINT                        NumClassInstances )
{
  if (! pDevContext) return;

  for ( UINT i = 0 ; i < NumClassInstances ; i++ )
  {
    if (ppClassInstances && ppClassInstances [i])
      addClassInstance   (ppClassInstances [i]);
  }

  const UINT dev_idx =
    SK_D3D11_GetDeviceContextHandle (pDevContext);

  const bool is_active =
    active.get (dev_idx);

  static auto& shaders =
    SK_D3D11_Shaders;

  if ((! is_active))
  {
    active.set (dev_idx, true);

    switch (type_)
    {
      case SK_D3D11_ShaderType::Vertex:
        shaders.vertex.current.shader   [dev_idx] = crc32c.load ();
        break;
      case SK_D3D11_ShaderType::Pixel:
        shaders.pixel.current.shader    [dev_idx] = crc32c.load ();
        break;
      case SK_D3D11_ShaderType::Geometry:
        shaders.geometry.current.shader [dev_idx] = crc32c.load ();
        break;
      case SK_D3D11_ShaderType::Domain:
        shaders.domain.current.shader   [dev_idx] = crc32c.load ();
        break;
      case SK_D3D11_ShaderType::Hull:
        shaders.hull.current.shader     [dev_idx] = crc32c.load ();
        break;
      case SK_D3D11_ShaderType::Compute:
        shaders.compute.current.shader  [dev_idx] = crc32c.load ();
        break;
    }
  }

  else
    return;


  // Timing is very difficult on deferred contexts; will finish later (years?)
  if ( SK_D3D11_IsDevCtxDeferred (pDevContext) )
    return;


  static SK_RenderBackend& rb =
    SK_GetCurrentRenderBackend ();

  CComQIPtr <ID3D11Device> pDev (rb.device);

  if (! pDev)
    return;

  if ( nullptr ==
      ReadPointerAcquire ((void **)&disjoint_query.async)
      && timers.empty () )
  {
    D3D11_QUERY_DESC query_desc {
      D3D11_QUERY_TIMESTAMP_DISJOINT, 0x00
    };

    ID3D11Query                                    *pQuery = nullptr;
    if (SUCCEEDED (pDev->CreateQuery (&query_desc, &pQuery)))
    {
      CComPtr <ID3D11DeviceContext>  pImmediateContext;
      pDev->GetImmediateContext    (&pImmediateContext);

      InterlockedExchangePointer ((void **)&disjoint_query.async, pQuery);
      pImmediateContext->Begin                                   (pQuery);
      InterlockedExchange (&disjoint_query.active, TRUE);
    }
  }

  if (ReadAcquire (&disjoint_query.active))
  {
    // Start a new query
    D3D11_QUERY_DESC query_desc {
      D3D11_QUERY_TIMESTAMP, 0x00
    };

    duration_s duration;

    ID3D11Query                                    *pQuery = nullptr;
    if (SUCCEEDED (pDev->CreateQuery (&query_desc, &pQuery)))
    {
      InterlockedExchangePointer ((void **)&duration.start.dev_ctx, pDevContext);
      InterlockedExchangePointer ((void **)&duration.start.async,   pQuery);
      pDevContext->End                                             (pQuery);
      timers.emplace_back (duration);
    }
  }
}

void
d3d11_shader_tracking_s::deactivate (ID3D11DeviceContext* pDevCtx)
{
  static SK_RenderBackend& rb =
    SK_GetCurrentRenderBackend ();

  const UINT dev_idx =
    SK_D3D11_GetDeviceContextHandle (pDevCtx);

  const bool is_active =
    active.get (dev_idx);

  static auto& shaders =
    SK_D3D11_Shaders;

  if (is_active)
  {
    active.set (dev_idx, false);

    bool end_of_frame = false;

    if (pDevCtx == nullptr)
    {
      end_of_frame = true;
      pDevCtx      = static_cast <ID3D11DeviceContext *>(rb.d3d11.immediate_ctx.p);
    }

    switch (type_)
    {
      case SK_D3D11_ShaderType::Vertex:
        shaders.vertex.current.shader [dev_idx]   = 0x0;
        break;
      case SK_D3D11_ShaderType::Pixel:
        shaders.pixel.current.shader  [dev_idx]    = 0x0;
        break;
      case SK_D3D11_ShaderType::Geometry:
        shaders.geometry.current.shader [dev_idx] = 0x0;
        break;
      case SK_D3D11_ShaderType::Domain:
        shaders.domain.current.shader [dev_idx]   = 0x0;
        break;
      case SK_D3D11_ShaderType::Hull:
        shaders.hull.current.shader [dev_idx]     = 0x0;
        break;
      case SK_D3D11_ShaderType::Compute:
        shaders.compute.current.shader [dev_idx]  = 0x0;
        break;
    }

    if (end_of_frame) return;
  }

  else
    return;


  if (pDevCtx == nullptr)
    pDevCtx  = static_cast <ID3D11DeviceContext *>(rb.d3d11.immediate_ctx.p);


  // Timing is very difficult on deferred contexts; will finish later (years?)
  if ( pDevCtx != nullptr && SK_D3D11_IsDevCtxDeferred (pDevCtx) )
    return;


  CComQIPtr <ID3D11Device> dev (rb.device);

  if (dev != nullptr && ReadAcquire (&disjoint_query.active))
  {
    D3D11_QUERY_DESC query_desc {
      D3D11_QUERY_TIMESTAMP, 0x00
    };

    duration_s& duration =
      timers.back ();

    ID3D11Query* pQuery = nullptr;
    if ( SUCCEEDED ( dev->CreateQuery (&query_desc, &pQuery ) ) )
    { InterlockedExchangePointer (
      (PVOID *) (&duration.end.dev_ctx), pDevCtx
    );
    InterlockedExchangePointer (
      (PVOID *) (&duration.end.async),   pQuery
    );                              pDevCtx->End (pQuery);
    }
  }
}

void
d3d11_shader_tracking_s::use (IUnknown* pShader)
{
  UNREFERENCED_PARAMETER (pShader);

  num_draws++;
}

bool
SK_D3D11_ShouldTrackDrawCall ( ID3D11DeviceContext* pDevCtx,
                         const SK_D3D11DrawType     draw_type,
                               SK_TLS**             ppTLS )
{
  //// Engine is way too parallel with its 8 queues, don't even try to
  ////   track the deferred command stream!
  //if ( SK_D3D11_IsDevCtxDeferred (pDevCtx) &&
  //     SK_GetCurrentGameID () == SK_GAME_ID::AssassinsCreed_Odyssey )
  //{
  //  return false;
  //}

  // If ReShade (custom version) is loaded, state tracking is non-optional
  if ( (intptr_t)hModReShade < (intptr_t)nullptr )
                 hModReShade = SK_ReShade_GetDLL ();

  bool process = false;

  auto reshadable =
   [&](void) ->
  bool
  {
    return ( draw_type == SK_D3D11DrawType::PrimList ||
             draw_type == SK_D3D11DrawType::Indexed );
  };

  if ( SK_ReShade_DrawCallback.fn != nullptr &&
                               reshadable () &&
       (! SK_D3D11_Shaders.reshade_triggered) )
  {
    process = true;
  }

  else
  {
    process =
     ( ReadAcquire (&SK_D3D11_DrawTrackingReqs) > 0 )
               ||
       SK_D3D11_ShouldTrackRenderOp (pDevCtx, ppTLS);
  }

  if (process && (ppTLS != nullptr && (! (*ppTLS))))
                 *ppTLS = SK_TLS_Bottom ();

  return
    process;
}

SK_D3D11_KnownThreads SK_D3D11_MemoryThreads;
SK_D3D11_KnownThreads SK_D3D11_DrawThreads;
SK_D3D11_KnownThreads SK_D3D11_DispatchThreads;
SK_D3D11_KnownThreads SK_D3D11_ShaderThreads;