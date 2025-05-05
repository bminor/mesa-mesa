/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#pragma once

#include "mfd3dmanager.h"
#include <algorithm>
#include <iterator>
#include <map>
#include <vector>
#include "wpptrace.h"

#include "mfd3dmanager.tmh"

static IDXCoreAdapter *
choose_dxcore_adapter( void )
{
   HRESULT hr = S_OK;
   std::vector<MFTAdapterInfo> adapter_infos;
   ComPtr<IDXCoreAdapterFactory> spFactory;
   ComPtr<IDXCoreAdapterList> spAdapterList;
   adapter_infos.reserve( 2 );

   hr = DXCoreCreateAdapterFactory( IID_IDXCoreAdapterFactory, (void **) spFactory.GetAddressOf() );
   if( FAILED( hr ) )
   {
      debug_printf( "CMFD3DManager: DXCoreCreateAdapterFactory failed: %08x\n", hr );
      return NULL;
   }

#ifdef NTDDI_WIN11_GA
   // Get all media adapters (e.g including MCDM using latest DXCore APIs)
   ComPtr<IDXCoreAdapterFactory1> spFactory1;
   if( SUCCEEDED( spFactory.As( &spFactory1 ) ) &&
       SUCCEEDED( spFactory1->CreateAdapterListByWorkload( DXCoreWorkload::Media,
                                                           DXCoreRuntimeFilterFlags::D3D12,
                                                           DXCoreHardwareTypeFilterFlags::None,
                                                           spAdapterList.GetAddressOf() ) ) )
   {
      debug_printf( "CMFD3DManager: Using IDXCoreAdapterFactory1::CreateAdapterListByWorkload\n" );
   }
#endif   // NTDDI_WIN11_GA

   // Fallback to older DXCore enumeration APIs
   if( !spAdapterList &&
       SUCCEEDED( spFactory->CreateAdapterList( 1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE, spAdapterList.GetAddressOf() ) ) )
   {
      debug_printf( "CMFD3DManager: Fallback to IDXCoreAdapterFactory::CreateAdapterList since "
                    "IDXCoreAdapterFactory1::CreateAdapterListByWorkload was not available\n" );
   }

   // Validate we enumerated one way or another
   if( !spAdapterList )
   {
      debug_printf( "CMFD3DManager: Couldn't create an adapter list\n" );
      return NULL;
   }

   adapter_infos.clear();
   for( unsigned i = 0; i < spAdapterList->GetAdapterCount(); i++ )
   {
      ComPtr<IDXCoreAdapter> spAdapter;
      MFTAdapterInfo adapter_info = {};
      uint64_t driver_version = 0;
      if( SUCCEEDED( spAdapterList->GetAdapter( i, spAdapter.GetAddressOf() ) ) )
      {
         if( FAILED( spAdapter->GetProperty( DXCoreAdapterProperty::IsIntegrated, &adapter_info.is_integrated ) ) )
            continue;
         if( FAILED( spAdapter->GetProperty( DXCoreAdapterProperty::HardwareID, &adapter_info.hardware_id ) ) )
            continue;
         if( FAILED( spAdapter->GetProperty( DXCoreAdapterProperty::InstanceLuid, &adapter_info.adapter_luid ) ) )
            continue;
         if( FAILED( spAdapter->GetProperty( DXCoreAdapterProperty::DriverVersion, &driver_version ) ) )
            continue;

         // Read into driver_version variable first since the parts in the struct are in high to low bits order
         adapter_info.driver_version.part1 = ( ( driver_version >> 48 ) & 0xFFFF );
         adapter_info.driver_version.part2 = ( ( driver_version >> 32 ) & 0xFFFF );
         adapter_info.driver_version.part3 = ( ( driver_version >> 16 ) & 0xFFFF );
         adapter_info.driver_version.part4 = ( driver_version & 0xFFFF );
         adapter_infos.push_back( adapter_info );
      }
   }

   std::map<uint32_t, std::string> vendor_id_friendly_names = {
      { 0x1002, "AMD" },
      { 0x1414, "Microsoft" },
      { 0x10DE, "NVidia" },
      { 0x8086, "Intel" },
   };

   const std::vector<uint32_t> vendor_preference_order = {
      0x10DE,   // NVidia
      0x1002,   // AMD
      0x8086,   // Intel
      0x1414,   // Microsoft
   };

   std::map<uint32_t, MFAdapterDriverVersion> driver_min_versions;
   {
      // Min driver versions
      driver_min_versions[0x1002 /* AMD */] = { 31, 0, 0, 0 };
      driver_min_versions[0x1414 /* Microsoft */] = { 10, 0, 26000, 0 };   // OS version for MSFT SW driver
      driver_min_versions[0x10DE /* NVidia */] = { 31, 0, 0, 0 };
      driver_min_versions[0x8086 /* Intel */] = { 31, 0, 0, 0 };
   }

   adapter_infos.erase(
      std::remove_if( adapter_infos.begin(),
                      adapter_infos.end(),
                      [&driver_min_versions]( const MFTAdapterInfo &adapter ) {
                         return ( adapter.driver_version.part1 < driver_min_versions[adapter.hardware_id.vendorID].part1 ) ||
                                ( adapter.driver_version.part2 < driver_min_versions[adapter.hardware_id.vendorID].part2 ) ||
                                ( adapter.driver_version.part3 < driver_min_versions[adapter.hardware_id.vendorID].part3 ) ||
                                ( adapter.driver_version.part4 < driver_min_versions[adapter.hardware_id.vendorID].part4 );
                      } ),
      adapter_infos.end() );

   std::map<uint32_t, std::vector<MFAdapterDriverVersion>> driver_denylist;
   {
      // Blocked driver versions
      driver_denylist[0x1002 /* AMD */] = { { 31, 0, 0, 0 } };
      driver_denylist[0x1414 /* Microsoft */] = { { 10, 0, 26000, 0 } };   // OS version for MSFT SW driver
      driver_denylist[0x10DE /* NVidia */] = { { 31, 0, 0, 0 } };
      driver_denylist[0x8086 /* Intel */] = { { 31, 0, 0, 0 } };
   }

   adapter_infos.erase( std::remove_if( adapter_infos.begin(),
                                        adapter_infos.end(),
                                        [&driver_denylist]( const MFTAdapterInfo &adapter ) {
                                           return std::find_if( driver_denylist[adapter.hardware_id.vendorID].begin(),
                                                                driver_denylist[adapter.hardware_id.vendorID].end(),
                                                                [&adapter]( const MFAdapterDriverVersion &x ) {
                                                                   return x.version == adapter.driver_version.version;
                                                                } ) != driver_denylist[adapter.hardware_id.vendorID].end();
                                        } ),
                        adapter_infos.end() );

   std::sort( adapter_infos.begin(), adapter_infos.end(), [vendor_preference_order]( MFTAdapterInfo a, MFTAdapterInfo b ) {
      // First criteria: iGPU first
      if( a.is_integrated > b.is_integrated )
         return true;
      if( a.is_integrated < b.is_integrated )
         return false;

      // Second criteria: IHV preference
      size_t preferenceOrderA =
         std::distance( vendor_preference_order.begin(),
                        std::find( vendor_preference_order.begin(), vendor_preference_order.end(), a.hardware_id.vendorID ) );
      size_t preferenceOrderB =
         std::distance( vendor_preference_order.begin(),
                        std::find( vendor_preference_order.begin(), vendor_preference_order.end(), b.hardware_id.vendorID ) );
      if( preferenceOrderA < preferenceOrderB )
         return true;
      if( preferenceOrderA > preferenceOrderB )
         return false;

      // Third criteria: driver version
      if( a.driver_version.version > b.driver_version.version )
         return true;
      if( a.driver_version.version < b.driver_version.version )
         return false;

      return false;
   } );

   debug_printf( "CMFD3DManager: Selecting adapter from adapter list...\n" );
   for( size_t i = 0; i < adapter_infos.size(); i++ )
   {
      debug_printf( "CMFD3DManager: %s Adapter LUID (%d %d) - is_integrated %d - vendor_id 0x%x (%s) - driver_version "
                    "%d.%d.%d.%d \n",
                    ( i == 0 ) ? "[SELECTED]" : "",
                    adapter_infos[i].adapter_luid.LowPart,
                    adapter_infos[i].adapter_luid.HighPart,
                    adapter_infos[i].is_integrated,
                    adapter_infos[i].hardware_id.vendorID,
                    vendor_id_friendly_names.count( adapter_infos[i].hardware_id.vendorID ) > 0 ?
                       vendor_id_friendly_names[adapter_infos[i].hardware_id.vendorID].c_str() :
                       "Unknown",
                    adapter_infos[i].driver_version.part1,
                    adapter_infos[i].driver_version.part2,
                    adapter_infos[i].driver_version.part3,
                    adapter_infos[i].driver_version.part4 );
   }

   IDXCoreAdapter *selected_adapter = NULL;
   if( ( adapter_infos.size() == 0 ) || FAILED( spFactory->GetAdapterByLuid( adapter_infos[0].adapter_luid, &selected_adapter ) ) )
      debug_printf( "CMFD3DManager: Error, no adapters found.\n" );

   return selected_adapter;
}

