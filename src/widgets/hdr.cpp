﻿/**
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

#include <SpecialK/widgets/widget.h>

#include <SpecialK/config.h>
#include <SpecialK/parameter.h>
#include <SpecialK/control_panel.h>
#include <SpecialK/render/dxgi/dxgi_backend.h>

#include <SpecialK/utility.h>
#include <SpecialK/steam_api.h>
#include <SpecialK/plugin/plugin_mgr.h>

#define SK_HDR_SECTION     L"SpecialK.HDR"
#define SK_MISC_SECTION    L"SpecialK.Misc"

extern iSK_INI*             dll_ini;
extern sk::ParameterFactory g_ParameterFactory;


static auto DeclKeybind =
[](SK_ConfigSerializedKeybind* binding, iSK_INI* ini, const wchar_t* sec) ->
auto
{
  auto* ret =
    dynamic_cast <sk::ParameterStringW *>
    (g_ParameterFactory.create_parameter <std::wstring> (L"DESCRIPTION HERE"));

  ret->register_to_ini ( ini, sec, binding->short_name );

  return ret;
};

static auto
Keybinding = [] (SK_Keybind* binding, sk::ParameterStringW* param) ->
auto
{
  std::string label =
    SK_WideCharToUTF8 (binding->human_readable) + "###";

  label += binding->bind_name;

  if (ImGui::Selectable (label.c_str (), false))
  {
    ImGui::OpenPopup (binding->bind_name);
  }

  std::wstring original_binding =
    binding->human_readable;

  SK_ImGui_KeybindDialog (binding);

  if (original_binding != binding->human_readable)
  {
    param->store (binding->human_readable);

    SK_SaveConfig ();

    return true;
  }

  return false;
};


sk::ParameterBool* _SK_HDR_10BitSwapChain;
sk::ParameterBool* _SK_HDR_16BitSwapChain;
sk::ParameterInt*  _SK_HDR_ActivePreset;
sk::ParameterBool* _SK_HDR_FullRange;

bool __SK_HDR_10BitSwap = false;
bool __SK_HDR_16BitSwap = false;

float __SK_HDR_Luma      = 300.0_Nits;
float __SK_HDR_Exp       = 1.0f;
int   __SK_HDR_Preset    = 0;
bool  __SK_HDR_FullRange = false;

float __SK_HDR_UI_Luma       =   1.0f;
float __SK_HDR_HorizCoverage = 100.0f;
float __SK_HDR_VertCoverage  = 100.0f;


#define MAX_HDR_PRESETS 4

struct SK_HDR_Preset_s {
  const char*  preset_name;
  int          preset_idx;

  float        peak_white_nits;
  float        eotf;

  std::wstring annotation = L"";

  sk::ParameterFloat*   cfg_nits    = nullptr;
  sk::ParameterFloat*   cfg_eotf    = nullptr;
  sk::ParameterStringW* cfg_notes   = nullptr;

  SK_ConfigSerializedKeybind
    preset_activate = {
      SK_Keybind {
        preset_name, L"",
          false, false, false, 'F1'
      }, L"Activate"
    };

  int activate (void)
  {
    __SK_HDR_Preset =
      preset_idx;

    __SK_HDR_Luma = peak_white_nits;
    __SK_HDR_Exp  = eotf;

    if (_SK_HDR_ActivePreset != nullptr)
    {   _SK_HDR_ActivePreset->store (preset_idx);

      SK_GetDLLConfig   ()->write (
        SK_GetDLLConfig ()->get_filename ()
                                  );
    }

    return preset_idx;
  }

  void setup (void)
  {
    if (cfg_nits == nullptr)
    {
      //preset_activate =
      //  std::move (
      //    SK_ConfigSerializedKeybind {
      //      SK_Keybind {
      //        preset_name, L"",
      //          false, true, true, '0'
      //      }, L"Activate"
      //    }
      //  );

      cfg_nits =
        _CreateConfigParameterFloat ( SK_HDR_SECTION,
                   SK_FormatStringW (L"scRGBLuminance_[%lu]", preset_idx).c_str (),
                    peak_white_nits, L"scRGB Luminance" );
      cfg_eotf =
        _CreateConfigParameterFloat ( SK_HDR_SECTION,
                   SK_FormatStringW (L"scRGBGamma_[%lu]",     preset_idx).c_str (),
                               eotf, L"scRGB Gamma" );

      wcsncpy_s ( preset_activate.short_name,                           32,
        SK_FormatStringW (L"Activate%lu", preset_idx).c_str (), _TRUNCATE );

      preset_activate.param =
        DeclKeybind (&preset_activate, SK_GetDLLConfig (), L"HDR.Presets");

      if (! preset_activate.param->load (preset_activate.human_readable))
      {
        preset_activate.human_readable =
          SK_FormatStringW (L"F%lu", preset_idx);
      }

      preset_activate.parse ();
      preset_activate.param->store (preset_activate.human_readable);

      SK_GetDLLConfig   ()->write (
        SK_GetDLLConfig ()->get_filename ()
                                  );
    }
  }
} static hdr_presets [4] = { { "HDR Preset 0", 0, 300.0_Nits, 1.0f, L"F1" },
                             { "HDR Preset 1", 1, 300.0_Nits, 1.0f, L"F2" },
                             { "HDR Preset 2", 2, 300.0_Nits, 1.0f, L"F3" },
                             { "HDR Preset 3", 3, 300.0_Nits, 1.0f, L"F4" } };

BOOL
CALLBACK
SK_HDR_KeyPress ( BOOL Control,
                  BOOL Shift,
                  BOOL Alt,
                  BYTE vkCode )
{
  #define SK_MakeKeyMask(vKey,ctrl,shift,alt)           \
    static_cast <UINT>((vKey) | (((ctrl) != 0) <<  9) | \
                                (((shift)!= 0) << 10) | \
                                (((alt)  != 0) << 11))

  for ( auto& it : hdr_presets )
  {
    if ( it.preset_activate.masked_code ==
           SK_MakeKeyMask ( vkCode,
                            Control != 0 ? 1 : 0,
                            Shift   != 0 ? 1 : 0,
                            Alt     != 0 ? 1 : 0 )
       )
    {
      it.activate ();

      return TRUE;
    }     
  }

  return FALSE;
}

extern iSK_INI* osd_ini;

////SK_DXGI_HDRControl*
////SK_HDR_GetControl (void)
////{
////  static SK_DXGI_HDRControl hdr_ctl = { };
////  return                   &hdr_ctl;
////}

class SKWG_HDR_Control : public SK_Widget
{
public:
  SKWG_HDR_Control (void) : SK_Widget ("DXGI_HDR")
  {
    SK_ImGui_Widgets.hdr_control = this;

    setAutoFit (true).setDockingPoint (DockAnchor::NorthEast).setClickThrough (false);
  };

  void run (void) override
  {
    static bool first = true;

    if (first) first = false;
    else return;

    hdr_presets [0].setup (); hdr_presets [1].setup ();
    hdr_presets [2].setup (); hdr_presets [3].setup ();

    _SK_HDR_10BitSwapChain =
      _CreateConfigParameterBool ( SK_HDR_SECTION,
                                  L"Use10BitSwapChain",  __SK_HDR_10BitSwap,
                                  L"10-bit SwapChain" );
    _SK_HDR_16BitSwapChain =
      _CreateConfigParameterBool ( SK_HDR_SECTION,
                                  L"Use16BitSwapChain",  __SK_HDR_16BitSwap,
                                  L"16-bit SwapChain" );

    _SK_HDR_FullRange =
      _CreateConfigParameterBool ( SK_HDR_SECTION,
                                  L"AllowFullLuminance",  __SK_HDR_FullRange,
                                  L"Slider will use full-range." );

    _SK_HDR_ActivePreset =
      _CreateConfigParameterInt ( SK_HDR_SECTION,
                                 L"Preset",               __SK_HDR_Preset,
                                 L"Light Adaptation Preset" );


    if ( __SK_HDR_Preset < 0 ||
        __SK_HDR_Preset >= MAX_HDR_PRESETS )
    {
      __SK_HDR_Preset = 0;
    }

    hdr_presets [__SK_HDR_Preset].activate ();

    if (_SK_HDR_10BitSwapChain->load (__SK_HDR_10BitSwap))
    {
      if (__SK_HDR_10BitSwap)
      {
        SK_GetCurrentRenderBackend ().scanout.colorspace_override =
          DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
      }
    }

    else if (_SK_HDR_16BitSwapChain->load (__SK_HDR_16BitSwap))
    {
      if (__SK_HDR_16BitSwap)
      {
        //dll_log.Log (L"Test");

        SK_GetCurrentRenderBackend ().scanout.colorspace_override =
          DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
      }
    }
  }

  void draw (void) override
  {
    if (! ImGui::GetFont ())
      return;

    auto& rb =
      SK_GetCurrentRenderBackend ();

    if (! rb.isHDRCapable ())
      return;

    ImVec2 v2Min (
      ImGui::GetIO ().DisplaySize.x / 6.0f, ImGui::GetIO ().DisplaySize.y / 4.0f
    );

    ImVec2 v2Max (
      ImGui::GetIO ().DisplaySize.x / 4.0f, ImGui::GetIO ().DisplaySize.y / 3.0f
    );

    setMinSize (v2Min);
    setMaxSize (v2Max);

    ////SK_DXGI_HDRControl* pHDRCtl =
    ////  SK_HDR_GetControl ();
    ////
    ////bool sync_metadata = false;
    ////
    ////ImGui::Checkbox ("###HDR_Override_MinMasterLevel", &pHDRCtl->overrides.MinMaster); ImGui::SameLine ();
    ////float fMinMaster = (float)pHDRCtl->meta.MinMasteringLuminance / 10000.0f;
    ////if (ImGui::SliderFloat ("Minimum Luminance", &fMinMaster, pHDRCtl->devcaps.MinLuminance, pHDRCtl->devcaps.MaxLuminance))
    ////{
    ////  if (pHDRCtl->overrides.MinMaster)
    ////  {
    ////    sync_metadata = true;
    ////    pHDRCtl->meta.MinMasteringLuminance = (UINT)(fMinMaster * 10000);
    ////  }
    ////}
    ////
    ////ImGui::Checkbox ("###HDR_Override_MaxMasterLevel", &pHDRCtl->overrides.MaxMaster); ImGui::SameLine ();
    ////float fMaxMaster = (float)pHDRCtl->meta.MaxMasteringLuminance / 10000.0f;
    ////if (ImGui::SliderFloat ("Maximum Luminance", &fMaxMaster, pHDRCtl->devcaps.MinLuminance, pHDRCtl->devcaps.MaxLuminance))
    ////{
    ////  if (pHDRCtl->overrides.MaxMaster)
    ////  {
    ////    sync_metadata = true;
    ////    pHDRCtl->meta.MaxMasteringLuminance = (UINT)(fMaxMaster * 10000);
    ////  }
    ////}
    ////
    ////ImGui::Separator ();
    ////
    ////ImGui::Checkbox ("###HDR_Override_MaxContentLevel", &pHDRCtl->overrides.MaxContentLightLevel); ImGui::SameLine ();
    ////float fBrightest = (float)pHDRCtl->meta.MaxContentLightLevel;
    ////if (ImGui::SliderFloat ("Max. Content Light Level (nits)",       &fBrightest,          pHDRCtl->devcaps.MinLuminance, pHDRCtl->devcaps.MaxLuminance))
    ////{
    ////  if (pHDRCtl->overrides.MaxContentLightLevel)
    ////  {
    ////    sync_metadata = true;
    ////    pHDRCtl->meta.MaxContentLightLevel = (UINT16)fBrightest;
    ////  }
    ////}
    ////
    ////ImGui::Checkbox ("###HDR_Override_MaxFrameAverageLightLevel", &pHDRCtl->overrides.MaxFrameAverageLightLevel); ImGui::SameLine ();
    ////float fBrightestLastFrame = (float)pHDRCtl->meta.MaxFrameAverageLightLevel;
    ////if (ImGui::SliderFloat ("Max. Frame Average Light Level (nits)", &fBrightestLastFrame, pHDRCtl->devcaps.MinLuminance, pHDRCtl->devcaps.MaxLuminance))
    ////{
    ////  if (pHDRCtl->overrides.MaxFrameAverageLightLevel)
    ////  {
    ////    sync_metadata = true;
    ////    pHDRCtl->meta.MaxFrameAverageLightLevel = (UINT16)fBrightestLastFrame;
    ////  }
    ////}
    ////
    ////if (sync_metadata)
    ////{
    ////  CComQIPtr <IDXGISwapChain4> pSwapChain (rb.swapchain);
    ////
    ////  if (pSwapChain != nullptr)
    ////  {
    ////    pSwapChain->SetHDRMetaData (
    ////         DXGI_HDR_METADATA_TYPE_HDR10,
    ////      sizeof (DXGI_HDR_METADATA_HDR10),
    ////           nullptr
    ////    );
    ////  }
    ////}
    ////
    ////ImGui::TreePush   ("");
    ////ImGui::BulletText ("Game has adjusted HDR Metadata %lu times...", pHDRCtl->meta._AdjustmentCount);
    ////ImGui::TreePop    (  );

    ////bool bRetro =
    ////  true && ImGui::CollapsingHeader ("HDR Retrofit", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlapMode);


    static bool TenBitSwap_Original     = __SK_HDR_10BitSwap;
    static bool SixteenBitSwap_Original = __SK_HDR_16BitSwap;

    static int sel = __SK_HDR_16BitSwap ? 2 :
                     __SK_HDR_10BitSwap ? 1 : 0;

    if (! rb.isHDRCapable ())
    {
      if ( __SK_HDR_10BitSwap ||
           __SK_HDR_16BitSwap   )
      {
        SK_RunOnce (SK_ImGui_Warning (
          L"Please Restart the Game\n\n\t\tHDR Features were Enabled on a non-HDR Display!")
        );

        __SK_HDR_16BitSwap = false;
        __SK_HDR_10BitSwap = false;

        SK_RunOnce (_SK_HDR_16BitSwapChain->store (__SK_HDR_16BitSwap));
        SK_RunOnce (_SK_HDR_10BitSwapChain->store (__SK_HDR_10BitSwap));

        SK_RunOnce (dll_ini->write (dll_ini->get_filename ()));
      }

      //DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
    }

    else
    {
      ImGui::SameLine ();

      if (ImGui::RadioButton ("None", &sel, 0))
      {
        __SK_HDR_10BitSwap = false;
        __SK_HDR_16BitSwap = false;

        _SK_HDR_10BitSwapChain->store (__SK_HDR_10BitSwap);
        _SK_HDR_16BitSwapChain->store (__SK_HDR_16BitSwap);

        dll_ini->write (dll_ini->get_filename ());
      }

      if ( __SK_HDR_10BitSwap ||
           __SK_HDR_16BitSwap )
      {
        if (__SK_HDR_16BitSwap)
        {
          rb.scanout.colorspace_override =
            DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        }

        CComQIPtr <IDXGISwapChain3> pSwap3 (
          rb.swapchain.p
        );

        if (pSwap3 != nullptr)
        {
          SK_RunOnce (pSwap3->SetColorSpace1 (
            (DXGI_COLOR_SPACE_TYPE)
            rb.scanout.colorspace_override )
          );
        }
      }

      ImGui::SameLine ();

      if (ImGui::RadioButton ("scRGB HDR (16-bit)", &sel, 2))
      {
        __SK_HDR_16BitSwap = true;

        if (__SK_HDR_16BitSwap) __SK_HDR_10BitSwap = false;

        _SK_HDR_10BitSwapChain->store (__SK_HDR_10BitSwap);
        _SK_HDR_16BitSwapChain->store (__SK_HDR_16BitSwap);

        dll_ini->write (dll_ini->get_filename ());
      }
    }

    ////if (bRetro)
    ////{ 
    ////  ImGui::Separator (  );
    ////  ImGui::TreePush  ("");
    ////
    ////  ImGui::PushStyleColor (ImGuiCol_Header,        ImVec4 (0.90f, 0.68f, 0.02f, 0.45f));
    ////  ImGui::PushStyleColor (ImGuiCol_HeaderHovered, ImVec4 (0.90f, 0.72f, 0.07f, 0.80f));
    ////  ImGui::PushStyleColor (ImGuiCol_HeaderActive,  ImVec4 (0.87f, 0.78f, 0.14f, 0.80f));
    ////  ImGui::PopStyleColor (3);
    ////  ImGui::TreePop       ( );
    ////}

    if (rb.isHDRCapable () && (rb.framebuffer_flags & SK_FRAMEBUFFER_FLAG_HDR))
    {
      if (ImGui::IsItemHovered ())
      {
        ImGui::BeginTooltip ();
        {
          ImGui::TextUnformatted ("For best image quality, ensure your desktop is using 10-bit color");
          ImGui::Separator ();
          ImGui::BulletText ("Either RGB Full-Range or YCbCr-4:4:4 will work");
          ImGui::EndTooltip ();
        }
      }

      ImGui::PushStyleColor (ImGuiCol_Text, ImColor::HSV (0.23f, 0.86f, 1.f));
      ImGui::Text ("NOTE: ");
      ImGui::PushStyleColor (ImGuiCol_Text, ImColor::HSV (0.4f, 0.28f, 0.94f)); ImGui::SameLine ();
      ImGui::Text ("This is pure magic"); if (ImGui::IsItemHovered ()) ImGui::SetTooltip ("Essence of Pirate reasoning is required; harvesting takes time and quite possibly your sanity.");
      ImGui::PopStyleColor  (2);
      
      ////if (ImGui::IsItemHovered ())
      ////{
      ////  ImGui::BeginTooltip ();
      ////  ImGui::Text         (" Future Work");
      ////  ImGui::Separator    ();
      ////  ImGui::BulletText   ("Automagic Dark and Light Adaptation are VERY Important and VERY Absent :P");
      ////  ImGui::BulletText   ("Separable HUD luminance (especially weapon cross-hairs) also must be implemented");
      ////  ImGui::EndTooltip   ();
      ////}
    }

    if ( ( TenBitSwap_Original     != __SK_HDR_10BitSwap ||
           SixteenBitSwap_Original != __SK_HDR_16BitSwap) )
    {
      ImGui::PushStyleColor (ImGuiCol_Text, ImColor::HSV (.3f, .8f, .9f));
      ImGui::BulletText     ("Game Restart Required");
      ImGui::PopStyleColor  ();
    }

    if ( __SK_HDR_10BitSwap ||
         __SK_HDR_16BitSwap    )
    {
      CComQIPtr <IDXGISwapChain4> pSwap4 (rb.swapchain);

      if (pSwap4.p != nullptr)
      {
        DXGI_OUTPUT_DESC1     out_desc = { };
        DXGI_SWAP_CHAIN_DESC swap_desc = { };
           pSwap4->GetDesc (&swap_desc);

        if (out_desc.BitsPerColor == 0)
        {
          CComPtr <IDXGIOutput>
            pOutput = nullptr;

          if ( SUCCEEDED (
                pSwap4->GetContainingOutput (&pOutput.p)
                         ) 
             )
          {
            CComQIPtr <IDXGIOutput6> pOutput6 (pOutput);
                                     pOutput6->GetDesc1 (&out_desc);
          }

          else
          {
            out_desc.BitsPerColor = 8;
          }
        }

        {
          //const DisplayChromacities& Chroma = DisplayChromacityList[selectedChroma];
          DXGI_HDR_METADATA_HDR10 HDR10MetaData = {};

          static int cspace = 1;

          struct DisplayChromacities
          {
            float RedX;
            float RedY;
            float GreenX;
            float GreenY;
            float BlueX;
            float BlueY;
            float WhiteX;
            float WhiteY;
          } const DisplayChromacityList [] =
          {
            { 0.64000f, 0.33000f, 0.30000f, 0.60000f, 0.15000f, 0.06000f, 0.31270f, 0.32900f }, // Display Gamut Rec709 
            { 0.70800f, 0.29200f, 0.17000f, 0.79700f, 0.13100f, 0.04600f, 0.31270f, 0.32900f }, // Display Gamut Rec2020
            { out_desc.RedPrimary   [0], out_desc.RedPrimary   [1],
              out_desc.GreenPrimary [0], out_desc.GreenPrimary [1],
              out_desc.BluePrimary  [0], out_desc.BluePrimary  [1],
              out_desc.WhitePoint   [0], out_desc.WhitePoint   [1] }
            };

          ImGui::TreePush ("");

          bool hdr_gamut_support (false);

          if (swap_desc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
          {
            hdr_gamut_support = true;
          }

          if ( swap_desc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
               rb.scanout.getEOTF ()       == SK_RenderBackend::scan_out_s::SMPTE_2084 )
          {
            hdr_gamut_support = true;
            ImGui::RadioButton ("Rec 709",  &cspace, 0); ImGui::SameLine (); 
          }
          //else if (cspace == 0) cspace = 1;

          if ( swap_desc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
               rb.scanout.getEOTF ()       == SK_RenderBackend::scan_out_s::SMPTE_2084 )
          {
            hdr_gamut_support = true;
            ImGui::RadioButton ("Rec 2020", &cspace, 1); ImGui::SameLine ();
          }
          //else if (cspace == 1) cspace = 0;

          if ( swap_desc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
               rb.scanout.getEOTF ()       == SK_RenderBackend::scan_out_s::SMPTE_2084 )
          {
            ImGui::RadioButton ("Native",   &cspace, 2); ImGui::SameLine ();
          }

          if ( swap_desc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
               rb.scanout.getEOTF ()       == SK_RenderBackend::scan_out_s::SMPTE_2084 )// hdr_gamut_support)
          {
              HDR10MetaData.RedPrimary   [0] =
                static_cast <UINT16> (DisplayChromacityList [cspace].RedX * 50000.0f);
              HDR10MetaData.RedPrimary   [1] =
                static_cast <UINT16> (DisplayChromacityList [cspace].RedY * 50000.0f);

              HDR10MetaData.GreenPrimary [0] =
                static_cast <UINT16> (DisplayChromacityList [cspace].GreenX * 50000.0f);
              HDR10MetaData.GreenPrimary [1] =
                static_cast <UINT16> (DisplayChromacityList [cspace].GreenY * 50000.0f);

              HDR10MetaData.BluePrimary  [0] =
                static_cast <UINT16> (DisplayChromacityList [cspace].BlueX * 50000.0f);
              HDR10MetaData.BluePrimary  [1] =
                static_cast <UINT16> (DisplayChromacityList [cspace].BlueY * 50000.0f);

              HDR10MetaData.WhitePoint   [0] =
                static_cast <UINT16> (DisplayChromacityList [cspace].WhiteX * 50000.0f);
              HDR10MetaData.WhitePoint   [1] =
                static_cast <UINT16> (DisplayChromacityList [cspace].WhiteY * 50000.0f);

              static float fLuma [4] = { out_desc.MaxLuminance,
                                         out_desc.MinLuminance,
                                         2000.0f,       600.0f };

              ImGui::InputFloat4 ("Luminance Coefficients", fLuma, 1);

              HDR10MetaData.MaxMasteringLuminance     = static_cast <UINT>   (fLuma [0] * 10000.0f);
              HDR10MetaData.MinMasteringLuminance     = static_cast <UINT>   (fLuma [1] * 10000.0f);
              HDR10MetaData.MaxContentLightLevel      = static_cast <UINT16> (fLuma [2]);
              HDR10MetaData.MaxFrameAverageLightLevel = static_cast <UINT16> (fLuma [3]);
          }

          ImGui::Separator ();

          ImGui::BeginGroup ();
          if (ImGui::Checkbox ("Enable FULL HDR Luminance", &__SK_HDR_FullRange))
          {
            _SK_HDR_FullRange->store (__SK_HDR_FullRange);
          }

          if (ImGui::IsItemHovered ())
            ImGui::SetTooltip ("Brighter values are possible with this on, but may clip white/black detail");

          ////ImGui::SameLine ();
          ////
          ////static bool open = false;
          ////
          ////open ^= ImGui::Button ("Edit Shaders");
          ////
          ////ImGui::Begin ( "Shader Editor",
          ////                 &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_ShowBorders );
          ////
          ////if (open)
          ////{
          ////  static char szBuf [16384 * 4] = { };
          ////
          ////  ImGui::InputTextMultiline  ("Shader Editor", szBuf, 16384 * 4 - 2);
          ////  ImGui::Button              ("Save");
          ////  ImGui::SameLine            ();
          ////  ImGui::Button              ("Reload");
          ////}
          ////
          ////ImGui::End ();

          ////ImGui::SameLine ();
          ////
          ////ImGui::SliderFloat ("###DQXI_UI_LUMINANCE", &__SK_DQXI_UI_Luma, -50.0f, 50.0f, "UI Luminance: %.3f");

          auto& pINI = dll_ini;

          float nits =
            __SK_HDR_Luma / 1.0_Nits;

          if (ImGui::SliderFloat ( "###SK_HDR_LUMINANCE", &nits, 80.0f, __SK_HDR_FullRange ?
                                                                          rb.display_gamut.maxLocalY :
                                                                          rb.display_gamut.maxY,
              u8"Peak White Luminance: %.1f cd/m²" ))
          {
            __SK_HDR_Luma = nits * 1.0_Nits;

            auto& preset =
              hdr_presets [__SK_HDR_Preset];

            preset.peak_white_nits =
              nits * 1.0_Nits;
            preset.cfg_nits->store (preset.peak_white_nits);

            pINI->write (pINI->get_filename ());
          }

          //ImGui::SameLine ();

          if (ImGui::SliderFloat ("###SK_HDR_GAMMA", &__SK_HDR_Exp, 1.0f, 2.4f, "SDR -> HDR Gamma: %.3f"))
          {
            auto& preset =
              hdr_presets [__SK_HDR_Preset];

            preset.eotf =
              __SK_HDR_Exp;
            preset.cfg_eotf->store (preset.eotf);

            pINI->write (pINI->get_filename ());
          }
          ImGui::EndGroup   ();
          ImGui::SameLine   ();
          ImGui::BeginGroup ();
          for ( int i = 0 ; i < MAX_HDR_PRESETS ; i++ )
          {
            bool selected =
              (__SK_HDR_Preset == i);

            if (ImGui::Selectable ( hdr_presets [i].preset_name, &selected, ImGuiSelectableFlags_SpanAllColumns ))
            {
              hdr_presets [i].activate ();
            }
          }
          ImGui::EndGroup   ();
          ImGui::SameLine   (); ImGui::Spacing ();
          ImGui::SameLine   (); ImGui::Spacing ();
          ImGui::SameLine   (); ImGui::Spacing (); ImGui::SameLine (); 
          ImGui::BeginGroup ();
          for ( int i = 0 ; i < MAX_HDR_PRESETS ; i++ )
          {
            ImGui::Text ( u8"Peak White: %5.1f cd/m²",
                          hdr_presets [i].peak_white_nits / 1.0_Nits );
          }
          ImGui::EndGroup   ();
          ImGui::SameLine   (); ImGui::Spacing ();
          ImGui::SameLine   (); ImGui::Spacing ();
          ImGui::SameLine   (); ImGui::Spacing (); ImGui::SameLine ();
          ImGui::BeginGroup ();
          for ( int i = 0 ; i < MAX_HDR_PRESETS ; i++ )
          {
            ImGui::Text ( u8"Power-Law ɣ: %3.1f",
                            hdr_presets [i].eotf );
          }
          ImGui::EndGroup   ();
          ImGui::SameLine   (); ImGui::Spacing ();
          ImGui::SameLine   (); ImGui::Spacing ();
          ImGui::SameLine   (); ImGui::Spacing ();
          ImGui::SameLine   (); ImGui::Spacing ();
          ImGui::SameLine   (); ImGui::Spacing ();
          ImGui::SameLine   (); ImGui::Spacing (); ImGui::SameLine (); 
          ImGui::BeginGroup ();

          for ( int i = 0 ; i < MAX_HDR_PRESETS ; i++ )
          {
            Keybinding ( &hdr_presets [i].preset_activate,
                        (&hdr_presets [i].preset_activate)->param );
          }
          ImGui::EndGroup   ();
          ImGui::Separator  ();

          static bool success = true;
          if (ImGui::Button ("Recompile HDR Shaders"))
          {
            extern bool SK_HDR_RecompileShaders (void);
            success =   SK_HDR_RecompileShaders (    );
          }

          if (! success) { ImGui::SameLine (); ImGui::TextUnformatted ("You dun screwed up!"); }

          ImGui::Separator ();

          extern int   __SK_HDR_input_gamut;
          extern int   __SK_HDR_output_gamut;
          extern int   __SK_HDR_visualization;
          extern float __SK_HDR_user_sdr_Y;

          ImGui::BeginGroup  ();
          ImGui::Combo       ("Source ColorSpace##SK_HDR_GAMUT_IN",  &__SK_HDR_input_gamut,  "Unaltered\0Rec. 709\0DCI-P3\0Rec. 2020\0CIE XYZ\0\0");
          ImGui::Combo       ("Output ColorSpace##SK_HDR_GAMUT_OUT", &__SK_HDR_output_gamut, "Unaltered\0Rec. 709\0DCI-P3\0Rec. 2020\0CIE XYZ\0\0");
          ImGui::EndGroup    ();
          //ImGui::SameLine    ();
          ImGui::BeginGroup  ();
          ImGui::SliderFloat ("Horz. (%) HDR Processing", &__SK_HDR_HorizCoverage, 0.0f, 100.f);
          ImGui::SliderFloat ("Vert. (%) HDR Processing", &__SK_HDR_VertCoverage,  0.0f, 100.f);
          ImGui::EndGroup    ();

          ImGui::BeginGroup  ();
          ImGui::SliderFloat ("Non-Std. SDR Peak White", &__SK_HDR_user_sdr_Y, 80.0f, 400.0f, u8"%.3f cd/m²");

          if (ImGui::IsItemHovered ())
          {
            ImGui::BeginTooltip    (  );
            ImGui::TextUnformatted (u8"Technically 80.0 cd/m² is the Standard Peak White Luminance for sRGB");
            ImGui::Separator       (  );
            ImGui::BulletText      ("It is doubtful you are accustomed to an image that dim, so...");
            ImGui::BulletText      ("Supply your own expected luminance (i.e. a familiar display's rated peak brightness)");
            ImGui::Separator       (  );
            ImGui::TreePush        ("");
            ImGui::TextColored     (ImVec4 (1.f, 1.f, 1.f, 1.f),
                                    "This will assist during visualization and split-screen HDR/SDR comparisons");
            ImGui::TreePop         (  );
            ImGui::EndTooltip      (  );
          }
          //ImGui::SameLine    ();

          ImGui::Combo       ("HDR Visualization##SK_HDR_VIZ",  &__SK_HDR_visualization, "None\0SDR=Monochrome//HDR=FalseColors\0\0");
          ImGui::EndGroup    ();


          if ( swap_desc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
               rb.scanout.getEOTF ()       == SK_RenderBackend::scan_out_s::SMPTE_2084 )
          {
            ImGui::BulletText ("HDR May Not be Working Correctly Until you Engage Fullscreen Exclusive Mode...");

            ImGui::Separator ();
            if (ImGui::Button ("Inject HDR10 Metadata"))
            {
              //if (cspace == 2)
              //  swap_desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
              //else if (cspace == 1)
              //  swap_desc.BufferDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
              //else

              /////if (rb.scanout.bpc == 10)
              /////  swap_desc.BufferDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
          
              //pSwap4->SetHDRMetaData (DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
              //
              //if (swap_desc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
              //{
              //  pSwap4->SetColorSpace1 (DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
              //}
          
              //else if (cspace == 0) pSwap4->SetColorSpace1 (DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
              //else                  pSwap4->SetColorSpace1 (DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
          
              pSwap4->SetHDRMetaData (DXGI_HDR_METADATA_TYPE_HDR10, sizeof (HDR10MetaData), &HDR10MetaData);
              pSwap4->SetColorSpace1 (DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            }
          }

          /////else
          /////{
          /////  ImGui::PushStyleColor (ImGuiCol_Text, ImColor::HSV (0.075f, 1.0f, 1.0f));
          /////  ImGui::BulletText     ("A waitable swapchain is required for HDR10 (D3D11 Settings/SwapChain | {Flip Model + Waitable}");
          /////  ImGui::PopStyleColor  ();
          /////}

          ImGui::TreePop ();
        }
      }
    }
  }

  virtual void OnConfig (ConfigEvent event) override
  {
    switch (event)
    {
      case SK_Widget::ConfigEvent::LoadComplete:
        break;

      case SK_Widget::ConfigEvent::SaveStart:
        break;
    }
  }

protected:

private:
} __dxgi_hdr__;