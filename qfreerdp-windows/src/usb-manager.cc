#include "usb-manager.h"
#include "qf_log.h"

#include <cstring>
#include <QMetaObject>
#include <map>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <algorithm>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

// Local DEVPROPKEY for BusReportedDeviceDesc.
// Values from devpkey.h:
//   {0x540b947e, 0x8b40, 0x45bc, {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}}, PID=4
static const DEVPROPKEY s_BusReportedDeviceDesc = {
    { 0x540b947e, 0x8b40, 0x45bc, { 0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2 } },
    4
};

namespace {
// Name cache: (VID << 16 | PID) → (product, manufacturer).
// Persists successfully read names so they survive USB redirection
// (when the device is no longer in the PnP tree).
static std::map<uint32_t, std::pair<std::string, std::string>> s_nameCache;

// ── Fast USB device name lookup via Windows SetupAPI ─────────────────
// Reads device names from the PnP device tree (registry cache) instead
// of calling libusb_open() + USB control transfers.  On a typical
// system this completes in < 50 ms, vs. 200-400 ms per device with
// the libusb approach.
//
// Returns  map: (VID << 16 | PID) → (product_name, manufacturer_name)
static std::map<uint32_t, std::pair<std::string, std::string>>
buildUsbNameMap_Win32()
{
    // Convert a UTF-16 (wchar_t) string to UTF-8 std::string.
    // This is needed because SetupAPI returns localized strings
    // (e.g. Chinese in GBK via the A-API, or UTF-16 via the W-API)
    // and we must store them as UTF-8 for compatibility with Qt.
    auto wideToUtf8 = [](const wchar_t* src) -> std::string {
        if (!src || !*src) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, src, -1,
                                      nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string result(static_cast<size_t>(len) - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, src, -1,
                            result.data(), len, nullptr, nullptr);
        return result;
    };

    std::map<uint32_t, std::pair<std::string, std::string>> map;

    // Enumerate ALL present devices, then filter by USB hardware IDs.
    // Using DIGCF_ALLCLASSES catches devices from any enumerator
    // (USB, USBSTOR, WPD, etc.) as long as their HWID contains VID_/PID_.
    // NOTE: We use the W (wide) API throughout so that device names are
    // returned as UTF-16, then converted to UTF-8.  The A (ANSI) API
    // would return strings in the system locale code page (e.g. GBK on
    // Chinese Windows), causing garbled text when treated as UTF-8.
    HDEVINFO devInfo = SetupDiGetClassDevsW(
        nullptr, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);

    if (devInfo == INVALID_HANDLE_VALUE)
        return map;

    SP_DEVINFO_DATA devData;
    devData.cbSize = sizeof(devData);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devData); i++)
    {
        wchar_t hwId[512] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(
                devInfo, &devData, SPDRP_HARDWAREID,
                nullptr, (PBYTE)hwId, sizeof(hwId), nullptr))
            continue;

        // USB device HWID always contains "VID_xxxx&PID_xxxx".
        unsigned vid = 0, pid = 0;
        {
            const wchar_t* vp = wcsstr(hwId, L"VID_");
            const wchar_t* pp = wcsstr(hwId, L"PID_");
            if (!vp || !pp)
                continue;
            if (swscanf_s(vp + 4, L"%x", &vid) < 1 ||
                swscanf_s(pp + 4, L"%x", &pid) < 1)
                continue;
        }

        uint32_t key = ((uint32_t)(uint16_t)vid << 16) | (uint16_t)pid;
        if (map.find(key) != map.end())
            continue; // already cached

        // Priority order for product name:
        //   1. BusReportedDeviceDesc — the actual USB string descriptor
        //      reported by the device itself (i.e. the real Product Name).
        //   2. FriendlyName — may include extra text like "USB Device".
        //   3. DeviceDesc — generic class name (e.g. "USB Mass Storage Device").
        wchar_t product[512] = {};
        DEVPROPTYPE propType;
        if (SetupDiGetDevicePropertyW(
                devInfo, &devData,
                &s_BusReportedDeviceDesc,
                &propType,
                (PBYTE)product, sizeof(product),
                nullptr, 0) && propType == DEVPROP_TYPE_STRING)
        {
            // BusReportedDeviceDesc is already the raw product name
        }
        else if (!SetupDiGetDeviceRegistryPropertyW(
                     devInfo, &devData, SPDRP_FRIENDLYNAME,
                     nullptr, (PBYTE)product, sizeof(product), nullptr))
        {
            SetupDiGetDeviceRegistryPropertyW(
                devInfo, &devData, SPDRP_DEVICEDESC,
                nullptr, (PBYTE)product, sizeof(product), nullptr);
        }

        wchar_t mfg[512] = {};
        SetupDiGetDeviceRegistryPropertyW(
            devInfo, &devData, SPDRP_MFG,
            nullptr, (PBYTE)mfg, sizeof(mfg), nullptr);

        if (product[0])
        {
            map[key] = { wideToUtf8(product), wideToUtf8(mfg) };
            // Persist to name cache so the name survives USB redirection
            // (when the device leaves the PnP tree).
            s_nameCache[key] = map[key];
        }
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return map;
}