CMFD3DManager::CMFD3DManager()
{ }

CMFD3DManager::~CMFD3DManager()
{
   Shutdown();
}

// #define ENABLE_D3D12_DEBUG_LAYER

HRESULT
CMFD3DManager::Initialize( D3D12_VIDEO_ENCODER_CODEC codec )
{
#ifdef ENABLE_D3D12_DEBUG_LAYER
   ComPtr<ID3D12Debug> spDebugController;
   if( SUCCEEDED( D3D12GetDebugInterface( IID_PPV_ARGS( &spDebugController ) ) ) )
   {
      spDebugController->EnableDebugLayer();
   }
#endif
   m_codec = codec;
   return S_OK;
}

HRESULT
CMFD3DManager::Shutdown( bool bReleaseDeviceManager )
{
   HRESULT hr = S_OK;

   m_spDevice = nullptr;
   m_spVideoDevice = nullptr;
   m_spDevice11 = nullptr;
   m_spStagingQueue = nullptr;

   if( m_spVideoSampleAllocator )
   {
      m_spVideoSampleAllocator->UninitializeSampleAllocator();
      m_spVideoSampleAllocator = nullptr;
   }

   if( m_spDeviceManager != nullptr )
   {
      if( m_hDevice != NULL )
      {
         m_spDeviceManager->CloseDeviceHandle( m_hDevice );
         m_hDevice = NULL;
      }
      if( bReleaseDeviceManager )
         m_spDeviceManager = nullptr;
   }

   if( m_pPipeContext )
   {
      m_pPipeContext->destroy( m_pPipeContext );
      m_pPipeContext = nullptr;
   }

   if( m_pVlScreen )
   {
      m_pVlScreen->destroy( this->m_pVlScreen );
      m_pVlScreen = nullptr;
   }

   if( m_pWinsys )
   {
      m_pWinsys->destroy( this->m_pWinsys );
      m_pWinsys = nullptr;
   }

   return hr;
}

