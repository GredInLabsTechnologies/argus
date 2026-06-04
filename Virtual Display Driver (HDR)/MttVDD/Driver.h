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
        ///
        /// ROUND-2 FIX (Issue 3 — Fix-5 timeout UAF): the worker thread dereferences `this`
        /// (m_Device, m_hSwapChain, and calls WdfObjectDelete on the swap-chain) after RunCore()
        /// returns. The old destructor bounded its join to 5s and, on WAIT_TIMEOUT, returned and
        /// let the object be freed while the worker could still touch it → use-after-free + a
        /// possible double swap-chain delete. The object is now owned by std::shared_ptr (stored
        /// in m_ProcessingThreads); RunThread() holds its OWN shared_ptr copy for the whole worker
        /// lifetime, so the object can NEVER be freed while the worker can still touch it. The
        /// last reference to drop runs the destructor — which may therefore be the worker thread
        /// itself; the destructor detects that and skips the self-join (a thread can't join
        /// itself). Construct via std::make_shared<SwapChainProcessor>(...) then call Start().
        /// </summary>
        class SwapChainProcessor : public std::enable_shared_from_this<SwapChainProcessor>
        {
        public:
            SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
            ~SwapChainProcessor();

            // Starts the worker thread. MUST be called exactly once, right after the object is
            // owned by a std::shared_ptr (it hands the worker a shared_ptr lifetime hold via
            // shared_from_this()). Not in the constructor, because shared_from_this() is only
            // valid once a shared_ptr owns the object.
            void Start();

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
            // Worker OS thread id, captured when the thread starts. Used by ~SwapChainProcessor to
            // detect "am I being destroyed ON the worker thread?" (the timed-out-teardown case,
            // where the worker holds the last shared_ptr) and skip the self-join.
            DWORD m_ThreadId = 0;
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

            // The IddCx adapter must be created EXACTLY ONCE per device lifetime. EvtDeviceD0Entry
            // fires on every D0 transition (initial start AND every sleep/hibernate wake), but
            // IddCxAdapterInitAsync may be called only once — calling it again on wake would create
            // a SECOND adapter, overwrite m_Adapter, and orphan every monitor bound to the original
            // adapter. This flag guards the one-time init; subsequent D0 entries skip it (IddCx
            // re-establishes swapchains itself per its D0 semantics).
            bool m_AdapterInitialized = false;

            // Live monitors keyed by connector index. Replaces the old fixed m_Monitor / m_Monitor2.
            // GUARDED BY m_MonitorsMutex. This mutex MUST remain separate from
            // m_ProcessingThreadsMutex: IddCxMonitorDeparture may synchronously call
            // UnassignSwapChain (which takes m_ProcessingThreadsMutex), so departure must NEVER
            // be invoked while either lock is held (see RemoveMonitor for the lock-then-unlock dance).
            std::map<UINT, IDDCX_MONITOR> m_Monitors;
            std::mutex m_MonitorsMutex;

            // ROUND-2 FIX (Issue 3): shared_ptr (was unique_ptr) so the worker thread can hold its
            // own lifetime hold on the processor (see SwapChainProcessor). Dropping the map's
            // reference during teardown does NOT free the object while the worker is still running.
            std::map<IDDCX_MONITOR, std::shared_ptr<SwapChainProcessor>> m_ProcessingThreads;
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