// ── USB 设备 → 已挂载盘符 映射 ───────────────────────────────────────
//
// 返回：(VID << 16 | PID) → 该 USB 设备上已挂载卷的盘符列表（大写）。
//
// 只收录 FreeRDP rdpdr 客户端“确实会重定向”的卷，判据与官方
// rdpdr_main.c 的 check_path() 保持一致：
//   GetDriveType ∈ {FIXED, REMOVABLE, CDROM} 且 GetVolumeInformation 成功。
// 因此：
//   * 未格式化 / 加密锁定的卷 → 读不到卷信息 → 不在结果里，
//     对应 USB 设备保留透传选项（不会被误置灰）；
//   * 无盘符设备（MTP/PTP 手机、U 盾、加密狗、空读卡器）→ 不在结果里。
// 网络盘（DRIVE_REMOTE）没有本地磁盘号，与 USB 设备无从关联，直接跳过。
static std::map<uint32_t, std::vector<char>> buildUsbVolumeMap_Win32()
{
    std::map<uint32_t, std::vector<char>> result;

    // {53f56307-b6bf-11d0-94f2-00a0c91efb8b} — GUID_DEVINTERFACE_DISK
    static const GUID guidDevInterfaceDisk = {
        0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b }
    };

    // ── 1. 磁盘号 → (VID, PID)：沿 PnP 父设备链上溯到 USB 节点 ──
    std::map<DWORD, uint32_t> diskToId;

    HDEVINFO diskInfo = SetupDiGetClassDevsW(
        &guidDevInterfaceDisk, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (diskInfo != INVALID_HANDLE_VALUE)
    {
        SP_DEVICE_INTERFACE_DATA ifaceData;
        ifaceData.cbSize = sizeof(ifaceData);

        for (DWORD i = 0;
             SetupDiEnumDeviceInterfaces(diskInfo, nullptr, &guidDevInterfaceDisk, i, &ifaceData);
             i++)
        {
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(diskInfo, &ifaceData, nullptr, 0, &needed, nullptr);
            if (!needed)
                continue;

            std::vector<BYTE> buffer(needed);
            auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            SP_DEVINFO_DATA devInfo;
            devInfo.cbSize = sizeof(devInfo);
            if (!SetupDiGetDeviceInterfaceDetailW(diskInfo, &ifaceData, detail, needed, nullptr,
                                                  &devInfo))
                continue;

            // 打开磁盘接口取磁盘号（PartitionNumber == 0 表示整盘）
            HANDLE hDisk = CreateFileW(detail->DevicePath, 0,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, 0, nullptr);
            if (hDisk == INVALID_HANDLE_VALUE)
                continue;

            STORAGE_DEVICE_NUMBER sdn = {};
            DWORD bytes = 0;
            const BOOL ok = DeviceIoControl(hDisk, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                                            &sdn, sizeof(sdn), &bytes, nullptr);
            CloseHandle(hDisk);
            if (!ok)
                continue;

            // 从磁盘 PDO 沿父链上溯，第一个带 VID_/PID_ 的节点即 USB 设备
            // （USBSTOR\Disk... 的父节点是 USB\VID_xxxx&PID_xxxx\...）
            DEVINST devInst = devInfo.DevInst;
            uint32_t vidpid = 0;
            for (int depth = 0; depth < 8; depth++)
            {
                DEVINST parent = 0;
                if (CM_Get_Parent(&parent, devInst, 0) != CR_SUCCESS)
                    break;
                devInst = parent;

                WCHAR instanceId[MAX_DEVICE_ID_LEN] = {};
                if (CM_Get_Device_IDW(devInst, instanceId, ARRAYSIZE(instanceId), 0) != CR_SUCCESS)
                    break;

                unsigned vid = 0, pid = 0;
                const wchar_t* vp = wcsstr(instanceId, L"VID_");
                const wchar_t* pp = wcsstr(instanceId, L"PID_");
                if (vp && pp && swscanf_s(vp + 4, L"%x", &vid) >= 1 &&
                    swscanf_s(pp + 4, L"%x", &pid) >= 1)
                {
                    vidpid = ((uint32_t)(uint16_t)vid << 16) | (uint16_t)pid;
                    break;
                }
            }

            if (vidpid)
                diskToId[sdn.DeviceNumber] = vidpid;
        }

        SetupDiDestroyDeviceInfoList(diskInfo);
    }

    if (diskToId.empty())
        return result;

    // ── 2. 盘符 → 磁盘号 → (VID, PID) ──
    const DWORD len = GetLogicalDriveStringsW(0, nullptr);
    if (len == 0)
        return result;

    std::vector<WCHAR> roots(static_cast<size_t>(len) + 1, L'\0');
    if (GetLogicalDriveStringsW(len + 1, roots.data()) == 0)
        return result;

    for (const WCHAR* root = roots.data(); *root; root += wcslen(root) + 1)
    {
        // 网络盘无本地磁盘号，与 USB 设备无关
        if (GetDriveTypeW(root) == DRIVE_REMOTE)
            continue;

        WCHAR volumeName[MAX_PATH] = {};
        if (!GetVolumeNameForVolumeMountPointW(root, volumeName, ARRAYSIZE(volumeName)))
            continue;

        // CreateFileW 不接受卷名的尾随反斜杠
        std::wstring volumePath = volumeName;
        if (!volumePath.empty() && volumePath.back() == L'\\')
            volumePath.pop_back();

        HANDLE hVol = CreateFileW(volumePath.c_str(), 0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (hVol == INVALID_HANDLE_VALUE)
            continue;

        STORAGE_DEVICE_NUMBER sdn = {};
        DWORD bytes = 0;
        const BOOL ok = DeviceIoControl(hVol, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                                        &sdn, sizeof(sdn), &bytes, nullptr);
        CloseHandle(hVol);
        if (!ok)
            continue;

        const auto it = diskToId.find(sdn.DeviceNumber);
        if (it == diskToId.end())
            continue; // 不是 USB 磁盘

        // 官方 check_path() 要求能读到卷信息，否则不会重定向。
        // 未格式化 / 加密锁定的卷在此被排除 → 保留 USB 透传选项。
        if (!GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, nullptr, 0))
            continue;

        char letter = static_cast<char>(root[0]);
        if (letter >= 'a' && letter <= 'z')
            letter = static_cast<char>(letter - 'a' + 'A');

        auto& letters = result[it->second];
        if (std::find(letters.begin(), letters.end(), letter) == letters.end())
            letters.push_back(letter);
    }

    for (auto& entry : result)
        std::sort(entry.second.begin(), entry.second.end());

    return result;
}

} // anonymous namespace
#endif