static inline HRESULT
CreateD3D12DeviceWithMinimumSupportedFeatureLevel( IUnknown *pAdapter, ComPtr<ID3D12Device> &spDevice )
{
   static const D3D_FEATURE_LEVEL levels[] = {
#if ( D3D12_SDK_VERSION >= 611 )
      D3D_FEATURE_LEVEL_1_0_GENERIC,
#endif
      D3D_FEATURE_LEVEL_1_0_CORE,
      D3D_FEATURE_LEVEL_11_0,
   };

   for( uint32_t i = 0; i < ARRAYSIZE( levels ); i++ )
      if( SUCCEEDED( D3D12CreateDevice( pAdapter, levels[i], IID_PPV_ARGS( spDevice.ReleaseAndGetAddressOf() ) ) ) )
         return S_OK;

   return E_FAIL;
}

HRESULT
CMFD3DManager::xReopenDeviceManager( bool bNewDevice )
{
   HRESULT hr = S_OK;
   D3D12_COMMAND_QUEUE_DESC commandQueueDesc = { D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE };
   D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC capCodecData = { 0U, m_codec, false };
   Shutdown( false );
   CHECKHR_GOTO( m_spDeviceManager->OpenDeviceHandle( &m_hDevice ), done );
   if( FAILED( m_spDeviceManager->GetVideoService( m_hDevice, IID_ID3D12Device, &m_spDevice ) ) )
   {
      ComPtr<IDXGIDevice> spDXGIDevice;
      ComPtr<IDXGIAdapter> spDXGIAdapter;
      ComPtr<IUnknown> spAdapter;
      CHECKHR_GOTO( m_spDeviceManager->GetVideoService( m_hDevice, IID_ID3D11Device, &m_spDevice11 ), done );
      CHECKHR_GOTO( m_spDevice11.As( &spDXGIDevice ), done );
      CHECKHR_GOTO( spDXGIDevice->GetAdapter( &spDXGIAdapter ), done );
      CHECKHR_GOTO( spDXGIAdapter.As( &spAdapter ), done );
      // Create a D3D12 device off of the same adapter this 11 device is on
      CHECKHR_GOTO( CreateD3D12DeviceWithMinimumSupportedFeatureLevel( spAdapter.Get(), m_spDevice ), done );
   }
   // Create a staging queue for MF to signal on input texture GPU completion
   CHECKHR_GOTO( m_spDevice->CreateCommandQueue( &commandQueueDesc, IID_PPV_ARGS( &m_spStagingQueue ) ), done );
   CHECKHR_GOTO( m_spDevice.As( &m_spVideoDevice ), done );
   CHECKHR_GOTO( m_spVideoDevice->CheckFeatureSupport( D3D12_FEATURE_VIDEO_ENCODER_CODEC, &capCodecData, sizeof( capCodecData ) ),
                 done );
   CHECKBOOL_GOTO( capCodecData.IsSupported, MF_E_UNSUPPORTED_D3D_TYPE, done );
done:
   return hr;
}

