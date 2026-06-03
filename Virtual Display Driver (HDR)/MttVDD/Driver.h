#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <IddCx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include <string>
#include <sstream>

#include "Trace.h"

// Maximum number of virtual monitors the adapter advertises (AdapterCaps.MaxMonitorsSupported).
// Decoupled from the configured display count so the adapter can idle at 0 monitors and have ONE
// monitor added/removed per connector index at runtime without an adapter reload.
#ifndef MAX_MONITORS
#define MAX_MONITORS 10
#endif

// Utility function declarations
std::vector<std::string> split(std::string& input, char delimiter);
std::string WStringToString(const std::wstring& wstr);

// Phase 5: Final Integration function declarations
NTSTATUS ValidateEdidIntegration();
NTSTATUS PerformanceMonitor();
NTSTATUS CreateFallbackConfiguration();
NTSTATUS ValidateAndSanitizeConfiguration();
NTSTATUS RunComprehensiveDiagnostics();
NTSTATUS InitializePhase5Integration();

namespace Microsoft
{
    namespace WRL
    {
        namespace Wrappers
        {
            // Adds a wrapper for thread handles to the existing set of WRL handle wrapper classes
            typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
        }
    }
}

namespace Microsoft
{
    namespace IndirectDisp
    {
        /// <summary>
        /// Manages the creation and lifetime of a Direct3D render device.
        /// </summary>
        struct Direct3DDevice
        {
            Direct3DDevice(LUID AdapterLuid);
            Direct3DDevice();
            HRESULT Init();

            LUID AdapterLuid;
            Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
            Microsoft::WRL::ComPtr<ID3D11Device> Device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
        };

        /// <summary>
        /// Manages a thread that consumes buffers from an indirect display swap-chain object.
        /// </summary>
        class SwapChainProcessor
        {
        public:
            SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
            ~SwapChainProcessor();

        private:
            static DWORD CALLBACK RunThread(LPVOID Argument);

            void Run();
            void RunCore();

        public:
            IDDCX_SWAPCHAIN m_hSwapChain;
            std::shared_ptr<Direct3DDevice> m_Device;
            HANDLE m_hAvailableBufferEvent;
            Microsoft::WRL::Wrappers::Thread m_hThread;
            Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
        };

        /// <summary>
        /// Custom comparator for LUID to be used in std::map
        /// </summary>
        struct LuidComparator
        {
            bool operator()(const LUID& a, const LUID& b) const
            {
                if (a.HighPart != b.HighPart)
                    return a.HighPart < b.HighPart;
                return a.LowPart < b.LowPart;
            }
        };

        /// <summary>
        /// Provides a sample implementation of an indirect display driver.
        /// </summary>
        class IndirectDeviceContext
        {
        public:
            IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
            virtual ~IndirectDeviceContext();

            void InitAdapter();
            void FinishInit();

            // Legacy entry point: create a monitor at a fixed connector index. Now a thin shim
            // that routes to AddMonitor so the index map stays authoritative.
            void CreateMonitor(unsigned int index);

            // ===== On-demand (spacedesk-style) monitor management =====
            // AddMonitor: plug ONE monitor at the given connector index at runtime via
            //   IddCxMonitorCreate + IddCxMonitorArrival. Stores the handle in m_Monitors[index]
            //   under m_MonitorsMutex. Rejects if the index is already live or >= MAX_MONITORS.
            // Returns true on success.
            bool AddMonitor(UINT index);
            // RemoveMonitor: unplug ONE monitor by index via IddCxMonitorDeparture, then erase
            //   from m_Monitors, g_HdrMetadataStore and g_GammaRampStore. Returns true if removed.
            bool RemoveMonitor(UINT index);
            // RemoveAllMonitors: watchdog self-heal path; departs every live monitor.
            void RemoveAllMonitors();
            // LowestFreeIndex: lowest connector index in [0, MAX_MONITORS) not currently live,
            //   or -1 if all slots are taken. Reuses freed indices.
            int LowestFreeIndex();
            // LiveMonitorCount: number of currently live monitors (locks m_MonitorsMutex).
            size_t LiveMonitorCount();

            void AssignSwapChain(IDDCX_MONITOR Monitor, IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
            void UnassignSwapChain(IDDCX_MONITOR Monitor);

        protected:

            WDFDEVICE m_WdfDevice;
            IDDCX_ADAPTER m_Adapter;

            // Live monitors keyed by connector index. Replaces the old fixed m_Monitor / m_Monitor2.
            // GUARDED BY m_MonitorsMutex. This mutex MUST remain separate from
            // m_ProcessingThreadsMutex: IddCxMonitorDeparture may synchronously call
            // UnassignSwapChain (which takes m_ProcessingThreadsMutex), so departure must NEVER
            // be invoked while either lock is held (see RemoveMonitor for the lock-then-unlock dance).
            std::map<UINT, IDDCX_MONITOR> m_Monitors;
            std::mutex m_MonitorsMutex;

            std::map<IDDCX_MONITOR, std::unique_ptr<SwapChainProcessor>> m_ProcessingThreads;
            std::mutex m_ProcessingThreadsMutex;

        public:
            static const DISPLAYCONFIG_VIDEO_SIGNAL_INFO s_KnownMonitorModes[];
            static std::vector<BYTE> s_KnownMonitorEdid;

        private:
            // Builds + creates + reports-arrival for one monitor at the given connector index.
            // Returns the new IDDCX_MONITOR handle (or nullptr). Holds NO locks; the caller
            // (AddMonitor) owns m_Monitors bookkeeping. Never call with m_MonitorsMutex held.
            IDDCX_MONITOR CreateMonitorObject(UINT index);

            static std::map<LUID, std::shared_ptr<Direct3DDevice>, LuidComparator> s_DeviceCache;
            static std::mutex s_DeviceCacheMutex;
            static std::shared_ptr<Direct3DDevice> GetOrCreateDevice(LUID RenderAdapter);
            static void CleanupExpiredDevices();
        };
    }
}