USBManager::USBManager(QObject* parent)
	: QObject(parent)
{
	// Initialize libusb
	int rc = libusb_init(&m_ctx);
	if (rc != LIBUSB_SUCCESS)
	{
		qf::log::error("usb/init", "libusb_init failed: {}", libusb_error_name(rc));
		m_ctx = nullptr;
		return;
	}

#if LIBUSB_API_VERSION >= 0x01000102
	libusb_set_option(m_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
#else
	libusb_set_debug(m_ctx, LIBUSB_LOG_LEVEL_WARNING);
#endif

#ifdef _WIN32
#if LIBUSB_API_VERSION >= 0x01000106
	// Enable UsbDk backend on Windows if available (libusb >= 1.0.22)
	// Without this, libusb_open() cannot access USB devices even when
	// UsbDk driver is installed.
	int usbdk_rc = libusb_set_option(m_ctx, LIBUSB_OPTION_USE_USBDK);
	if (usbdk_rc == LIBUSB_SUCCESS)
		qf::log::info("usb/init", "UsbDk backend enabled");
	else if (usbdk_rc == LIBUSB_ERROR_NOT_FOUND)
		qf::log::warn("usb/init", "UsbDk not installed, using default backend");
	else
		qf::log::warn("usb/init", "UsbDk backend init: {} [{}]",
			      libusb_error_name(usbdk_rc), usbdk_rc);
#endif
#endif

	qf::log::info("usb/init", "libusb initialized");
	startHotplugThread();
}

USBManager::~USBManager()
{
	stopHotplugThread();
	if (m_ctx)
	{
		libusb_exit(m_ctx);
		m_ctx = nullptr;
	}
}

// ====================================================================
// Hotplug
// ====================================================================

int LIBUSB_CALL USBManager::hotplugCallback(libusb_context* /*ctx*/,
                                            libusb_device* /*dev*/,
                                            libusb_hotplug_event /*event*/,
                                            void* userdata)
{
	auto* self = static_cast<USBManager*>(userdata);
	// Queue a re-enumerate on the Qt main thread
	QMetaObject::invokeMethod(self, "onHotplugEvent", Qt::QueuedConnection);
	return 0; // keep callback registered
}

void USBManager::startHotplugThread()
{
	if (!m_ctx)
		return;

	// Register hotplug callback for device arrival + removal
	int rc = libusb_hotplug_register_callback(
		m_ctx,
		static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
		                                   LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
		LIBUSB_HOTPLUG_NO_FLAGS,
		LIBUSB_HOTPLUG_MATCH_ANY, // vid
		LIBUSB_HOTPLUG_MATCH_ANY, // pid
		LIBUSB_HOTPLUG_MATCH_ANY, // dev_class
		USBManager::hotplugCallback,
		this,
		&m_hotplugHandle);

	if (rc != LIBUSB_SUCCESS)
	{
		qf::log::warn("usb/hotplug", "hotplug registration failed: {}",
		              libusb_error_name(rc));
		return;
	}

	qf::log::info("usb/hotplug", "hotplug callback registered");

	// Start a dedicated event thread so hotplug callbacks actually fire
	m_stop = false;
	m_eventThread = std::thread([this]() {
		while (!m_stop.load(std::memory_order_relaxed))
		{
			// libusb_handle_events_completed blocks until an event occurs,
			// then returns 0. It returns 1 when the context is about to be
			// destroyed.
			struct timeval tv = { 1, 0 }; // 1 second timeout
			int rc = libusb_handle_events_timeout_completed(m_ctx, &tv, nullptr);
			if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED)
			{
				// Context destroyed or other fatal error
				break;
			}
		}
	});
}