HRESULT
CMFD3DManager::xOnSetD3DManager( ULONG_PTR ulParam )
{
   HRESULT hr = S_OK;
   Shutdown();

   if( ulParam == 0 )
   {
#if 0      
      // Treat 0 as "pick a default"
      ComPtr<ID3D12Device> spD3D12Device;
      ComPtr<IDXCoreAdapter> dxcore_adapter = choose_dxcore_adapter();
      CHECKNULL_GOTO(dxcore_adapter, MF_E_DXGI_DEVICE_NOT_INITIALIZED, done);
      CHECKHR_GOTO(MFCreateDXGIDeviceManager(&m_uiResetToken, &m_spDeviceManager), done);
      CHECKHR_GOTO(CreateD3D12DeviceWithMinimumSupportedFeatureLevel(dxcore_adapter.Get(), spD3D12Device), done);
      CHECKHR_GOTO(m_spDeviceManager->ResetDevice(spD3D12Device.Get(), m_uiResetToken), done);
#else
      return hr;
#endif
   }
   else
   {
      // We've been given an IUnknown, make sure it is an IMFDXGIDeviceManager
      CHECKHR_GOTO( reinterpret_cast<IUnknown *>( ulParam )->QueryInterface( IID_PPV_ARGS( &m_spDeviceManager ) ), done );
   }
   CHECKHR_GOTO( xReopenDeviceManager( true ), done );

   CHECKNULL_GOTO( m_pWinsys = null_sw_create(), MF_E_DXGI_DEVICE_NOT_INITIALIZED, done );

   CHECKNULL_GOTO( m_pVlScreen = vl_win32_screen_create_from_d3d12_device( m_spDevice.Get(), m_pWinsys ),
                   MF_E_DXGI_DEVICE_NOT_INITIALIZED,
                   done );
   CHECKNULL_GOTO( m_pPipeContext = pipe_create_multimedia_context( m_pVlScreen->pscreen, false ),
                   MF_E_DXGI_DEVICE_NOT_INITIALIZED,
                   done );
   CHECKHR_GOTO( MFCreateVideoSampleAllocatorEx( IID_PPV_ARGS( &m_spVideoSampleAllocator ) ), done );
done:
   if( FAILED( hr ) )
   {
      Shutdown();
   }
   return hr;
}
