#include "usb-manager.h"
#include "qf_log.h"

#include <cstring>
#include <QMetaObject>
#include <map>

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <devpkey.h>
#pragma comment(lib, "setupapi.lib")

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

bool USBManager::shouldShowDevice(const libusb_device_descriptor& desc,
                                  libusb_device* dev) const
{
	// Always skip USB hubs
	if (desc.bDeviceClass == 0x09)
		return false;

	// Skip wireless/Bluetooth controllers
	if (desc.bDeviceClass == 0xE0)
		return false;

	// Skip audio devices (headsets, speakers, microphones)
	if (desc.bDeviceClass == 0x01)
		return false;

	// Check interface descriptors for HID (keyboard, mouse, touchpad)
	// and Audio (headsets, speakers)
	libusb_config_descriptor* config = nullptr;
	if (libusb_get_active_config_descriptor(dev, &config) == 0 && config)
	{
		for (int i = 0; i < static_cast<int>(config->bNumInterfaces); i++)
		{
			const auto* iface = &config->interface[i];
			for (int j = 0; j < iface->num_altsetting; j++)
			{
				if (iface->altsetting[j].bInterfaceClass == 0x03 ||
				    iface->altsetting[j].bInterfaceClass == 0x01)
				{
					// HID (keyboard/mouse/touchpad) or Audio (headset/speaker)
					libusb_free_config_descriptor(config);
					return false;
				}
			}
		}
		libusb_free_config_descriptor(config);
	}

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

		if (!shouldShowDevice(desc, dev))
			continue;

		DeviceInfo info;
		info.vid = desc.idVendor;
		info.pid = desc.idProduct;
		info.bus = libusb_get_bus_number(dev);
		info.addr = libusb_get_device_address(dev);

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

	qf::log::info("usb/enum", "found {} USB device(s) after filtering",
	              m_devices.size());
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

std::vector<std::pair<uint16_t, uint16_t>> USBManager::selectedDeviceIds() const
{
	QMutexLocker lock(&m_mutex);
	std::vector<std::pair<uint16_t, uint16_t>> ids;
	ids.reserve(m_selectedIds.size());
	for (const auto& key : m_selectedIds)
		ids.emplace_back(key.first, key.second);
	return ids;
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