void USBManager::stopHotplugThread()
{
	m_stop.store(true, std::memory_order_relaxed);
	if (m_eventThread.joinable())
		m_eventThread.join();

	if (m_hotplugHandle && m_ctx)
	{
		libusb_hotplug_deregister_callback(m_ctx, m_hotplugHandle);
		m_hotplugHandle = {};
	}
}

void USBManager::onHotplugEvent()
{
	qf::log::info("usb/hotplug", "device change detected, re-enumerating");
	enumerate();
}

// ====================================================================
// Enumeration
// ====================================================================

bool USBManager::shouldShowDevice(const libusb_device_descriptor& desc, libusb_device* dev,
                                  UsbInterfaceInfo* info) const
{
	if (info)
		*info = UsbInterfaceInfo{};

	// Always skip USB hubs
	if (desc.bDeviceClass == 0x09)
		return false;

	// Skip wireless/Bluetooth controllers
	if (desc.bDeviceClass == 0xE0)
		return false;

	libusb_config_descriptor* config = nullptr;
	if (libusb_get_active_config_descriptor(dev, &config) != 0 || !config)
	{
		// 读不到接口描述符（权限/后端限制）时保守保留：宁可多显示一个，
		// 也不要因为读不到描述符把加密狗这类设备整支漏掉。
		// 同时标记为非纯存储，避免被磁盘重定向规则整体置灰。
		if (info)
			info->hasNonStorage = true;
		return true;
	}

	size_t ifaceCount = 0;
	bool allHidOrAudio = true;
	bool hasStorage = false;
	bool hasNonStorage = false;

	for (int i = 0; i < static_cast<int>(config->bNumInterfaces); i++)
	{
		const auto* iface = &config->interface[i];
		if (iface->num_altsetting <= 0)
			continue;

		// 只有第一个 altsetting 代表该接口的类别
		const uint8_t cls = iface->altsetting[0].bInterfaceClass;
		ifaceCount++;

		if (cls == 0x08) // Mass Storage
			hasStorage = true;
		else
			hasNonStorage = true;

		// 只要出现 HID/Audio 之外的接口（如厂商自定义 0xFF），
		// 就不能整支设备隐藏——带 HID 子接口的加密狗依赖这一点。
		if (cls != 0x01 && cls != 0x03)
			allHidOrAudio = false;
	}

	libusb_free_config_descriptor(config);

	if (info)
	{
		info->hasStorage = hasStorage;
		info->hasNonStorage = hasNonStorage;
	}

	// 仅当“所有接口都是 HID/Audio”时才隐藏：
	// 键盘、鼠标、耳机等仍被过滤，带厂商自定义接口的设备保留。
	if (ifaceCount > 0 && allHidOrAudio)
		return false;

	return true;
}

