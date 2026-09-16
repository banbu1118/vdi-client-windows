#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QPair>
#include <QMutex>
#include <QSet>
#include <atomic>
#include <thread>
#include <memory>
#include <libusb-1.0/libusb.h>

class USBManager : public QObject
{
	Q_OBJECT
public:
	explicit USBManager(QObject* parent = nullptr);
	~USBManager() override;

	// === QML-invokable interface ===
	Q_INVOKABLE void enumerate();
	Q_INVOKABLE int deviceCount() const;
	Q_INVOKABLE QString deviceLabel(int index) const;
	Q_INVOKABLE QString deviceVidPid(int index) const;
	Q_INVOKABLE int deviceState(int index) const;
	Q_INVOKABLE QString deviceError(int index) const;
	Q_INVOKABLE bool isDeviceSelected(int index) const;
	Q_INVOKABLE void setDeviceSelected(int index, bool selected);
	Q_INVOKABLE void clearSelection();
	Q_INVOKABLE int selectedCount() const;
	Q_INVOKABLE void applySelection();
	// 该设备是否已被磁盘重定向接管（此时 UI 置灰、禁止勾选 USB 透传）
	Q_INVOKABLE bool isDiskRedirected(int index) const;
	// 已重定向的盘符，如 "E:, F:"（未重定向时为空）
	Q_INVOKABLE QString deviceDriveLetters(int index) const;
	// 复合存储设备：既有存储接口，又有其它接口（HID/厂商自定义等）。
	// 这类设备不整体置灰（保留 USB 透传选项），UI 需提示可能与盘符重复出现。
	Q_INVOKABLE bool isStorageComposite(int index) const;

	// === Internal API (called from C++ connection code) ===
	// 已选中的 USB 设备。除 VID:PID 外带上 bus/addr，
	// 用于“同型号多支”时改用 addr:bus:addr 精确指定。
	struct SelectedDevice
	{
		uint16_t vid = 0;
		uint16_t pid = 0;
		uint8_t bus = 0;
		uint8_t addr = 0;
	};
	std::vector<SelectedDevice> selectedDevices() const;

	// 由连接代码同步：本次连接是否启用了磁盘重定向（沿用服务端下发）
	void setDiskRedirectState(bool wildcard, const QSet<QChar>& letters);
	// 卷/盘符变化时只重建“USB 设备 ↔ 盘符”映射（不做完整 libusb 枚举）
	void refreshDiskRedirectMap();

	// Mark a device as redirected (by VID:PID)
	void markRedirected(uint16_t vid, uint16_t pid, bool success,
	                    const std::string& error = {});

signals:
	void deviceListChanged(); // QML re-builds its list model
	void reconnectRequested(); // C++ triggers reconnect

private slots:
	void onHotplugEvent();
	void onEnumerationFinished();
	void onVolumeScanFinished();

private:
	struct DeviceInfo
	{
		uint16_t vid, pid;
		uint8_t bus, addr;
		std::string manufacturer;
		std::string product;
		std::string serial;
		bool selected = false;
		bool diskRedirected = false; // 该设备已通过磁盘重定向进入 VM
		std::string driveLetters;    // 已重定向的盘符，如 "E:, F:"
		// 除存储接口外还存在其它接口（HID/厂商自定义等）→ 不整体置灰
		bool hasNonStorageInterface = false;
		// 既有存储接口又有其它接口，且确实被识别出盘符 → UI 标注用
		bool storageComposite = false;

		enum State : int { Idle = 0, Redirecting = 1, Redirected = 2, Failed = 3 };
		State state = Idle;
		std::string error;
	};

	// 设备接口类别汇总，用于过滤判定与“是否复合设备”判定
	struct UsbInterfaceInfo
	{
		bool hasStorage = false;    // 存在 Mass Storage(0x08) 接口
		bool hasNonStorage = false; // 存在非存储接口
	};

	static int LIBUSB_CALL hotplugCallback(libusb_context* ctx, libusb_device* dev,
	                                       libusb_hotplug_event event, void* userdata);
	void enumerateInternal();
	void applyDiskRedirectMap();
	void startHotplugThread();
	void stopHotplugThread();
	bool shouldShowDevice(const libusb_device_descriptor& desc, libusb_device* dev,
	                      UsbInterfaceInfo* info) const;

	libusb_context* m_ctx = nullptr;
	std::vector<DeviceInfo> m_devices;
	QSet<QPair<uint16_t, uint16_t>> m_selectedIds; // persists across enumerate()
	mutable QMutex m_mutex;
	libusb_hotplug_callback_handle m_hotplugHandle{};
	std::thread m_eventThread;
	std::atomic<bool> m_stop{ false };
	std::atomic<bool> m_enumRunning{ false };
	std::atomic<bool> m_volumeScanRunning{ false };
	bool m_diskRedirectWildcard = false; // 通配模式：所有有盘符的卷都被重定向
	QSet<QChar> m_redirectedLetters;     // 显式模式：被重定向的盘符
};