void USBManager::enumerateInternal()
{
	if (!m_ctx)
		return;

	m_devices.clear();

	libusb_device** list = nullptr;
	ssize_t count = libusb_get_device_list(m_ctx, &list);
	if (count < 0)
		return;

#ifdef _WIN32
	// Build fast device-name lookup map from Windows PnP tree.
	// Done once before the loop so each device can look up its name
	// without calling slow libusb_open() + string descriptor reads.
	auto nameMap = buildUsbNameMap_Win32();
#endif

	for (ssize_t i = 0; i < count; i++)
	{
		libusb_device* dev = list[i];
		libusb_device_descriptor desc;

		if (libusb_get_device_descriptor(dev, &desc) != 0)
			continue;

		UsbInterfaceInfo ifInfo;
		if (!shouldShowDevice(desc, dev, &ifInfo))
			continue;

		DeviceInfo info;
		info.vid = desc.idVendor;
		info.pid = desc.idProduct;
		info.bus = libusb_get_bus_number(dev);
		info.addr = libusb_get_device_address(dev);
		info.hasNonStorageInterface = ifInfo.hasNonStorage;

#ifdef _WIN32
		// Fast path: look up device name from the Windows PnP name map.
		// No libusb_open() needed — avoids 200-400ms per device.
		{
			uint32_t key = ((uint32_t)info.vid << 16) | info.pid;
			auto it = nameMap.find(key);
			if (it != nameMap.end())
			{
				info.product = it->second.first;
				info.manufacturer = it->second.second;
			}
			else
			{
				// Device not in current PnP tree (e.g. already redirected).
				// Fall back to name cache so the label doesn't change to
				// "USB Device (VID:PID)" after redirection.
				auto cacheIt = s_nameCache.find(key);
				if (cacheIt != s_nameCache.end())
				{
					info.product = cacheIt->second.first;
					info.manufacturer = cacheIt->second.second;
				}
			}
		}
#else
		// Raw libusb calls without SEH (non-Windows platform)
		{
			libusb_device_handle* handle = nullptr;
			if (libusb_open(dev, &handle) == 0)
			{
				if (desc.iManufacturer)
				{
					char buf[256] = {};
					int len = libusb_get_string_descriptor_ascii(
						handle, desc.iManufacturer,
						reinterpret_cast<unsigned char*>(buf), sizeof(buf));
					if (len > 0)
						info.manufacturer.assign(buf, static_cast<size_t>(len));
				}

				if (desc.iProduct)
				{
					char buf[256] = {};
					int len = libusb_get_string_descriptor_ascii(
						handle, desc.iProduct,
						reinterpret_cast<unsigned char*>(buf), sizeof(buf));
					if (len > 0)
						info.product.assign(buf, static_cast<size_t>(len));
				}

				libusb_close(handle);
			}
		}
#endif

		// Restore selection state if this device was previously selected
		auto key = qMakePair(info.vid, info.pid);
		if (m_selectedIds.contains(key))
		{
			info.selected = true;
		}

		m_devices.push_back(std::move(info));
	}

	libusb_free_device_list(list, 1);

	// 重建“USB 设备 ↔ 盘符”映射：已纳入磁盘重定向的设备需置灰
	applyDiskRedirectMap();

	qf::log::info("usb/enum", "found {} USB device(s) after filtering",
	              m_devices.size());
}

// ====================================================================
// 磁盘重定向 (rdpdr drive) —— USB 设备 ↔ 盘符 映射
// ====================================================================

// 重建每个设备的 diskRedirected / driveLetters 状态。
// 调用者必须已持有 m_mutex。
void USBManager::applyDiskRedirectMap()
{
#ifdef _WIN32
	const std::map<uint32_t, std::vector<char>> volumeMap = buildUsbVolumeMap_Win32();
#else
	const std::map<uint32_t, std::vector<char>> volumeMap;
#endif

	// 统计当前列表中同 VID/PID 的设备数量：>1 说明无法区分是哪一支
	// （例如两支同型号 U 盘），此时一律保留 USB 透传选项。
	std::map<uint32_t, int> idCount;
	for (const auto& d : m_devices)
		idCount[((uint32_t)d.vid << 16) | d.pid]++;

	for (auto& d : m_devices)
	{
		d.diskRedirected = false;
		d.driveLetters.clear();
		d.storageComposite = false;

		const uint32_t key = ((uint32_t)d.vid << 16) | d.pid;
		const auto it = volumeMap.find(key);
		if (it == volumeMap.end() || it->second.empty())
			continue; // 无盘符 / 未格式化 / 加密盘 → 保留 USB 透传

		// 复合设备（存储接口之外还有自定义/HID 等接口）不整体置灰：
		// 只把“纯 U 盘/移动硬盘”交给磁盘重定向，其余一律保留 USB 透传选项。
		// 注意 urbdrc 是整设备透传，两个选项同时生效时 VM 内可能出现两份，
		// 因此 UI 会提示用户，且默认不勾选。
		if (d.hasNonStorageInterface)
		{
			d.storageComposite = true;
			qf::log::info("usb/disk-redirect",
			              "{:04x}:{:04x} is a composite device (has non-storage "
			              "interface), keeping USB passthrough",
			              d.vid, d.pid);
			continue;
		}

		if (idCount[key] > 1)
		{
			qf::log::warn("usb/disk-redirect",
			              "{} USB device(s) share {:04x}:{:04x}, drive mapping is "
			              "ambiguous, keeping USB passthrough",
			              idCount[key], d.vid, d.pid);
			continue;
		}

		std::string letters;
		for (char letter : it->second)
		{
			// 通配模式：所有有盘符的卷都被重定向；
			// 显式模式：只置灰确实列在 /drive: / drivestoredirect 中的盘符。
			if (!m_diskRedirectWildcard && !m_redirectedLetters.contains(QChar::fromLatin1(letter)))
				continue;
			if (!letters.empty())
				letters += ", ";
			letters += letter;
			letters += ':';
		}
		if (letters.empty())
			continue;

		d.diskRedirected = true;
		d.driveLetters = letters;
	}

	// 已置灰的设备不能同时留在 USB 透传选中集合里
	QVector<QPair<uint16_t, uint16_t>> stale;
	for (const auto& key : m_selectedIds)
	{
		for (const auto& d : m_devices)
		{
			if (d.vid == key.first && d.pid == key.second && d.diskRedirected)
			{
				stale.append(key);
				break;
			}
		}
	}
	for (const auto& key : stale)
		m_selectedIds.remove(key);
	for (auto& d : m_devices)
	{
		if (d.diskRedirected)
			d.selected = false;
	}
}

void USBManager::setDiskRedirectState(bool wildcard, const QSet<QChar>& letters)
{
	{
		QMutexLocker lock(&m_mutex);
		m_diskRedirectWildcard = wildcard;
		m_redirectedLetters = letters;
	}

	qf::log::info("usb/disk-redirect",
	              "state updated: wildcard={} explicitLetters={}",
	              wildcard, static_cast<int>(letters.size()));

	// 盘符探测涉及磁盘 I/O，放到后台线程，避免阻塞 RDP 连接建立
	refreshDiskRedirectMap();
}

void USBManager::refreshDiskRedirectMap()
{
	if (m_volumeScanRunning.exchange(true, std::memory_order_acquire))
		return; // 已有扫描在进行

	std::thread([this]() {
		{
			QMutexLocker lock(&m_mutex);
			applyDiskRedirectMap();
		}
		QMetaObject::invokeMethod(this, "onVolumeScanFinished", Qt::QueuedConnection);
	}).detach();
}

void USBManager::onVolumeScanFinished()
{
	m_volumeScanRunning.store(false, std::memory_order_release);
	emit deviceListChanged();
}

bool USBManager::isDiskRedirected(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return false;
	return m_devices[index].diskRedirected;
}

QString USBManager::deviceDriveLetters(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return {};
	return QString::fromStdString(m_devices[index].driveLetters);
}

bool USBManager::isStorageComposite(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return false;
	return m_devices[index].storageComposite;
}

void USBManager::enumerate()
{
	if (m_enumRunning.exchange(true, std::memory_order_acquire))
	{
		// An enumeration is already in progress on a background thread.
		// Since the caller (QML USB button click) already triggered
		// the window to show, there is no need to re-enumerate now.
		return;
	}

	qf::log::info("usb/enum", "starting async enumeration...");

	// Spawn a worker thread so libusb device enumeration does NOT
	// block the Qt main / QML thread.  When the thread finishes it
	// signals back to the main thread via onEnumerationFinished().
	std::thread([this]() {
		{
			QMutexLocker lock(&m_mutex);
			enumerateInternal();
		}
		QMetaObject::invokeMethod(this, "onEnumerationFinished", Qt::QueuedConnection);
	}).detach();
}

void USBManager::onEnumerationFinished()
{
	m_enumRunning.store(false, std::memory_order_release);
	qf::log::info("usb/enum", "async enumeration done, emitting deviceListChanged");
	emit deviceListChanged();
}

// ====================================================================
// QML accessors
// ====================================================================

int USBManager::deviceCount() const
{
	QMutexLocker lock(&m_mutex);
	return static_cast<int>(m_devices.size());
}

QString USBManager::deviceLabel(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return {};

	const auto& d = m_devices[index];
	// Display format: manufacturer + product name.
	// The product field comes from Windows FriendlyName (e.g.
	// "SanDisk Cruzer USB Device") or DeviceDesc. Both are
	// descriptive enough on their own, but we show manufacturer
	// separately when the product string doesn't already include it.
	if (!d.product.empty())
	{
		if (!d.manufacturer.empty())
			return QString::fromStdString(d.manufacturer + " " + d.product);
		return QString::fromStdString(d.product);
	}
	if (!d.manufacturer.empty())
		return QString::fromStdString(d.manufacturer);
	// Fallback: show VID:PID when nothing else is available
	return QStringLiteral("USB Device (%1:%2)")
		.arg(d.vid, 4, 16, QLatin1Char('0'))
		.arg(d.pid, 4, 16, QLatin1Char('0'));
}

QString USBManager::deviceVidPid(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return {};

	const auto& d = m_devices[index];
	return QString("%1:%2")
		.arg(d.vid, 4, 16, QLatin1Char('0'))
		.arg(d.pid, 4, 16, QLatin1Char('0'));
}

int USBManager::deviceState(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return DeviceInfo::Idle;
	return static_cast<int>(m_devices[index].state);
}

QString USBManager::deviceError(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return {};
	return QString::fromStdString(m_devices[index].error);
}

bool USBManager::isDeviceSelected(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return false;
	return m_devices[index].selected;
}

void USBManager::setDeviceSelected(int index, bool selected)
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return;

	auto& d = m_devices[index];
	// 已通过磁盘重定向进入 VM 的设备不能再走 USB 透传（互斥）
	if (d.diskRedirected)
	{
		qf::log::warn("usb/select",
		              "{:04x}:{:04x} is redirected as drive {}, USB passthrough is not allowed",
		              d.vid, d.pid, d.driveLetters);
		return;
	}
	d.selected = selected;
	auto key = qMakePair(d.vid, d.pid);

	if (selected)
		m_selectedIds.insert(key);
	else
		m_selectedIds.remove(key);
}

void USBManager::clearSelection()
{
	QMutexLocker lock(&m_mutex);
	m_selectedIds.clear();
	for (auto& d : m_devices)
	{
		d.selected = false;
		d.state = DeviceInfo::Idle;
		d.error.clear();
	}
	emit deviceListChanged();
}

void USBManager::applySelection()
{
	// Selection is already stored in m_selectedIds.
	// Signal C++ side to trigger a reconnect so my_pre_connect
	// picks up the new device selection.
	emit reconnectRequested();
}

int USBManager::selectedCount() const
{
	QMutexLocker lock(&m_mutex);
	return static_cast<int>(m_selectedIds.size());
}

std::vector<USBManager::SelectedDevice> USBManager::selectedDevices() const
{
	QMutexLocker lock(&m_mutex);
	std::vector<SelectedDevice> out;
	out.reserve(m_selectedIds.size());

	for (const auto& key : m_selectedIds)
	{
		SelectedDevice sd;
		sd.vid = key.first;
		sd.pid = key.second;

		// bus/addr 用于“同型号多支”时改用 addr: 精确指定。
		// 设备已从列表消失（例如已被重定向）时保持 0，调用方会退回 id:。
		for (const auto& d : m_devices)
		{
			if (d.vid == key.first && d.pid == key.second)
			{
				sd.bus = d.bus;
				sd.addr = d.addr;
				break;
			}
		}

		out.push_back(sd);
	}

	return out;
}

void USBManager::markRedirected(uint16_t vid, uint16_t pid, bool success,
                                const std::string& error)
{
	{
		QMutexLocker lock(&m_mutex);
		for (auto& d : m_devices)
		{
			if (d.vid == vid && d.pid == pid)
			{
				d.state = success ? DeviceInfo::Redirected : DeviceInfo::Failed;
				d.error = error;
				break;
			}
		}
	}
	emit deviceListChanged();
}
